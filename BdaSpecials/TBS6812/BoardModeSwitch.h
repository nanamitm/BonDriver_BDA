// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>

namespace tbs6812 {
constexpr uint8_t BoardModeTerrestrial=1, BoardModeSatellite=2;
constexpr unsigned BoardSlots=2;
using BoardModes=std::array<uint8_t,BoardSlots>;

// Another receiver holds the card; writing the mode property would restart it too.
struct BoardBusy : std::runtime_error { using std::runtime_error::runtime_error; };

struct BoardModeDriver {
    virtual ~BoardModeDriver()=default;
    virtual bool TryClaim(unsigned slot)=0;
    virtual void Release(unsigned slot)=0;
    virtual BoardModes Read(unsigned slot)=0;
    virtual void Write(unsigned slot,const BoardModes& modes)=0;
    virtual bool WaitFor(unsigned slot,const BoardModes& modes)=0;
    virtual void CloseOwnGraph()=0;
};

// Moves both inputs to the wanted family, only while no other receiver holds the card.
// ownSlot<0 means this receiver has no graph. Returns true when the card was rewritten.
inline bool AlignBoardModes(BoardModeDriver& driver,int ownSlot,uint8_t wanted,bool graphMatches) {
    struct Claims {
        BoardModeDriver& driver; bool held[BoardSlots]{};
        ~Claims() { for(unsigned slot=0;slot<BoardSlots;++slot) if(held[slot]) driver.Release(slot); }
    } claims{driver};
    int via=-1; bool allFree=true;
    for(unsigned slot=0;slot<BoardSlots;++slot) {
        if(int(slot)==ownSlot) continue;
        claims.held[slot]=driver.TryClaim(slot);
        if(!claims.held[slot]) allFree=false;
        else if(via<0) via=int(slot);
    }
    if(!allFree) {
        if(graphMatches || (ownSlot<0 && via>=0)) return false; // The current graph or a free input may still serve this space.
        throw BoardBusy("another receiver is using the card");
    }
    const BoardModes target{wanted,wanted};
    if(driver.Read(unsigned(via))==target) return false;
    driver.CloseOwnGraph();
    if(ownSlot>=0) {
        claims.held[ownSlot]=driver.TryClaim(unsigned(ownSlot));
        if(!claims.held[ownSlot]) throw BoardBusy("another receiver took the input during the mode switch");
    }
    driver.Write(unsigned(via),target);
    if(!driver.WaitFor(unsigned(via),target)) throw std::runtime_error("the card did not return in the requested mode");
    return true;
}
}
