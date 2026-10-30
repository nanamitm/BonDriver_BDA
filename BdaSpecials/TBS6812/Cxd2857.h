// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace tbs6812 {
using Bytes = std::vector<uint8_t>;
struct I2cBus {
    virtual ~I2cBus() = default;
    virtual void Write(uint8_t address, const Bytes& bytes) = 0;
    virtual Bytes Read(uint8_t address, uint8_t reg, size_t length) = 0;
};

enum class DeliverySystem : unsigned { ISDBT = 0, ISDBS = 1, ISDBS3 = 3 };

// Warm ISDB takeover only. No reset, clock, board-mode or shared-GPIO writes.
class Cxd2857 {
public:
    explicit Cxd2857(I2cBus& bus, unsigned tunerIndex = 0, DeliverySystem system = DeliverySystem::ISDBS3);
    void Adopt(uint32_t frequencyKHz, uint16_t expectedStreamID);
    void Tune(uint32_t frequencyKHz, uint16_t streamID);
    void Restore();
    bool Locked();
    float CNR();
    bool Adopted() const { return adopted_; }
    std::string InspectWarmState();
    void AdoptCached(uint32_t frequencyKHz, uint16_t streamID);
    static uint32_t IntermediateFrequency(long rfKHz, long oscillatorKHz);
    static bool IsS3(long streamID);
    static bool SupportsID(long streamID, DeliverySystem system);
    static bool ValidFrequency(uint32_t frequencyKHz, DeliverySystem system);
private:
    I2cBus& bus_;
    uint8_t tunerAddress_;
    DeliverySystem system_;
    bool S3() const { return system_ == DeliverySystem::ISDBS3; }
    void Output(uint8_t value);
    bool Valid(uint32_t frequency, uint16_t streamID) const;
    bool adopted_ = false, dirty_ = false;
    uint32_t originalFrequency_ = 0;
    Bytes originalStream_;
    uint8_t originalPins_ = 0, originalOutput_ = 0;
    void Bank(uint8_t bank);
    void Put(uint8_t address, uint8_t reg, const Bytes& bytes);
    void Set(uint8_t address, uint8_t reg, uint8_t value);
    void Tuner(uint8_t reg, const Bytes& bytes);
    void TuneRF(uint32_t frequencyKHz);
    void TuneTerrestrialRF(uint32_t frequencyKHz);
    bool Terrestrial() const { return system_ == DeliverySystem::ISDBT; }
    void Acquire(const Bytes& stream, uint32_t frequencyKHz);
};
}
