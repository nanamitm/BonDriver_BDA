// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <stdexcept>
#include <string>

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

// "S,S", "T,S", ... for input 0 and input 1.
inline std::optional<BoardModes> ParseBoardHome(const wchar_t* text) {
    const auto mode=[](wchar_t c)->std::optional<uint8_t> {
        if(c==L'T' || c==L't') return BoardModeTerrestrial;
        if(c==L'S' || c==L's') return BoardModeSatellite;
        return {};
    };
    if(!text || wcslen(text)!=3 || text[1]!=L',') return {};
    const auto first=mode(text[0]), second=mode(text[2]);
    if(!first || !second) return {};
    return BoardModes{*first,*second};
}

namespace board_detail {
struct Claims {
    BoardModeDriver& driver; bool held[BoardSlots]{};
    ~Claims() { for(unsigned slot=0;slot<BoardSlots;++slot) if(held[slot]) driver.Release(slot); }
};
}

// Called when this receiver's graph cannot serve the wanted family. Changes only the input
// that family needs, so the other input keeps its family: terrestrial uses input 0, satellite input 1.
// ownSlot<0 means this receiver has no graph. Returns true when the card was rewritten.
inline bool AlignBoardModes(BoardModeDriver& driver,int ownSlot,uint8_t wanted) {
    board_detail::Claims claims{driver};
    int via=-1; bool allFree=true;
    for(unsigned slot=0;slot<BoardSlots;++slot) {
        if(int(slot)==ownSlot) continue;
        claims.held[slot]=driver.TryClaim(slot);
        if(!claims.held[slot]) allFree=false;
        else if(via<0) via=int(slot);
    }
    if(!allFree) {
        if(ownSlot<0 && via>=0) return false; // The free input may already serve this family.
        throw BoardBusy("another receiver is using the card");
    }
    auto target=driver.Read(unsigned(via));
    if(target[0]==wanted || target[1]==wanted) return false;
    target[wanted==BoardModeTerrestrial ? 0 : 1]=wanted;
    driver.CloseOwnGraph();
    if(ownSlot>=0) {
        claims.held[ownSlot]=driver.TryClaim(unsigned(ownSlot));
        if(!claims.held[ownSlot]) throw BoardBusy("another receiver took the input during the mode switch");
    }
    driver.Write(unsigned(via),target);
    if(!driver.WaitFor(unsigned(via),target)) throw std::runtime_error("the card did not return in the requested mode");
    return true;
}

// Returns the card to its configured modes after a receiver closed. A busy card stays as it is.
inline bool RestoreHomeModes(BoardModeDriver& driver,const BoardModes& home) {
    board_detail::Claims claims{driver};
    for(unsigned slot=0;slot<BoardSlots;++slot) {
        claims.held[slot]=driver.TryClaim(slot);
        if(!claims.held[slot]) return false;
    }
    if(driver.Read(0)==home) return false;
    driver.Write(0,home);
    if(!driver.WaitFor(0,home)) throw std::runtime_error("the card did not return in the configured modes");
    return true;
}
}
