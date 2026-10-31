// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include "SystemRouting.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace tbs6812;
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    const auto path=std::filesystem::temp_directory_path()/(L"tbs6812-routing-"+std::to_wstring(GetCurrentProcessId())+L".ini");
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code error;std::filesystem::remove(path,error);} } cleanup{path};
    const auto write=[&](const char* text){std::ofstream file(path,std::ios::trunc);file<<text;};
    const auto read=[&](){return SystemRouting::Read(path.c_str());};
    try {
        write("");
        Require(read().ForSpace(0)==DeliverySystem::ISDBS3,"absent setting preserves legacy S3 default");
        write("[TBS6812]\nSatelliteSystem=ISDB-S\n");
        Require(read().ForSpace(5)==DeliverySystem::ISDBS,"legacy satellite setting remains fixed across spaces");
        write("[TBS6812]\nSatelliteSystem=ISDB-S\nDeliverySystem=ISDB-T\n");
        Require(read().ForSpace(0)==DeliverySystem::ISDBT,"delivery system overrides legacy setting");
        write("[TBS6812]\nDeliverySystem=J.83B\n");
        Require(!read().combined && read().ForSpace(3)==DeliverySystem::J83B,"fixed optical mode remains supported");
        write("[TBS6812]\nDeliverySystem=ISDB-T/J.83B\n[TuningSpace00]\nDeliverySystem=\"isdb-t\"\n[TuningSpace07]\nDeliverySystem=J.83B\n");
        const auto combined=read();
        Require(combined.combined && combined.ForSpace(0)==DeliverySystem::ISDBT &&
            combined.ForSpace(7)==DeliverySystem::J83B,"actual quoted/case-insensitive INI selects systems by space ID");
        Require(!combined.ForSpace(1) && !combined.ForSpace(0xffffffffU),"unmapped spaces cannot silently select a core");
        write("[TBS6812]\nDeliverySystem=ISDB-T/J.83B/ISDB-S\n[TuningSpace00]\nDeliverySystem=ISDB-T\n[TuningSpace01]\nDeliverySystem=J.83B\n[TuningSpace02]\nDeliverySystem=ISDB-S\n");
        const auto mixed=read();
        Require(mixed.mixed && mixed.combined && mixed.ForSpace(2)==DeliverySystem::ISDBS,"mixed routing maps satellite spaces");
        Require(BoardModeFor(*mixed.ForSpace(0))==BoardModeTerrestrial && BoardModeFor(*mixed.ForSpace(1))==BoardModeTerrestrial &&
            BoardModeFor(*mixed.ForSpace(2))==BoardModeSatellite,"spaces map to the board mode of their family");
        Require(!combined.mixed,"terrestrial combined mode never rewrites the board");
        for(const char* invalid : {
            "[TBS6812]\nDeliverySystem=ISDB-T/J.83B\n[TuningSpace00]\nDeliverySystem=ISDB-S\n",
            "[TBS6812]\nDeliverySystem=ISDB-T/J.83B/ISDB-S\n[TuningSpace00]\nDeliverySystem=ISDB-S3\n",
            "[TBS6812]\nDeliverySystem=Auto\n",
            "[TBS6812]\nDeliverySystem=ISDB-T/J.83B\n",
            "[TBS6812]\nDeliverySystem=ISDB-T/J.83B\n[TuningSpace00]\nDeliverySystem=ISDB-S3\n",
            "[TBS6812]\nDeliverySystem=ISDB-T/J.83B\n[TuningSpace00]\nDeliverySystem=J.83C\n"}) {
            write(invalid); bool rejected=false; try { read(); } catch(const std::exception&) { rejected=true; }
            Require(rejected,"invalid routing must fail before hardware access");
        }
        const auto deployed=SystemRouting::Read(std::filesystem::absolute("config/BonDriver_TBS6812_T.ini").c_str());
        Require(deployed.combined && deployed.ForSpace(0)==DeliverySystem::ISDBT &&
            deployed.ForSpace(1)==DeliverySystem::J83B,"shipped combined template has both systems");
        const auto shipped=SystemRouting::Read(std::filesystem::absolute("config/BonDriver_TBS6812_TS.ini").c_str());
        Require(shipped.mixed && shipped.ForSpace(0)==DeliverySystem::ISDBT && shipped.ForSpace(1)==DeliverySystem::J83B &&
            shipped.ForSpace(2)==DeliverySystem::ISDBS && shipped.ForSpace(3)==DeliverySystem::ISDBS,"shipped mixed template maps all spaces");
        std::cout<<"PASS: INI routing, legacy defaults, combined spaces and invalid/missing mapping guards\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
