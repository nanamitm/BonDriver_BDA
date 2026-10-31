// SPDX-License-Identifier: GPL-2.0-or-later
#include "BoardModeSwitch.h"
#include <iostream>
#include <string>
#include <vector>
using namespace tbs6812;
namespace {
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Mock : BoardModeDriver {
    explicit Mock(int own):ownSlot(own) { if(own>=0) busy[own]=true; }
    int ownSlot;
    BoardModes modes{BoardModeTerrestrial,BoardModeSatellite};
    bool busy[BoardSlots]{}, ready=true, closed=false;
    int writes=0;
    std::vector<std::string> order;
    bool TryClaim(unsigned slot) override { if(busy[slot]) return false; busy[slot]=true; return true; }
    void Release(unsigned slot) override { busy[slot]=false; }
    BoardModes Read(unsigned) override { return modes; }
    void Write(unsigned,const BoardModes& value) override {
        Require(busy[0] && busy[1],"both inputs are claimed before the write");
        Require(closed,"the own graph is closed before the write");
        modes=value; ++writes; order.push_back("write");
    }
    bool WaitFor(unsigned,const BoardModes& value) override { return ready && modes==value; }
    void CloseOwnGraph() override { closed=true; if(ownSlot>=0) busy[ownSlot]=false; order.push_back("close"); }
};
template<class Exception,class Function> bool Throws(Function function) {
    try { function(); } catch(const Exception&) { return true; }
    return false;
}
}
int main() {
    try {
        { // Both free, own graph on slot 0: the card moves to one family.
            Mock mock(0);
            Require(AlignBoardModes(mock,0,BoardModeSatellite,false),"different modes are rewritten");
            Require(mock.modes==BoardModes{BoardModeSatellite,BoardModeSatellite},"both inputs follow the selected family");
            Require(mock.order==std::vector<std::string>{"close","write"},"the graph closes before the write");
            Require(!mock.busy[0] && !mock.busy[1],"claims are released for the rebuild");
        }
        { // First channel of a family with the other input in the other mode also aligns.
            Mock mock(0);
            Require(AlignBoardModes(mock,0,BoardModeTerrestrial,true) && mock.modes==BoardModes{BoardModeTerrestrial,BoardModeTerrestrial},"matching graph still aligns a free card");
        }
        { // Already aligned: no restart and the graph stays open.
            Mock mock(0); mock.modes={BoardModeTerrestrial,BoardModeTerrestrial};
            Require(!AlignBoardModes(mock,0,BoardModeTerrestrial,true),"aligned card is not rewritten");
            Require(!mock.closed && !mock.writes && mock.busy[0] && !mock.busy[1],"aligned card keeps the graph and frees the peer claim");
        }
        { // Peer busy and another family requested: refuse, keep receiving.
            Mock mock(0); mock.busy[1]=true;
            Require(Throws<BoardBusy>([&]{AlignBoardModes(mock,0,BoardModeSatellite,false);}),"busy peer rejects a family change");
            Require(!mock.closed && !mock.writes && mock.busy[0] && mock.busy[1],"rejection leaves the graph and peer untouched");
        }
        { // Peer busy within the same family: tune normally, no write.
            Mock mock(0); mock.busy[1]=true;
            Require(!AlignBoardModes(mock,0,BoardModeTerrestrial,true) && !mock.closed && !mock.writes,"busy peer does not block the same family");
        }
        { // A card that does not return is an error and frees the claims.
            Mock mock(0); mock.ready=false;
            Require(Throws<std::runtime_error>([&]{AlignBoardModes(mock,0,BoardModeSatellite,false);}),"card that does not return is an error");
            Require(!mock.busy[0] && !mock.busy[1],"claims are released after failure");
        }
        { // No own graph: a busy input prevents a write, the free one may still serve.
            Mock mock(-1); mock.busy[1]=true;
            Require(!AlignBoardModes(mock,-1,BoardModeTerrestrial,false) && !mock.writes,"partly busy card is never rewritten");
            Require(!mock.busy[0] && mock.busy[1],"probe claim is released");
            mock.busy[0]=true;
            Require(Throws<BoardBusy>([&]{AlignBoardModes(mock,-1,BoardModeTerrestrial,false);}),"fully busy card is reported");
        }
        { // No own graph and a free card aligns.
            Mock mock(-1);
            Require(AlignBoardModes(mock,-1,BoardModeSatellite,false) && mock.writes==1,"free card without a graph aligns");
        }
        std::cout<<"PASS: board mode alignment requires a free card and keeps the peer and current graph on rejection\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
