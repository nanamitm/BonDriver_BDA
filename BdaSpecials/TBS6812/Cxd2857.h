// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>

namespace tbs6812 {
using Bytes = std::vector<uint8_t>;
struct I2cBus {
    virtual ~I2cBus() = default;
    virtual void Write(uint8_t address, const Bytes& bytes) = 0;
    virtual Bytes Read(uint8_t address, uint8_t reg, size_t length) = 0;
};

// Warm ISDB-S3 takeover only. No reset, clock, board-mode or shared-GPIO writes.
class Cxd2857 {
public:
    explicit Cxd2857(I2cBus& bus) : bus_(bus) {}
    void Adopt(uint32_t frequencyKHz, uint16_t expectedStreamID);
    void Tune(uint32_t frequencyKHz, uint16_t streamID);
    void Restore();
    bool Locked();
    float CNR();
    bool Adopted() const { return adopted_; }
    static uint32_t IntermediateFrequency(long rfKHz, long oscillatorKHz);
    static bool IsS3(long streamID);
private:
    I2cBus& bus_;
    bool adopted_ = false, dirty_ = false;
    uint32_t originalFrequency_ = 0;
    Bytes originalStream_;
    uint8_t originalPins_ = 0, originalOutput_ = 0;
    void Bank(uint8_t bank);
    void Put(uint8_t address, uint8_t reg, const Bytes& bytes);
    void Set(uint8_t address, uint8_t reg, uint8_t value);
    void Tuner(uint8_t reg, const Bytes& bytes);
    void TuneRF(uint32_t frequencyKHz);
    void Acquire(const Bytes& stream, uint32_t frequencyKHz);
};
}
