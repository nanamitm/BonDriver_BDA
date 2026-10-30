// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "Cxd2857.h"
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace tbs6812 {
struct WarmStartRecord {
    uint32_t frequencyKHz;
    uint16_t streamID;
};
// Experimental, explicitly configured storage. A record is consumed
// before use and only republished after successful normal restoration.
inline uint64_t WarmChecksum(uint64_t device, uint32_t frequency, uint32_t stream) {
    uint64_t hash=14695981039346656037ULL;
    for(uint64_t value : {device,uint64_t(frequency),uint64_t(stream)}) {
        for(int i=0;i<8;++i) { hash^=value&255; hash*=1099511628211ULL; value>>=8; }
    }
    return hash;
}
inline std::optional<WarmStartRecord> ReadWarmRecord(const std::filesystem::path& path, uint64_t device) {
    std::error_code error;
    const auto size=std::filesystem::file_size(path,error);
    if(error || size>256) return {};
    std::ifstream input(path);
    std::string magic,extra; uint64_t savedDevice=0,checksum=0; uint32_t frequency=0,stream=0;
    if(!(input>>magic>>savedDevice>>frequency>>stream>>checksum) || input>>extra || magic!="TBS6812-WARM-1" ||
        savedDevice!=device || frequency<950000 || frequency>3224000 || !Cxd2857::IsS3(stream) ||
        checksum!=WarmChecksum(device,frequency,stream)) return {};
    return WarmStartRecord{frequency,uint16_t(stream)};
}
inline void WriteWarmRecord(const std::filesystem::path& path, uint64_t device, WarmStartRecord record) {
    std::ofstream output(path,std::ios::trunc);
    output<<"TBS6812-WARM-1 "<<device<<' '<<record.frequencyKHz<<' '<<record.streamID<<' '
        <<WarmChecksum(device,record.frequencyKHz,record.streamID)<<'\n';
    output.close();
    if(!output) throw std::runtime_error("cannot write warm-start record");
}
}
