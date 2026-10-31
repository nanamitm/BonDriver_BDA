// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <map>
#include <cwchar>
#include <optional>
#include <stdexcept>
#include "Cxd2857.h"
#include "BoardModeSwitch.h"

namespace tbs6812 {
inline uint8_t BoardModeFor(DeliverySystem system) {
    return system==DeliverySystem::ISDBT || system==DeliverySystem::J83B ? BoardModeTerrestrial : BoardModeSatellite;
}
struct SystemRouting {
    bool combined=false;
    bool mixed=false; // Terrestrial and satellite spaces; each graph serves one board mode.
    DeliverySystem fixed=DeliverySystem::ISDBS3;
    std::map<uint32_t,DeliverySystem> spaces;
    static std::optional<DeliverySystem> Parse(const wchar_t* value) {
        if(!_wcsicmp(value,L"ISDB-T")) return DeliverySystem::ISDBT;
        if(!_wcsicmp(value,L"J.83B")) return DeliverySystem::J83B;
        if(!_wcsicmp(value,L"ISDB-S")) return DeliverySystem::ISDBS;
        if(!_wcsicmp(value,L"ISDB-S3")) return DeliverySystem::ISDBS3;
        return {};
    }
    static SystemRouting Read(const wchar_t* path) {
        SystemRouting result;
        wchar_t legacy[64]{},value[64]{};
        GetPrivateProfileStringW(L"TBS6812",L"SatelliteSystem",L"ISDB-S3",legacy,64,path);
        GetPrivateProfileStringW(L"TBS6812",L"DeliverySystem",legacy,value,64,path);
        const bool withS3=!_wcsicmp(value,L"ISDB-T/J.83B/ISDB-S/ISDB-S3");
        result.mixed=withS3 || !_wcsicmp(value,L"ISDB-T/J.83B/ISDB-S");
        result.combined=result.mixed || !_wcsicmp(value,L"ISDB-T/J.83B");
        if(!result.combined) {
            const auto system=Parse(value);
            if(!system) throw std::runtime_error("DeliverySystem must be ISDB-T, J.83B, ISDB-S, ISDB-S3, ISDB-T/J.83B, ISDB-T/J.83B/ISDB-S or ISDB-T/J.83B/ISDB-S/ISDB-S3");
            result.fixed=*system;
            return result;
        }
        result.fixed=DeliverySystem::ISDBT;
        for(unsigned i=0;i<100;++i) {
            wchar_t section[32]{};
            swprintf_s(section,L"TuningSpace%02u",i);
            GetPrivateProfileStringW(section,L"DeliverySystem",L"",value,64,path);
            if(!*value) continue;
            const auto system=Parse(value);
            if(!system || (*system!=DeliverySystem::ISDBT && *system!=DeliverySystem::J83B && !(result.mixed && *system==DeliverySystem::ISDBS) && !(withS3 && *system==DeliverySystem::ISDBS3)))
                throw std::runtime_error(result.mixed ? "this mixed DeliverySystem does not accept the tuning space's delivery system"
                                                      : "combined terrestrial tuning spaces accept only ISDB-T or J.83B");
            result.spaces.emplace(i,*system);
        }
        if(result.spaces.empty()) throw std::runtime_error("combined mode requires DeliverySystem in each tuning space");
        return result;
    }
    std::optional<DeliverySystem> ForSpace(uint32_t space) const {
        if(!combined) return fixed;
        const auto found=spaces.find(space);
        if(found==spaces.end()) return {};
        return found->second;
    }
};
}
