// SPDX-License-Identifier: GPL-2.0-or-later
#include "Cxd2857.h"
#include "WarmStartCache.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
#include <string>
using namespace tbs6812;
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct FakeBus final : I2cBus {
    std::map<std::tuple<int,int,int>,uint8_t> registers;
    uint8_t banks[256]{};
    std::vector<std::pair<uint8_t,Bytes>> writes;
    bool failNextTuner=false, failMonitor=false, failCableWake=false;
    FakeBus() {
        registers[{0x66,0,0xfb}]=0; registers[{0x66,0,0xfd}]=0x91;
        registers[{0x66,0,0x17}]=0x0d; registers[{0x66,0,0x14}]=1;
        registers[{0x64,0,0xa9}]=1; registers[{0x64,0,0xc4}]=0x29;
        registers[{0x64,0xd0,0x87}]=0xb0; registers[{0x64,0xd0,0x88}]=0x70;
        registers[{0x64,0xa0,0x11}]=0x40;
    }
    void Write(uint8_t address,const Bytes& data) override {
        if((address==0x60 || address==0x61) && failNextTuner) { failNextTuner=false; throw std::runtime_error("injected tuner write failure"); }
        if(failCableWake && address==0x64 && banks[address]==0x40 && data[0]==0x21) { failCableWake=false; throw std::runtime_error("injected cable wake failure"); }
        Require(data.size()>=2,"empty register write"); writes.emplace_back(address,data);
        if(address!=0x60 && address!=0x61 && data[0]==0) { banks[address]=data[1]; return; }
        for(size_t i=1;i<data.size();++i) registers[{address,banks[address],data[0]+int(i)-1}]=data[i];
    }
    Bytes Read(uint8_t address,uint8_t reg,size_t length) override {
        if(failMonitor && address==0x64 && ((banks[address]==0xd0 && reg==0xf3) || (banks[address]==0x60 && reg==0x28) || (banks[address]==0x40 && reg==0x4c))) throw std::runtime_error("injected monitor failure");
        Bytes answer(length); for(size_t i=0;i<length;++i) answer[i]=registers[{address,banks[address],reg+int(i)}]; return answer;
    }
    uint8_t At(int address,int bank,int reg) { return registers[{address,bank,reg}]; }
};
int main() {
    try {
        Require(Cxd2857::IntermediateFrequency(11976820,9505000)==2471820,"left circular LNB conversion");
        bool rejected=false; try { Cxd2857::IntermediateFrequency(11976820,-1); } catch(...) { rejected=true; }
        Require(rejected,"missing LNB rejected");
        Require(!Cxd2857::IsS3(-1) && !Cxd2857::IsS3(65536) && !Cxd2857::IsS3(0x4010) && Cxd2857::IsS3(0xb0e0),"stream ID bounds");
        FakeBus bus; Cxd2857 chip(bus); chip.Adopt(1164560,0xb070);
        bus.writes.clear(); rejected=false; try { chip.Tune(2471820,0x4010); } catch(...) { rejected=true; }
        Require(rejected && bus.writes.empty(),"unsupported tune performs no I2C writes");
        chip.Tune(2471820,0xb0e0);
        Require(bus.At(0x60,0,1)==3 && bus.At(0x60,0,0x45)==2,"8K selects high RF path");
        Require(bus.At(0x60,0,0x10)==0xe3 && bus.At(0x60,0,0x11)==0x6d && bus.At(0x60,0,0x12)==9,"8K frequency bytes");
        Require(bus.At(0x64,0xd0,0x87)==0xb0 && bus.At(0x64,0xd0,0x88)==0xe0 && bus.At(0x64,1,0xc0)==0,"stream ID and TLV enabled");
        for(const auto& write:bus.writes) if(write.first==0x66) Require(write.second[0]!=2 && write.second[0]!=0x14,"no shared clock/reset writes");
        chip.Restore();
        Require(bus.At(0x60,0,1)==1 && bus.At(0x64,0xd0,0x88)==0x70 && bus.At(0x64,1,0xc0)==0,"close restores bootstrap carrier");
        bus.failNextTuner=true; rejected=false; try { chip.Tune(2471820,0xb0e0); } catch(...) { rejected=true; }
        Require(rejected,"injected I2C failure propagated"); chip.Restore();
        Require(bus.At(0x64,1,0xc0)==0 && bus.At(0x64,0xd0,0x88)==0x70,"partial tune can be restored");
        bus.failMonitor=true; try { chip.CNR(); } catch(...) {}
        Require(bus.At(0x64,0,1)==0,"failed C/N read unfreezes monitor");
        FakeBus wrong; wrong.registers[{0x66,0,0xfd}]=0x90; Cxd2857 wrongChip(wrong); rejected=false;
        try { wrongChip.Adopt(1164560,0xb070); } catch(...) { rejected=true; }
        Require(rejected,"wrong chip rejected"); for(const auto& w:wrong.writes) Require(w.first!=0x60,"wrong chip never writes RF tuner");
        FakeBus wrongStream; wrongStream.registers[{0x64,0xd0,0x88}]=0x71; Cxd2857 streamChip(wrongStream); rejected=false;
        try { streamChip.Adopt(1164560,0xb070); } catch(...) { rejected=true; }
        Require(rejected && !streamChip.Adopted(),"wrong bootstrap stream is never adopted");
        FakeBus second; Cxd2857 secondChip(second,1); secondChip.Adopt(1164560,0xb070);
        secondChip.Tune(2471820,0xb0e0); secondChip.Restore();
        Require(second.At(0x61,0,1)==1,"second tuner restores its RF path");
        bool sawSecond=false;
        for(const auto& w:second.writes) {
            Require(w.first!=0x60,"second tuner never writes first RF address");
            if(w.first==0x61) { sawSecond=true; Require(w.second[0]!=0x81,"preserve shared reference output"); }
        }
        Require(sawSecond,"second tuner uses address 0x61");
        rejected=false; try { Cxd2857 invalid(second,2); } catch(...) { rejected=true; }
        Require(rejected,"invalid tuner index rejected");
        FakeBus reusable; Cxd2857 reusableChip(reusable); reusableChip.AdoptCached(1164560,0xb070);
        Require(reusableChip.Adopted(),"locked prepared state can be adopted");
        for(int kind=0;kind<9;++kind) {
            FakeBus stale;
            if(kind==0) stale.registers[{0x64,0xa0,0x11}]=0;
            if(kind==1) stale.registers[{0x66,0,0x10}]=1;
            if(kind==2) stale.registers[{0x64,0,0x81}]=0xff;
            if(kind==3) stale.registers[{0x64,1,0xc0}]=1;
            if(kind==4) stale.registers[{0x66,0,0x17}]=0x0c;
            if(kind==5) stale.registers[{0x66,0,0x14}]=0;
            if(kind==6) stale.registers[{0x64,0,0xc4}]=0x80;
            if(kind==7) stale.registers[{0x64,0,0xa9}]=0;
            if(kind==8) stale.registers[{0x64,0xd0,0x88}]=0x71;
            Cxd2857 staleChip(stale); rejected=false;
            try { staleChip.AdoptCached(1164560,0xb070); } catch(...) { rejected=true; }
            Require(rejected && !staleChip.Adopted(),"inactive cached state rejected before adoption");
            for(const auto& w:stale.writes) Require(w.first!=0x60 && w.first!=0x61,"rejected cached state does not write RF");
        }
        FakeBus standard;
        standard.registers[{0x66,0,0x17}]=0x0c;
        standard.registers[{0x64,0,0xa9}]=0;
        standard.registers[{0x64,0xc0,0xe9}]=0x40;
        standard.registers[{0x64,0xc0,0xea}]=0x10;
        Cxd2857 standardChip(standard,0,DeliverySystem::ISDBS);
        Require(!standardChip.Locked(),"ISDB-S must not use S3 lock bit");
        standard.registers[{0x64,0xa0,0x12}]=0x40;
        standardChip.AdoptCached(1049480,0x4010);
        standardChip.Tune(1087840,0x4030);
        Require(standard.At(0x64,0xc0,0xe9)==0x40 && standard.At(0x64,0xc0,0xea)==0x30 &&
            standard.At(0x64,0xc0,0xeb)==0 && standard.At(0x64,0,0xc3)==0,"absolute TSID and TS enable");
        Require(standard.At(0x60,0,7)==0x80 && standard.At(0x60,0,8)==0x70 &&
            standard.At(0x60,0,9)==0x1e && standard.At(0x60,0,0x0b)==0x24,"ISDB-S RF filters");
        standardChip.Tune(1049480,2);
        Require(standard.At(0x64,0xc0,0xea)==2 && standard.At(0x64,0xc0,0xeb)==1,"relative TS index selection");
        standard.registers[{0x64,0xa1,0x10}]=1;
        standard.registers[{0x64,0xa1,0x11}]=0;
        standard.registers[{0x64,0xa1,0x12}]=0x41;
        Require(standardChip.CNR()==15.0f,"ISDB-S C/N curve and monitor bank");
        standardChip.Restore();
        Require(standard.At(0x64,0xc0,0xea)==0x10 && standard.At(0x64,0xc0,0xeb)==0 &&
            standard.At(0x64,0,0xc3)==0,"ISDB-S restores bootstrap TS selection and output");
        standard.writes.clear();
        rejected=false; try { standardChip.Tune(2471820,0x4010); } catch(...) { rejected=true; }
        Require(rejected && standard.writes.empty(),"ISDB-S rejects out-of-band IF before writes");
        rejected=false; try { standardChip.Tune(1049480,0xb070); } catch(...) { rejected=true; }
        Require(rejected && standard.writes.empty(),"ISDB-S rejects S3 stream before writes");
        FakeBus otherMode; Cxd2857 otherModeChip(otherMode,0,DeliverySystem::ISDBS);
        rejected=false; try { otherModeChip.Adopt(1049480,0x4010); } catch(...) { rejected=true; }
        Require(rejected && !otherModeChip.Adopted(),"wrong demodulator core rejected");
        FakeBus terrestrial;
        terrestrial.registers[{0x66,0,0x17}]=6;
        terrestrial.registers[{0x64,0,0xa9}]=0;
        terrestrial.registers[{0x64,0x10,0xd7}]=4;
        terrestrial.registers[{0x64,0x60,0x10}]=3;
        terrestrial.registers[{0x60,0,0x67}]=0xff;
        Cxd2857 terrestrialChip(terrestrial,0,DeliverySystem::ISDBT);
        terrestrial.writes.clear(); rejected=false;
        try { terrestrialChip.AdoptCached(473143,0); } catch(...) { rejected=true; }
        Require(rejected && terrestrial.writes.empty(),"ISDB-T cache rejected before I2C without RF verification");
        terrestrialChip.Adopt(473143,0);
        terrestrial.writes.clear();
        terrestrialChip.Tune(557143,0);
        Require(terrestrial.At(0x60,0,0x70)==0x57 && terrestrial.At(0x60,0,0x71)==0x80 &&
            terrestrial.At(0x60,0,0x72)==8,"ISDB-T RF frequency packet");
        Require(terrestrial.At(0x60,0,0x69)==0x87 && terrestrial.At(0x60,0,0x6c)==0x33 &&
            terrestrial.At(0x60,0,0x6e)==0x17 && terrestrial.At(0x60,0,0x6f)==0x1b,"ISDB-T 6 MHz RF row");
        Require(terrestrial.At(0x60,0,0x67)==0xfd && terrestrial.At(0x64,0,0xc3)==0 &&
            terrestrial.At(0x60,0,0x87)==0xc0,"ISDB-T masked analog setting and output resume");
        for(const auto& w:terrestrial.writes) {
            if(w.first==0x60) Require(w.second[0]<0x81 || w.second[0]>0x86,"ISDB-T preserves REFOUT and GPIO");
            if(w.first==0x64) Require(w.second[0]!=0xe9 && w.second[0]!=0x87,"ISDB-T does not write satellite stream selector");
        }
        for(uint8_t status : {uint8_t(0),uint8_t(1),uint8_t(2),uint8_t(0x13)}) {
            terrestrial.registers[{0x64,0x60,0x10}]=status;
            Require(!terrestrialChip.Locked(),"ISDB-T requires sync, TS lock and no unlock");
        }
        terrestrial.registers[{0x64,0x60,0x10}]=3;
        Require(terrestrialChip.Locked(),"ISDB-T lock bank");
        terrestrial.registers[{0x64,0x60,0x28}]=0x13;
        terrestrial.registers[{0x64,0x60,0x29}]=0x88;
        Require(std::fabs(terrestrialChip.CNR()-27.9587f)<0.001f,"ISDB-T C/N conversion");
        terrestrial.failMonitor=true; rejected=false; try { terrestrialChip.CNR(); } catch(...) { rejected=true; }
        Require(rejected && terrestrial.At(0x64,0,1)==0,"ISDB-T monitor failure unfreezes registers");
        terrestrial.failNextTuner=true; rejected=false; try { terrestrialChip.Tune(473143,0); } catch(...) { rejected=true; }
        Require(rejected,"ISDB-T partial tune failure propagated");
        terrestrialChip.Restore();
        Require(terrestrial.At(0x60,0,0x70)==0x37 && terrestrial.At(0x60,0,0x71)==0x38 &&
            terrestrial.At(0x60,0,0x72)==7 && terrestrial.At(0x64,0,0xc3)==0,"ISDB-T restore after failed tune");
        terrestrial.writes.clear(); rejected=false;
        try { terrestrialChip.Tune(800000,0); } catch(...) { rejected=true; }
        Require(rejected && terrestrial.writes.empty(),"ISDB-T invalid RF rejected before writes");
        FakeBus wrongBand;
        wrongBand.registers[{0x66,0,0x17}]=6; wrongBand.registers[{0x64,0,0xa9}]=0;
        Cxd2857 wrongBandChip(wrongBand,0,DeliverySystem::ISDBT);
        rejected=false; try { wrongBandChip.Adopt(473143,0); } catch(...) { rejected=true; }
        Require(rejected && !wrongBandChip.Adopted(),"ISDB-T rejects non-6-MHz preparation");
        FakeBus cable;
        cable.registers[{0x66,0,0x17}]=6; cable.registers[{0x64,0,0xa9}]=0;
        cable.registers[{0x64,0x10,0xd7}]=4; cable.registers[{0x64,0x60,0x10}]=3;
        cable.registers[{0x64,0,0xd3}]=0xa5; cable.registers[{0x64,0,0x33}]=0xb6;
        cable.registers[{0x64,0x10,0xb6}]=0x12; cable.registers[{0x64,0x10,0xb7}]=0xee;
        cable.registers[{0x64,0x10,0xb8}]=0xef;
        Cxd2857 cableChip(cable,0,DeliverySystem::J83B);
        cableChip.Adopt(473143,0);
        const auto originalDemod=cable.registers;
        cableChip.BeginJ83B();
        Require(cable.At(0x66,0,0x17)==8 && cable.At(0x64,0x40,0x14)==0x10 &&
            cable.At(0x64,0x40,0x26)==7 && cable.At(0x64,0x40,0x27)==0x77 &&
            cable.At(0x64,0x10,0xb6)==0x14 && cable.At(0x64,0x10,0xb7)==0,"J.83B 5.6 Msym/s core and IF");
        Require(!cableChip.Locked(),"J.83B never reports old ISDB-T lock");
        cable.registers[{0x64,0x40,0x88}]=1;
        Require(!cableChip.Locked(),"J.83B sync alone is not TS lock");
        cable.registers[{0x64,0x40,0x10}]=0x20;
        Require(cableChip.Locked(),"J.83B requires QAM sync and TS lock");
        cable.registers[{0x66,0,0x17}]=6;
        Require(!cableChip.Locked(),"stale QAM flags never report lock after a vendor core change");
        cable.registers[{0x66,0,0x17}]=8;
        cableChip.Tune(314250,0);
        Require(cable.At(0x60,0,0x69)==0x83 && cable.At(0x60,0,0x6b)==9 &&
            cable.At(0x60,0,0x6e)==4 && cable.At(0x60,0,0x6f)==8,"J.83B optical Freia row");
        cable.registers[{0x64,0x40,0x19}]=4; // 256-QAM
        cable.registers[{0x64,0x40,0x4c}]=1; cable.registers[{0x64,0x40,0x4d}]=0;
        Require(std::fabs(cableChip.CNR()-43.2618f)<0.001f,"J.83B QAM C/N conversion");
        cable.failMonitor=true; rejected=false; try { cableChip.CNR(); } catch(...) { rejected=true; }
        Require(rejected && cable.At(0x64,0,1)==0,"J.83B C/N failure releases freeze");
        // Ignore monitor fixture values when comparing writable control registers.
        for(const auto& key : {std::make_tuple(0x64,0x40,0x88),std::make_tuple(0x64,0x40,0x10),
            std::make_tuple(0x64,0x40,0x19),std::make_tuple(0x64,0x40,0x4c),std::make_tuple(0x64,0x40,0x4d)}) cable.registers[key]=0;
        cableChip.Restore();
        for(const auto& entry:cable.registers) {
            const auto [address,bank,reg]=entry.first;
            if((address!=0x64 && address!=0x66) || (address==0x64 && bank==0 && reg==0xfe) ||
                (address==0x66 && bank==0 && reg==8)) continue;
            const auto found=originalDemod.find(entry.first);
            Require(entry.second==(found==originalDemod.end()?0:found->second),"J.83B restore preserves all touched vendor demodulator registers");
        }
        Require(cable.At(0x60,0,0x69)==0x87 && cable.At(0x60,0,0x6e)==0x17 &&
            cable.At(0x64,0,0xc3)==0,"J.83B close restores terrestrial RF row and TS output");
        for(const auto& w:cable.writes) if(w.first==0x66) Require(w.second[0]!=2 && w.second[0]!=0x14,"J.83B never cold-resets or changes shared reference");
        cable.failCableWake=true; rejected=false; try { cableChip.BeginJ83B(); } catch(...) { rejected=true; }
        Require(rejected,"partial J.83B transition failure propagated");
        cableChip.Restore();
        Require(cable.At(0x66,0,0x17)==6 && cable.At(0x64,0x10,0xb6)==0x12 &&
            cable.At(0x64,0,0xc3)==0,"partial J.83B transition restores ISDB-T core/IF/output");
        for(unsigned cycle=0;cycle<3;++cycle) {
            cableChip.Tune(497143,0);
            Require(!cableChip.J83BActive() && cable.At(0x60,0,0x69)==0x87,"combined mode tunes terrestrial RF before cable entry");
            cableChip.BeginJ83B(); cableChip.Tune(314250,0);
            Require(cableChip.J83BActive() && cable.At(0x66,0,0x17)==8 && cable.At(0x60,0,0x69)==0x83,"combined mode enters cable repeatedly");
            cableChip.EndJ83B();
            Require(!cableChip.J83BActive() && cable.At(0x66,0,0x17)==6 && cable.At(0x60,0,0x69)==0x87,"combined mode hands back to terrestrial without close");
        }
        const auto path=std::filesystem::temp_directory_path()/("tbs6812-cache-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".state");
        struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code error;std::filesystem::remove(path,error);} } cleanup{path};
        Require(!ReadWarmRecord(path,17),"missing cache rejected");
        WriteWarmRecord(path,17,{1164560,0xb070});
        const auto saved=ReadWarmRecord(path,17);
        Require(saved && saved->frequencyKHz==1164560 && saved->streamID==0xb070,"cache round trip");
        Require(!ReadWarmRecord(path,18),"different device cache rejected");
        Require(!ReadWarmRecord(path,17,DeliverySystem::ISDBS),"S3 record cannot bootstrap ISDB-S");
        WriteWarmRecord(path,17,{1049480,0x4010,DeliverySystem::ISDBS});
        Require(bool(ReadWarmRecord(path,17,DeliverySystem::ISDBS)),"ISDB-S cache round trip");
        Require(!ReadWarmRecord(path,17),"ISDB-S record cannot bootstrap S3");
        { std::ofstream legacy(path); legacy<<"TBS6812-WARM-1 17 1164560 45168 "<<WarmChecksum(17,1164560,45168)<<'\n'; }
        Require(bool(ReadWarmRecord(path,17)) && !ReadWarmRecord(path,17,DeliverySystem::ISDBS),"legacy cache is S3 only");
        WriteWarmRecord(path,17,{473143,0,DeliverySystem::ISDBT});
        Require(!ReadWarmRecord(path,17,DeliverySystem::ISDBT),"ISDB-T warm cache remains unsupported");
        Require(!ReadWarmRecord(path,17,DeliverySystem::ISDBS) && !ReadWarmRecord(path,17),"terrestrial cache isolated from satellite");
        WriteWarmRecord(path,17,{1,0xb070}); Require(!ReadWarmRecord(path,17),"invalid cached IF rejected");
        WriteWarmRecord(path,17,{1164560,0x4010}); Require(!ReadWarmRecord(path,17),"invalid cached stream rejected");
        { std::ofstream corrupt(path); corrupt<<"TBS6812-WARM-1 17 1164560 45168 0\n"; }
        Require(!ReadWarmRecord(path,17),"corrupt cache checksum rejected");
        { std::ofstream malformed(path); malformed<<"not-a-cache\n"; }
        Require(!ReadWarmRecord(path,17),"malformed cache rejected");
        { std::ofstream oversized(path); oversized<<std::string(300,'x'); }
        Require(!ReadWarmRecord(path,17),"oversized cache rejected");
        std::cout<<"PASS: J.83B core/RF/CNR/transactional restore, ISDB-T RF/lock/CNR/restore/cache, IF/ID validation, warm restore, I2C failures, independent RF addresses, cached hardware guards, cache validation\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
