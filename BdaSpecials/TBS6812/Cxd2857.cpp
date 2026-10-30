// Copyright (c) Sony; TurboSight; 2026 Till0196
// C++ adaptation of coax CXD2857ER tuneSatellite and satellite monitors.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Cxd2857.h"
#include "CnrTable.h"
#include <chrono>
#include <stdexcept>
#include <thread>
#include <sstream>

namespace tbs6812 {
constexpr uint8_t SLVT = 0x64, SLVX = 0x66;
Cxd2857::Cxd2857(I2cBus& bus, unsigned tunerIndex, SatelliteSystem system) : bus_(bus), tunerAddress_(uint8_t(0x60 + tunerIndex)), system_(system) {
    if (tunerIndex > 1) throw std::invalid_argument("TBS6812 tuner index must be 0 or 1");
}
void Cxd2857::Put(uint8_t address, uint8_t reg, const Bytes& bytes) {
    Bytes request{reg}; request.insert(request.end(), bytes.begin(), bytes.end());
    bus_.Write(address, request);
}
void Cxd2857::Set(uint8_t address, uint8_t reg, uint8_t value) { Put(address, reg, {value}); }
void Cxd2857::Bank(uint8_t bank) { Set(SLVT, 0, bank); }
void Cxd2857::Tuner(uint8_t reg, const Bytes& bytes) {
    Set(SLVX, 0, 0); Set(SLVX, 8, 1); Put(tunerAddress_, reg, bytes);
}
bool Cxd2857::IsS3(long id) { return id >= 0 && id <= 65535 && ((id >> 12) == 0xb || (id >> 12) == 0xc); }
bool Cxd2857::SupportsID(long id, SatelliteSystem system) {
    return id >= 0 && id <= 65535 && (system == SatelliteSystem::ISDBS3 ? IsS3(id) : !IsS3(id));
}
bool Cxd2857::Valid(uint32_t frequency, uint16_t id) const {
    return SupportsID(id, system_) && frequency >= 950000 && frequency <= (S3() ? 3224000U : 2150000U);
}
void Cxd2857::Output(uint8_t value) {
    Bank(S3() ? 1 : 0); Set(SLVT, S3() ? 0xc0 : 0xc3, value);
}
uint32_t Cxd2857::IntermediateFrequency(long rf, long oscillator) {
    const int64_t value = int64_t(rf) - oscillator;
    if (oscillator <= 0 || value < 950000 || value > 3224000) throw std::runtime_error("satellite IF must be 950..3224 MHz; check LNB oscillator and RF frequency");
    return static_cast<uint32_t>(value);
}
void Cxd2857::Adopt(uint32_t frequency, uint16_t expectedStreamID) {
    if (!Valid(frequency, expectedStreamID)) throw std::runtime_error("invalid bootstrap parameters");
    // Identifying the chip and checking its prepared mode precede tuner writes.
    Set(SLVX, 0, 0);
    const auto high = bus_.Read(SLVX, 0xfb, 1)[0];
    const auto low = bus_.Read(SLVX, 0xfd, 1)[0];
    if (((high & 3) << 8 | low) != 0x091) throw std::runtime_error("CXD2857ER chip ID mismatch");
    if (bus_.Read(SLVX, 0x17, 1)[0] != (S3() ? 0x0d : 0x0c)) throw std::runtime_error("vendor preparation did not select the configured satellite core");
    if (bus_.Read(SLVX, 0x14, 1)[0] != 1) throw std::runtime_error("unexpected demodulator reference clock; expected 24 MHz");
    Bank(0);
    if ((bus_.Read(SLVT, 0xa9, 1)[0] & 3) != (S3() ? 1 : 0)) throw std::runtime_error("vendor preparation did not select the configured TS/TLV output");
    if (bus_.Read(SLVT, 0xc4, 1)[0] & 0x80) throw std::runtime_error("unexpected serial TS wiring");
    originalPins_ = bus_.Read(SLVT, 0x81, 1)[0];
    Bank(S3() ? 1 : 0); originalOutput_ = bus_.Read(SLVT, S3() ? 0xc0 : 0xc3, 1)[0];
    Bank(S3() ? 0xd0 : 0xc0); originalStream_ = bus_.Read(SLVT, S3() ? 0x87 : 0xe9, S3() ? 2 : 3);
    if ((uint16_t(originalStream_[0]) << 8 | originalStream_[1]) != expectedStreamID || (!S3() && (originalStream_[2] & 1) != (expectedStreamID < 8 ? 1 : 0))) throw std::runtime_error("vendor preparation selected a different stream ID or selection mode");
    originalFrequency_ = frequency; adopted_ = true;
}
void Cxd2857::TuneRF(uint32_t frequency) {
    Tuner(0x15, {0x12}); Tuner(0x6a, {0, 0});
    Tuner(0x74, {0x12, 0xf9, 0x0f, 0x25, 0x44});
    constexpr uint8_t prologue[][2]={{0x75,0xf9},{0x40,7},{0x41,7},{0x45,3},{0x48,7}};
    for (const auto& pair : prologue) Tuner(pair[0], {pair[1]});
    // CXD2868ER/Freia, 24 MHz: reference divider 3; filters differ by system.
    Tuner(4, S3() ? Bytes{0xc4, 0x40, 3, 0, 0xb4, 0x78, 8, 0x30}
                  : Bytes{0xc4, 0x40, 3, 0x80, 0x70, 0x1e, 2, 0x24});
    const bool high = S3() && frequency > 2150000;
    if (high) { Tuner(0x45, {2}); Tuner(1, {3}); }
    else { Tuner(0x43, {4}); Tuner(1, {1}); }
    const uint32_t step = (frequency + 2) / 4;
    Tuner(0x0c, {uint8_t(high ? 0xfc : 0xfe), uint8_t(high ? 0x32 : 0x22), 0x9e, 0x16,
        uint8_t(step), uint8_t(step >> 8), uint8_t((step >> 16) & 15), 0xff, 0, 1});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Tuner(5, {0}); Tuner(4, {0xc0});
}
void Cxd2857::AdoptCached(uint32_t frequency, uint16_t streamID) {
    if(!Locked()) throw std::runtime_error("cached hardware is not locked");
    Set(SLVX,0,0);
    if(bus_.Read(SLVX,0x10,1)[0]!=0) throw std::runtime_error("cached demodulator is not active");
    Bank(0);
    if(bus_.Read(SLVT,0x81,1)[0]!=0) throw std::runtime_error("cached data pins are disabled");
    Bank(S3() ? 1 : 0);
    if(bus_.Read(SLVT,S3() ? 0xc0 : 0xc3,1)[0]!=0) throw std::runtime_error("cached stream output is disabled");
    Adopt(frequency,streamID);
}
void Cxd2857::Acquire(const Bytes& stream, uint32_t frequency) {
    Output(1); // stop the configured output while retuning
    Bank(S3() ? 0xd0 : 0xc0); Put(SLVT, S3() ? 0x87 : 0xe9, stream);
    TuneRF(frequency);
    Bank(0); Set(SLVT, 0xfe, 1); // soft reacquisition, not chip initialization
    Set(SLVT, 0x81, 0); // drive the parallel data pins
    Output(0);
    Bank(0);
}
void Cxd2857::Tune(uint32_t frequency, uint16_t streamID) {
    if (!adopted_) throw std::runtime_error("demodulator was not adopted");
    if (!Valid(frequency, streamID)) throw std::runtime_error("unsupported satellite tune parameters");
    dirty_ = true; // even a failed partial transfer requires restoration
    Bytes stream{uint8_t(streamID >> 8), uint8_t(streamID)};
    if (!S3()) stream.push_back(streamID < 8 ? 1 : 0);
    Acquire(stream, frequency);
}
void Cxd2857::Restore() {
    if (!adopted_ || !dirty_) return;
    Acquire(originalStream_, originalFrequency_);
    Bank(0); Set(SLVT, 0x81, originalPins_);
    Output(originalOutput_);
    Bank(0); dirty_ = false;
}
bool Cxd2857::Locked() {
    Bank(0xa0); return (bus_.Read(SLVT, 0x10, S3() ? 2 : 3)[S3() ? 1 : 2] & 0x40) != 0;
}
std::string Cxd2857::InspectWarmState() {
    std::ostringstream result;
    const auto show = [&](const char* name, uint8_t address, uint8_t reg, size_t length) {
        result << name << '=';
        for(uint8_t value : bus_.Read(address, reg, length)) result << std::hex << unsigned(value) << ',';
        result << ' ';
    };
    Set(SLVX,0,0);
    show("id",SLVX,0xfb,3); show("clock/core",SLVX,0x14,4); show("x10",SLVX,0x10,1);
    Bank(0); show("tlv",SLVT,0xa9,1); show("serial",SLVT,0xc4,1); show("pins",SLVT,0x81,1);
    Bank(S3() ? 1 : 0); show("output",SLVT,S3() ? 0xc0 : 0xc3,1);
    Bank(S3() ? 0xd0 : 0xc0); show("stream",SLVT,S3() ? 0x87 : 0xe9,S3() ? 2 : 3);
    Bank(0xa0); show("lock",SLVT,0x10,S3() ? 2 : 3);
    const uint8_t repeater=bus_.Read(SLVX,8,1)[0];
    Set(SLVX,8,repeater|1);
    try {
        show("rf01",tunerAddress_,1,1); show("rf04",tunerAddress_,4,2);
        show("rf0c",tunerAddress_,0x0c,10); show("rf1a",tunerAddress_,0x1a,1);
        show("rf45",tunerAddress_,0x45,1); show("rf81",tunerAddress_,0x81,4);
    } catch(...) { Set(SLVX,8,repeater); Bank(0); throw; }
    Set(SLVX,8,repeater); Bank(0);
    return result.str();
}
float Cxd2857::CNR() {
    Bank(0); Set(SLVT, 1, 1);
    Bytes data;
    try { Bank(S3() ? 0xd0 : 0xa1); data = bus_.Read(SLVT, S3() ? 0xf3 : 0x10, S3() ? 4 : 3); }
    catch (...) { try { Bank(0); Set(SLVT, 1, 0); } catch (...) {} throw; }
    Bank(0); Set(SLVT, 1, 0);
    if (!(data[0] & 1)) throw std::runtime_error("C/N monitor is not valid yet");
    const uint32_t raw = S3() ? ((uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3]) & 0x1ffff
                             : ((uint32_t(data[1]) << 8) | data[2]) & 0x1fff;
    const auto* curve = S3() ? s3Cnr : sCnr;
    const size_t count = S3() ? sizeof(s3Cnr)/sizeof(s3Cnr[0]) : sizeof(sCnr)/sizeof(sCnr[0]);
    size_t index = 1;
    for (; index < count; ++index) if (curve[index].raw < raw) break;
    return curve[index - 1].mdB / 1000.0f;
}
}
