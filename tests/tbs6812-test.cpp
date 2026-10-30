// SPDX-License-Identifier: GPL-2.0-or-later
#include "Cxd2857.h"
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
    bool failNextTuner=false, failMonitor=false;
    FakeBus() {
        registers[{0x66,0,0xfb}]=0; registers[{0x66,0,0xfd}]=0x91;
        registers[{0x66,0,0x17}]=0x0d; registers[{0x66,0,0x14}]=1;
        registers[{0x64,0,0xa9}]=1; registers[{0x64,0,0xc4}]=0x29;
        registers[{0x64,0xd0,0x87}]=0xb0; registers[{0x64,0xd0,0x88}]=0x70;
        registers[{0x64,0xa0,0x11}]=0x40;
    }
    void Write(uint8_t address,const Bytes& data) override {
        if((address==0x60 || address==0x61) && failNextTuner) { failNextTuner=false; throw std::runtime_error("injected tuner write failure"); }
        Require(data.size()>=2,"empty register write"); writes.emplace_back(address,data);
        if(address!=0x60 && address!=0x61 && data[0]==0) { banks[address]=data[1]; return; }
        for(size_t i=1;i<data.size();++i) registers[{address,banks[address],data[0]+int(i)-1}]=data[i];
    }
    Bytes Read(uint8_t address,uint8_t reg,size_t length) override {
        if(failMonitor && address==0x64 && banks[address]==0xd0 && reg==0xf3) throw std::runtime_error("injected monitor failure");
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
        std::cout<<"PASS: IF/ID validation, 8K path, warm restore, partial I2C failure, monitor cleanup, chip identity, independent RF addresses\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
