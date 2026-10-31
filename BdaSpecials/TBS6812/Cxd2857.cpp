// Copyright (c) Sony; TurboSight; 2026 Till0196
// C++ adaptation of coax CXD2857ER satellite/terrestrial tuning and monitors.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Cxd2857.h"
#include "CnrTable.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <sstream>

namespace tbs6812 {
constexpr uint8_t SLVT = 0x64, SLVX = 0x66;
Cxd2857::Cxd2857(I2cBus& bus, unsigned tunerIndex, DeliverySystem system) : bus_(bus), tunerAddress_(uint8_t(0x60 + tunerIndex)), system_(system) {
    if (tunerIndex > 1) throw std::invalid_argument("TBS6812 tuner index must be 0 or 1");
}
void Cxd2857::Put(uint8_t address, uint8_t reg, const Bytes& bytes) {
    if(captureTransition_ && (address==SLVT || address==SLVX) && reg!=0) Remember(address,reg,bytes.size());
    Bytes request{reg}; request.insert(request.end(), bytes.begin(), bytes.end());
    bus_.Write(address, request);
    if(reg==0 && !bytes.empty()) {
        if(address==SLVT) slvtBank_=bytes[0];
        if(address==SLVX) slvxBank_=bytes[0];
    }
}
void Cxd2857::Remember(uint8_t address, uint8_t reg, size_t length) {
    const auto bank=address==SLVT ? slvtBank_ : slvxBank_;
    for(size_t i=0;i<length;++i) {
        const auto one=uint8_t(reg+i);
        if(address==SLVT && bank==0 && one==0xfe) continue;
        const auto found=std::find_if(transitionRegisters_.begin(),transitionRegisters_.end(),[&](const SavedRegister& r){return r.address==address && r.bank==bank && r.reg==one;});
        if(found==transitionRegisters_.end()) transitionRegisters_.push_back({address,bank,one,bus_.Read(address,one,1)[0]});
    }
}
void Cxd2857::Set(uint8_t address, uint8_t reg, uint8_t value) { Put(address, reg, {value}); }
void Cxd2857::Mask(uint8_t address, uint8_t reg, uint8_t value, uint8_t mask) {
    const auto previous=bus_.Read(address,reg,1)[0];
    Set(address,reg,uint8_t((previous & ~mask) | (value & mask)));
}
void Cxd2857::Bank(uint8_t bank) { Set(SLVT, 0, bank); }
void Cxd2857::Tuner(uint8_t reg, const Bytes& bytes) {
    Set(SLVX, 0, 0); Set(SLVX, 8, 1); Put(tunerAddress_, reg, bytes);
}
bool Cxd2857::IsS3(long id) { return id >= 0 && id <= 65535 && ((id >> 12) == 0xb || (id >> 12) == 0xc); }
bool Cxd2857::SupportsID(long id, DeliverySystem system) {
    if (system == DeliverySystem::ISDBT || system == DeliverySystem::J83B) return id == 0; // TSID is not a terrestrial tuning selector.
    return id >= 0 && id <= 65535 && (system == DeliverySystem::ISDBS3 ? IsS3(id) : !IsS3(id));
}
bool Cxd2857::ValidFrequency(uint32_t frequency, DeliverySystem system) {
    if (system == DeliverySystem::ISDBT || system == DeliverySystem::J83B) return frequency >= 90000 && frequency <= 770000;
    return frequency >= 950000 && frequency <= (system == DeliverySystem::ISDBS3 ? 3224000U : 2150000U);
}
bool Cxd2857::Valid(uint32_t frequency, uint16_t id) const {
    return SupportsID(id, system_) && ValidFrequency(frequency, system_);
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
    if (bus_.Read(SLVX, 0x17, 1)[0] != (Terrestrial() ? 0x06 : S3() ? 0x0d : 0x0c)) throw std::runtime_error("vendor preparation did not select the configured ISDB core");
    if (bus_.Read(SLVX, 0x14, 1)[0] != 1) throw std::runtime_error("unexpected demodulator reference clock; expected 24 MHz");
    Bank(0);
    if ((bus_.Read(SLVT, 0xa9, 1)[0] & 3) != (S3() ? 1 : 0)) throw std::runtime_error("vendor preparation did not select the configured TS/TLV output");
    if (bus_.Read(SLVT, 0xc4, 1)[0] & 0x80) throw std::runtime_error("unexpected serial TS wiring");
    originalPins_ = bus_.Read(SLVT, 0x81, 1)[0];
    Bank(S3() ? 1 : 0); originalOutput_ = bus_.Read(SLVT, S3() ? 0xc0 : 0xc3, 1)[0];
    if (Terrestrial()) {
        Bank(0x10);
        if ((bus_.Read(SLVT,0xd7,1)[0] & 7) != 4) throw std::runtime_error("ISDB-T requires vendor-prepared 6 MHz bandwidth");
        // Keep the vendor-established nominal rate, IF and equalizer coefficients.
        originalStream_.clear();
    } else {
        Bank(S3() ? 0xd0 : 0xc0); originalStream_ = bus_.Read(SLVT, S3() ? 0x87 : 0xe9, S3() ? 2 : 3);
        if ((uint16_t(originalStream_[0]) << 8 | originalStream_[1]) != expectedStreamID || (!S3() && (originalStream_[2] & 1) != (expectedStreamID < 8 ? 1 : 0))) throw std::runtime_error("vendor preparation selected a different stream ID or selection mode");
    }
    originalFrequency_ = frequency; adopted_ = true; adoptedSystem_ = system_;
}
void Cxd2857::TuneTerrestrialRF(uint32_t frequency) {
    // CXD2868ER/Freia ISDB-T 6 MHz, 24 MHz reference. No REFOUT/GPIO writes.
    Tuner(1,{0});
    Tuner(0x74,{0x12,0xf9,0x0f,0x05,0x44});
    Tuner(0x87,{0xc4,0x40});
    Tuner(0x79,{0xa1}); Tuner(0x7d,{0}); Tuner(0x8d,{0}); Tuner(0x8e,{8});
    Tuner(0x91,{0x0a,0x0f}); Tuner(0x9c,{0x90,0});
    Tuner(0x5e,{0xee,2,0x9e,0x67,3,0x38,0x1e,2,0x24});
    Set(SLVX,0,0); Set(SLVX,8,1);
    const auto analog=bus_.Read(tunerAddress_,0x67,1)[0];
    Tuner(0x67,{uint8_t(analog & ~2)});
    Tuner(0x68,{0,uint8_t(cableActive_?0x83:0x87),0,uint8_t(cableActive_?9:0x0d),0x33,0,uint8_t(cableActive_?4:0x17),uint8_t(cableActive_?8:0x1b),
        uint8_t(frequency),uint8_t(frequency>>8),uint8_t((frequency>>16)&0x1f),0xff,0x11});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Tuner(0x88,{0}); Tuner(0x87,{0xc0});
}
void Cxd2857::TuneRF(uint32_t frequency) {
    if (Terrestrial()) { TuneTerrestrialRF(frequency); return; }
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
    if(Terrestrial()) throw std::runtime_error("ISDB-T cached takeover is not supported without RF state verification");
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
    if (!Terrestrial()) { Bank(S3() ? 0xd0 : 0xc0); Put(SLVT, S3() ? 0x87 : 0xe9, stream); }
    TuneRF(frequency);
    Bank(0); Set(SLVT, 0xfe, 1); // soft reacquisition, not chip initialization
    Set(SLVT, 0x81, 0); // drive the parallel data pins
    Output(0);
    Bank(0);
}
void Cxd2857::Tune(uint32_t frequency, uint16_t streamID) {
    if (!adopted_) throw std::runtime_error("demodulator was not adopted");
    if (!Valid(frequency, streamID)) throw std::runtime_error("unsupported ISDB tune parameters");
    dirty_ = true; // even a failed partial transfer requires restoration
    Bytes stream{uint8_t(streamID >> 8), uint8_t(streamID)};
    if (!S3() && !Terrestrial()) stream.push_back(streamID < 8 ? 1 : 0);
    Acquire(stream, frequency);
}
void Cxd2857::BeginJ83B() {
    if(!adopted_ || system_!=DeliverySystem::J83B || cableDirty_) throw std::runtime_error("J.83B requires a fresh adopted ISDB-T bootstrap");
    Set(SLVX,0,0);
    if(bus_.Read(SLVX,0x17,1)[0]!=6) throw std::runtime_error("J.83B takeover requires the active ISDB-T core");
    dirty_=true; cableDirty_=true; captureTransition_=true;
    try {
        // Save also the terrestrial-only wake registers needed when leaving the cable core.
        Bank(0x10); Remember(SLVT,0x9f,5); Remember(SLVT,0xd7,1); Remember(SLVT,0xd9,2);
        Bank(0x12); Remember(SLVT,0x71,1); Bank(0x15); Remember(SLVT,0xbe,1);
        Bank(0x60); Remember(SLVT,0xa8,2); Bank(0x40); Remember(SLVT,0xc3,1);
        Output(1); Bank(0); Mask(SLVT,0x80,0x1f,0x1f); Set(SLVT,0x81,0xff);
        // ISDB-T active-to-sleep, without chip reset or tuner REFOUT changes.
        Bank(0x10);
        for(const auto& p : std::vector<Bytes>{{0x69,5},{0x6b,7},{0x9d,0x14},{0xd3,0},{0xed,1},{0xe2,0x4e},{0xf2,3},{0xde,0x32}}) Set(SLVT,p[0],p[1]);
        Bank(0x15); Set(SLVT,0xde,3); Bank(0x17); Put(SLVT,0x38,{1,2});
        Bank(0x1e); Set(SLVT,0x73,0); Bank(0x63); Set(SLVT,0x81,1);
        Set(SLVX,0,0); Set(SLVX,0x18,1);
        Bank(0); Set(SLVT,0x49,0x33); Set(SLVT,0x4b,0x21); Set(SLVT,0xfe,1);
        Set(SLVT,0x2c,0); Set(SLVT,0xa9,0); Set(SLVX,0x17,1);
        // Per-demodulator TS interface clock. The shared RF reference is untouched.
        Mask(SLVT,0xd3,0,1); Mask(SLVT,0xde,0,1); Mask(SLVT,0xda,0,1);
        Set(SLVT,0xd9,8); Mask(SLVT,0x32,0,1); Mask(SLVT,0x33,1,3); Mask(SLVT,0x32,1,1);
        Bank(0x10); Mask(SLVT,0x66,1,1); Bank(0x40); Mask(SLVT,0x66,1,1);
        // SLtoACC: J.83 Annex B core 0x08, Japan optical 5.6 Msym/s only.
        Set(SLVX,0x17,8); Bank(0); Set(SLVT,0xa9,0); Set(SLVT,0x2c,1);
        Set(SLVT,0x4b,0x74); Set(SLVT,0x49,0); Set(SLVX,0x18,0);
        Bank(0x11); Set(SLVT,0x6a,0x48); Bank(0x10); Set(SLVT,0xa5,1);
        Bank(0x40); Set(SLVT,0x21,4); Set(SLVT,0xb3,5); Set(SLVT,0x1e,0x80); Set(SLVT,0x8e,0x58);
        Bank(0x41); Set(SLVT,0xcf,0x99); Bank(0x11); Set(SLVT,0xa3,0x14); Put(SLVT,0x33,{0,3,0x3b});
        Bank(0); Put(SLVT,0xce,{1,1});
        Bank(0x10); Put(SLVT,0xa6,{0x31,0xa8,0x29,0x9b,0x27,0x9c,0x28,0x9e,0x29,0xa4,0x29,0xa2,0x29,0xa8});
        Put(SLVT,0xb6,{0x14,0,0}); // 3.75 MHz / 48 MHz in 24-bit fixed point
        Bank(0x40); Set(SLVT,0x14,0x10); Put(SLVT,0x26,{7,0x77,7,0x77});
        Bank(0); Mask(SLVT,0x80,8,0x1f); Set(SLVT,0x81,0);
        cableActive_=true; captureTransition_=false;
    } catch(...) { captureTransition_=false; throw; }
}
void Cxd2857::WakeTerrestrial() {
    // J.83B teardown and sleep epilogue; no cold chip initialization.
    Bank(0x11); Set(SLVT,0xa3,0); Bank(0x40);
    for(const auto& p : std::vector<Bytes>{{0x21,0},{0xc3,0},{0xb3,2},{0x1e,0},{0x8e,0x0e},{0x14,0x1f}}) Set(SLVT,p[0],p[1]);
    Put(SLVT,0x26,{9,0x9a,0,0xee}); Bank(0x41); Set(SLVT,0xcf,0x77);
    Set(SLVX,0,0); Set(SLVX,0x18,1); Bank(0);
    Set(SLVT,0x49,0x33); Set(SLVT,0x4b,0x21); Set(SLVT,0xfe,1);
    Set(SLVT,0x2c,0); Set(SLVT,0xa9,0); Set(SLVX,0x17,1);
    Mask(SLVT,0xd3,1,1); Mask(SLVT,0xde,0,1); Mask(SLVT,0xda,0,1);
    Set(SLVT,0xd9,8); Mask(SLVT,0x32,0,1); Mask(SLVT,0x33,1,3); Mask(SLVT,0x32,1,1);
    Bank(0x10); Mask(SLVT,0x66,1,1); Bank(0x40); Mask(SLVT,0x66,1,1);
    // SLtoAIT and fixed 6 MHz band setting.
    Set(SLVX,0x17,6); Bank(0); Set(SLVT,0xa9,0); Set(SLVT,0x2c,1);
    Set(SLVT,0x4b,0x74); Set(SLVT,0x49,0); Set(SLVX,0x18,0);
    Bank(0x11); Set(SLVT,0x6a,0x50); Bank(0x10); Set(SLVT,0xa5,1);
    Bank(0); Put(SLVT,0xce,{0,0}); Bank(0x10);
    for(const auto& p : std::vector<Bytes>{{0x69,4},{0x6b,3},{0x9d,0x50},{0xd3,6},{0xed,0},{0xe2,0xce},{0xf2,0x13},{0xde,0x2e}}) Set(SLVT,p[0],p[1]);
    Bank(0x15); Set(SLVT,0xde,2); Bank(0x17); Put(SLVT,0x38,{0,3});
    Bank(0x1e); Set(SLVT,0x73,0x68); Bank(0x63); Set(SLVT,0x81,0);
    Bank(0x11); Put(SLVT,0x33,{0,3,0x3b}); Bank(0x60); Put(SLVT,0xa8,{0xb7,0x1b});
    Bank(0x10); Put(SLVT,0x9f,{0x17,0xa0,0,0,0});
    Put(SLVT,0xa6,{0x31,0xa8,0x29,0x9b,0x27,0x9c,0x28,0x9e,0x29,0xa4,0x29,0xa2,0x29,0xa8});
    Put(SLVT,0xb6,{0x12,0xee,0xef}); Set(SLVT,0xd7,4); Put(SLVT,0xd9,{0x1f,0x79});
    Bank(0x12); Set(SLVT,0x71,7); Bank(0x15); Set(SLVT,0xbe,2);
    Bank(0); Mask(SLVT,0x80,8,0x1f);
}
void Cxd2857::LeaveCable() {
    captureTransition_=false;
    Output(1); Bank(0); Set(SLVT,0x81,0xff);
    WakeTerrestrial();
    // Reverse the recorded transition, retaining the vendor's exact TS/IF/filter settings.
    for(auto it=transitionRegisters_.rbegin();it!=transitionRegisters_.rend();++it) {
        const auto& r=*it;
        if(r.address==SLVT && r.bank==0 && (r.reg==0x81 || r.reg==0xc3)) continue;
        Set(r.address,0,r.bank); Set(r.address,r.reg,r.value);
    }
    cableActive_=false;
}
void Cxd2857::RestoreTerrestrial() {
    LeaveCable();
    Acquire({},originalFrequency_);
    Bank(0); Set(SLVT,0x81,originalPins_); Output(originalOutput_); Bank(0);
    cableDirty_=false; dirty_=false; transitionRegisters_.clear();
    const auto start=std::chrono::steady_clock::now();
    while(!Locked()) {
        if(std::chrono::steady_clock::now()-start>=std::chrono::seconds(3)) throw std::runtime_error("ISDB-T state restored but bootstrap carrier did not relock");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
void Cxd2857::EndJ83B() {
    if(!adopted_ || !Terrestrial()) throw std::runtime_error("J.83B handback requires an adopted terrestrial core");
    if(cableDirty_) RestoreTerrestrial();
}
// A killed process leaves another core running, and the vendor driver only initializes
// its demodulator when the board mode is written, so its next tune never locks.
bool Cxd2857::RecoverAbandonedCable() {
    if(adopted_) return false;
    Set(SLVX,0,0);
    const auto core=bus_.Read(SLVX,0x17,1)[0];
    const auto wanted=system_;
    const bool foreign=Terrestrial() ? core==8 || core==0x0c || core==0x0d : core==6 || core==8;
    if(!foreign) return false;
    if(core==0x0c || core==0x0d) { system_=core==0x0d ? DeliverySystem::ISDBS3 : DeliverySystem::ISDBS; SleepActiveCore(); }
    else if(core==6) { system_=DeliverySystem::ISDBT; SleepActiveCore(); }
    else {
        system_=DeliverySystem::ISDBT;
        Output(1); Bank(0); Set(SLVT,0x81,0xff);
        WakeTerrestrial();
        if(wanted!=DeliverySystem::ISDBT && wanted!=DeliverySystem::J83B) SleepActiveCore();
    }
    system_=wanted;
    if(Terrestrial()) { if(core!=8) WakeTerrestrial(); }
    else WakeSatellite();
    Bank(0); Set(SLVT,0x81,0);
    Output(0);
    Bank(0);
    return true;
}
void Cxd2857::SleepActiveCore() {
    Output(1); Bank(0); Mask(SLVT,0x80,0x1f,0x1f); Set(SLVT,0x81,0xff);
    if(Terrestrial()) {
        Bank(0x10);
        for(const auto& p : std::vector<Bytes>{{0x69,5},{0x6b,7},{0x9d,0x14},{0xd3,0},{0xed,1},{0xe2,0x4e},{0xf2,3},{0xde,0x32}}) Set(SLVT,p[0],p[1]);
        Bank(0x15); Set(SLVT,0xde,3); Bank(0x17); Put(SLVT,0x38,{1,2});
        Bank(0x1e); Set(SLVT,0x73,0); Bank(0x63); Set(SLVT,0x81,1);
        Set(SLVX,0,0); Set(SLVX,0x18,1);
        Bank(0); Set(SLVT,0x49,0x33); Set(SLVT,0x4b,0x21); Set(SLVT,0xfe,1);
        Set(SLVT,0x2c,0); Set(SLVT,0xa9,0); Set(SLVX,0x17,1);
        return;
    }
    if(S3()) { Bank(0xa3); Put(SLVT,0x43,{0x0a,0x0a}); }
    Set(SLVX,0,0); Set(SLVX,0x18,1);
    Bank(0); Set(SLVT,0x6a,0x11); Set(SLVT,0x4b,0x21); Set(SLVX,0x28,0x13);
    Set(SLVT,0xfe,1); Set(SLVT,0x2c,0); Set(SLVT,0xa9,0); Set(SLVT,0x2d,0); Set(SLVX,0x17,1);
    Bank(0xa0); Set(SLVT,0xd7,0xa0); Bank(0);
}
void Cxd2857::WakeSatellite() {
    if(S3()) {
        // SLtoAIS3 with the parallel TLV interface clock.
        Bank(1); Mask(SLVT,0xc1,0,0x80); Mask(SLVT,0xcf,0,1); Mask(SLVT,0xe7,1,1);
        Bank(0x56); Mask(SLVT,0x83,4,7);
        Bank(0); Mask(SLVT,0x32,0,1); Mask(SLVT,0x33,2,3); Mask(SLVT,0x32,1,1);
        Set(SLVX,0,0); Set(SLVX,0x17,0x0d);
        Bank(0); Set(SLVT,0x2d,0); Set(SLVT,0xa9,1); Set(SLVT,0x2c,1);
        Set(SLVX,0x28,0x31); Set(SLVT,0x4b,0x31); Set(SLVT,0x6a,0); Set(SLVX,0x18,0);
        Bank(0); Set(SLVT,0x20,1);
        Bank(0xa3); Put(SLVT,0x43,{0x0b,0x0b});
        Bank(0xae); Put(SLVT,0x46,{0x04,0x91});
        Bank(0xb6); Set(SLVT,0x74,0x59);
        Bank(0xd5); Put(SLVT,0x61,{0x48,0xfe,0x4e,0x6e,0xfe}); Put(SLVT,0x67,{0x4e,0x6e}); Put(SLVT,0x90,{0x10,0x03,0x10});
        Bank(0xd6); Put(SLVT,0x18,{0x33,0xeb}); Set(SLVT,0x1c,0x59); Set(SLVT,0x1e,0x6e); Put(SLVT,0x21,{0x4e,0x80,0x80,0x10}); Put(SLVT,0x51,{0xb3,0xb3,0xc3});
        Bank(0xda); Set(SLVT,0xbc,2); Put(SLVT,0xd6,{0x60,0x70});
        Bank(0xae); Put(SLVT,0x20,{0x08,0x70,0x64});
        Bank(0xda); Set(SLVT,0xcb,1); Set(SLVT,0xc1,1);
        Bank(0xa0); Set(SLVT,0xd7,0);
        Bank(0); Mask(SLVT,0x80,0x10,0x1f);
        return;
    }
    // SLtoAIS with the parallel TS interface clock the vendor driver selected.
    Bank(0); Mask(SLVT,0xd3,1,1); Mask(SLVT,0xde,0,1); Mask(SLVT,0xda,0,1);
    Set(SLVT,0xd9,8); Mask(SLVT,0x32,0,1); Mask(SLVT,0x33,1,3); Mask(SLVT,0x32,1,1);
    Bank(0x10); Mask(SLVT,0x66,1,1); Bank(0x40); Mask(SLVT,0x66,1,1);
    Set(SLVX,0,0); Set(SLVX,0x17,0x0c);
    Bank(0); Set(SLVT,0x2d,0); Set(SLVT,0xa9,0); Set(SLVT,0x2c,1);
    Set(SLVX,0x28,0x31); Set(SLVT,0x4b,0x31); Set(SLVT,0x6a,0); Set(SLVX,0x18,0);
    Bank(0); Set(SLVT,0x20,1); Put(SLVT,0xce,{0,0});
    Bank(0xae); Put(SLVT,0x20,{7,0x37,0x0a});
    Bank(0xa0); Set(SLVT,0xd7,0);
    Bank(0); Mask(SLVT,0x80,0x10,0x1f);
}
void Cxd2857::SwitchSystem(DeliverySystem system) {
    if(system==system_) return;
    if(!adopted_) throw std::runtime_error("direct switching requires an adopted demodulator");
    dirty_=true;
    if(cableActive_) {
        LeaveCable();
        cableDirty_=false; transitionRegisters_.clear();
        system_=DeliverySystem::ISDBT;
    }
    if(system==DeliverySystem::J83B || system==DeliverySystem::ISDBT) {
        if(system_!=DeliverySystem::ISDBT) {
            SleepActiveCore();
            system_=DeliverySystem::ISDBT;
            WakeTerrestrial();
        }
        Bank(0); Set(SLVT,0x81,0);
        if(system==DeliverySystem::J83B) { system_=system; BeginJ83B(); }
        return;
    }
    if(system_!=system) {
        SleepActiveCore();
        system_=system;
        WakeSatellite();
    }
    Bank(0); Set(SLVT,0x81,0);
}
void Cxd2857::Restore() {
    if(adopted_ && system_!=adoptedSystem_) SwitchSystem(adoptedSystem_);
    if(cableDirty_) { RestoreTerrestrial(); return; }
    if (!adopted_ || !dirty_) return;
    Acquire(originalStream_, originalFrequency_);
    Bank(0); Set(SLVT, 0x81, originalPins_);
    Output(originalOutput_);
    Bank(0); dirty_ = false;
}
bool Cxd2857::Locked() {
    if(cableActive_) {
        Set(SLVX,0,0); if(bus_.Read(SLVX,0x17,1)[0]!=8) return false;
        Bank(0x40); const auto sync=bus_.Read(SLVT,0x88,1)[0];
        return (sync & 1) && (bus_.Read(SLVT,0x10,1)[0] & 0x20);
    }
    if (Terrestrial()) {
        Set(SLVX,0,0); if(bus_.Read(SLVX,0x17,1)[0]!=6) return false;
        Bank(0x60); const auto status=bus_.Read(SLVT,0x10,1)[0];
        return (status & 0x13) == 3; // sync + TS lock, with unlock clear
    }
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
    if (!Terrestrial()) { Bank(S3() ? 0xd0 : 0xc0); show("stream",SLVT,S3() ? 0x87 : 0xe9,S3() ? 2 : 3); }
    Bank(cableActive_ ? 0x40 : Terrestrial() ? 0x60 : 0xa0); show("lock",SLVT,0x10,Terrestrial() ? 1 : S3() ? 2 : 3);
    if(cableActive_) show("qam-sync",SLVT,0x88,1);
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
    if(cableActive_) {
        try {
            Bank(0x40); const auto constellation=bus_.Read(SLVT,0x19,1)[0]&7;
            data=bus_.Read(SLVT,0x4c,2);
            Bank(0); Set(SLVT,1,0);
            const auto raw=((unsigned(data[0])&0x1f)<<8)|data[1];
            if(!raw) throw std::runtime_error("J.83B C/N monitor is not valid yet");
            if(constellation==1 || constellation==3) return float(-8.8*std::log(double(std::max(raw,69U)))+86.999);
            return float(-9.5*std::log(double(std::max(raw,126U)))+95.941);
        } catch(...) { try { Bank(0); Set(SLVT,1,0); } catch(...) {} throw; }
    }
    try { Bank(Terrestrial() ? 0x60 : S3() ? 0xd0 : 0xa1); data = bus_.Read(SLVT, Terrestrial() ? 0x28 : S3() ? 0xf3 : 0x10, Terrestrial() ? 2 : S3() ? 4 : 3); }
    catch (...) { try { Bank(0); Set(SLVT, 1, 0); } catch (...) {} throw; }
    Bank(0); Set(SLVT, 1, 0);
    if (Terrestrial()) {
        const auto raw = (unsigned(data[0]) << 8) | data[1];
        if (!raw) throw std::runtime_error("ISDB-T C/N monitor is not valid yet");
        return float(10.0*std::log10(double(raw))-9.031);
    }
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
