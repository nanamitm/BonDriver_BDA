// SPDX-License-Identifier: GPL-2.0-or-later
#include "BoardModeSwitch.h"
#include <iostream>
#include <string>
#include <vector>
using namespace tbs6812;
namespace {
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
constexpr auto T=BoardModeTerrestrial;
constexpr auto S=BoardModeSatellite;
struct Mock : BoardModeDriver {
    Mock(int own,BoardModes initial):ownSlot(own),modes(initial) { if(own>=0) busy[own]=true; }
    int ownSlot;
    BoardModes modes;
    bool busy[BoardSlots]{}, ready=true, closed=false;
    int writes=0;
    std::vector<std::string> order;
    bool TryClaim(unsigned slot) override { if(busy[slot]) return false; busy[slot]=true; return true; }
    void Release(unsigned slot) override { busy[slot]=false; }
    BoardModes Read(unsigned) override { return modes; }
    void Write(unsigned,const BoardModes& value) override {
        Require(busy[0] && busy[1],"both inputs are claimed before the write");
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
        { // S,S and terrestrial: only input 0 changes, input 1 can still receive S3.
            Mock mock(1,{S,S});
            Require(AlignBoardModes(mock,1,T),"missing family is written");
            Require(mock.modes==BoardModes{T,S},"only the input the family needs changes");
            Require(mock.order==std::vector<std::string>{"close","write"},"the graph closes before the write");
            Require(!mock.busy[0] && !mock.busy[1],"claims are released for the rebuild");
        }
        { // T,T and satellite: only input 1 changes.
            Mock mock(0,{T,T});
            Require(AlignBoardModes(mock,0,S) && mock.modes==BoardModes{T,S},"satellite uses input 1");
        }
        { // The other input already has the family: no restart.
            Mock mock(0,{T,S});
            Require(!AlignBoardModes(mock,0,S),"available family is not rewritten");
            Require(!mock.closed && !mock.writes && mock.busy[0] && !mock.busy[1],"caller rebuilds; peer claim is released");
        }
        { // Peer busy: refuse, keep receiving.
            Mock mock(0,{T,S}); mock.busy[1]=true;
            Require(Throws<BoardBusy>([&]{AlignBoardModes(mock,0,S);}),"busy peer rejects a family change");
            Require(!mock.closed && !mock.writes && mock.busy[0] && mock.busy[1],"rejection leaves the graph and peer untouched");
        }
        { // A card that does not return is an error and frees the claims.
            Mock mock(0,{T,T}); mock.ready=false;
            Require(Throws<std::runtime_error>([&]{AlignBoardModes(mock,0,S);}),"card that does not return is an error");
            Require(!mock.busy[0] && !mock.busy[1],"claims are released after failure");
        }
        { // No own graph: a busy input prevents a write, the free one may still serve.
            Mock mock(-1,{T,T}); mock.busy[1]=true;
            Require(!AlignBoardModes(mock,-1,S) && !mock.writes,"partly busy card is never rewritten");
            Require(!mock.busy[0] && mock.busy[1],"probe claim is released");
            mock.busy[0]=true;
            Require(Throws<BoardBusy>([&]{AlignBoardModes(mock,-1,S);}),"fully busy card is reported");
        }
        { // No own graph and a free card writes.
            Mock mock(-1,{S,S});
            Require(AlignBoardModes(mock,-1,T) && mock.modes==BoardModes{T,S},"free card without a graph aligns");
        }
        { // Closing returns to the configured modes.
            Mock mock(-1,{T,S});
            Require(RestoreHomeModes(mock,{S,S}) && mock.modes==BoardModes{S,S} && !mock.busy[0] && !mock.busy[1],"free card returns home");
        }
        { // Already home: no restart.
            Mock mock(-1,{S,S});
            Require(!RestoreHomeModes(mock,{S,S}) && !mock.writes,"home card is not restarted");
        }
        { // A receiver on either input keeps the card as it is.
            Mock mock(-1,{T,S}); mock.busy[1]=true;
            Require(!RestoreHomeModes(mock,{S,S}) && !mock.writes && mock.busy[1] && !mock.busy[0],"busy card is never restored");
        }
        Require(ParseBoardHome(L"S,S")==BoardModes{S,S} && ParseBoardHome(L"t,s")==BoardModes{T,S},"home modes parse");
        for(const wchar_t* invalid : {L"",L"S",L"S,",L"S,X",L"S;S",L"S,S,S"}) Require(!ParseBoardHome(invalid),"invalid home modes are rejected");
        Require(!ParseBoardHome(nullptr),"missing home modes are rejected");
        std::cout<<"PASS: mode changes need a free card, touch only the needed input and keep the peer and graph on rejection; home restore\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
