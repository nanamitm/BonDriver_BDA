// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include <atlbase.h>
#include <dshow.h>
#include <tuner.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include "IBdaSpecials2.h"
#include "Cxd2857.h"
#include "WarmStartCache.h"
#include "SystemRouting.h"

namespace {
const GUID accessSet = {0xfaa8f3e5,0x31d4,0x4e41,{0x88,0xef,0xd9,0xeb,0x71,0x6f,0x6e,0xc9}};
HMODULE selfModule = nullptr;
std::mutex logMutex;
void WriteLog(const std::string& message) {
    std::lock_guard<std::mutex> guard(logMutex);
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(selfModule,path,MAX_PATH);
    std::wstring name(path); const auto dot=name.rfind(L'.'); if(dot!=std::wstring::npos) name.resize(dot); name+=L".log";
    FILE* file=nullptr; _wfopen_s(&file,name.c_str(),L"ab");
    SYSTEMTIME now{}; GetLocalTime(&now);
    if(file) { fprintf(file,"%02u:%02u:%02u.%03u pid=%lu %s\n",now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,GetCurrentProcessId(),message.c_str()); fclose(file); }
    OutputDebugStringA(("TBS6812: "+message+"\n").c_str());
}
std::string HResult(HRESULT hr) { std::ostringstream out; out<<"HRESULT=0x"<<std::hex<<std::setw(8)<<std::setfill('0')<<uint32_t(hr); return out.str(); }
std::wstring Lower(std::wstring value) { std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){ return wchar_t(towlower(c)); }); return value; }
int Target(const wchar_t* display,const wchar_t* friendly,const wchar_t* kind=L"tuner") {
    const std::wstring d=Lower(display?display:L""), n=Lower(friendly?friendly:L"");
    if(d.find(L"ven_544d")==d.npos || d.find(L"dev_6178")==d.npos || d.find(L"subsys_00016812")==d.npos ||
        n.find(L"6812")==n.npos || n.size()<7) return -1;
    for(int index=0;index<2;++index) {
        const auto suffix=std::wstring(kind)+L" "+std::to_wstring(index);
        if(n.size()>=suffix.size() && n.compare(n.size()-suffix.size(),suffix.size(),suffix)==0) return index;
    }
    return -1;
}
std::wstring BoardPath(const wchar_t* display) {
    const auto name=Lower(display?display:L"");
    const auto start=name.find(L"pci#");
    const auto end=name.find(L"#{",start);
    return start!=name.npos && end!=name.npos ? name.substr(start,end-start) : L"";
}
CComPtr<IKsPropertySet> Properties(IBaseFilter* tuner) {
    CComPtr<IEnumPins> pins;
    if(!tuner || FAILED(tuner->EnumPins(&pins))) throw std::runtime_error("cannot enumerate the tuner pins");
    for(;;) {
        CComPtr<IPin> pin; ULONG fetched=0;
        if(pins->Next(1,&pin,&fetched)!=S_OK) break;
        PIN_DIRECTION direction{};
        if(SUCCEEDED(pin->QueryDirection(&direction)) && direction==PINDIR_INPUT) {
            CComQIPtr<IKsPropertySet> property(pin);
            if(property) return CComPtr<IKsPropertySet>(property);
        }
    }
    throw std::runtime_error("connected input pin has no IKsPropertySet");
}
class KsBus : public tbs6812::I2cBus {
    CComPtr<IKsPropertySet> property_;
public:
    explicit KsBus(IKsPropertySet* property):property_(property) {}
    void Write(uint8_t address,const tbs6812::Bytes& data) override {
        if(data.empty() || data.size()>128) throw std::runtime_error("I2C write length outside 1..128");
        BYTE request[0x103]{}; request[0]=address<<1;
        std::copy(data.begin(),data.end(),request+1); request[0x81]=BYTE(data.size());
        const HRESULT hr=property_->Set(accessSet,0x19,nullptr,0,request,sizeof(request));
        if(FAILED(hr)) { std::ostringstream s; s<<"I2C write slave=0x"<<std::hex<<int(address)<<" reg=0x"<<int(data[0])<<" "<<HResult(hr); throw std::runtime_error(s.str()); }
    }
    tbs6812::Bytes Read(uint8_t address,uint8_t reg,size_t length) override {
        if(!length || length>128) throw std::runtime_error("I2C read length outside 1..128");
        BYTE request[0x103]{},reply[0x103]{};
        request[0]=address<<1; request[1]=reg; request[0x81]=1; request[0x102]=BYTE(length);
        DWORD returned=0;
        const HRESULT hr=property_->Get(accessSet,0x19,request,sizeof(request),reply,sizeof(reply),&returned);
        if(FAILED(hr)) { std::ostringstream s; s<<"I2C read slave=0x"<<std::hex<<int(address)<<" reg=0x"<<int(reg)<<" "<<HResult(hr); throw std::runtime_error(s.str()); }
        if(returned<0x82+length) throw std::runtime_error("I2C read returned a truncated property structure");
        return {reply+0x82,reply+0x82+length};
    }
};

class Specials final : public IBdaSpecials2b5 {
    CComPtr<IBaseFilter> tuner_;
    CComPtr<IKsPropertySet> property_;
    std::unique_ptr<KsBus> bus_;
    std::unique_ptr<tbs6812::Cxd2857> chip_;
    std::mutex mutex_;
    int tunerIndex_;
    std::wstring identity_;
    bool fatal_=false, bootstrapPending_=false, finalized_=false;
    bool inspectWarm_=false;
    bool warmStart_=false;
    tbs6812::DeliverySystem system_=tbs6812::DeliverySystem::ISDBS3;
    bool validSystem_=true;
    tbs6812::SystemRouting routing_;
    bool requestedCable_=false, vendorSettled_=false, terrestrialCombined_=false;
    std::wstring cacheDirectory_,cachePath_;
    uint64_t deviceHash_=0;
    std::optional<tbs6812::WarmStartRecord> originalRecord_;
    DWORD lockTimeout_=3000;
    uint32_t bootstrapFrequency_=473143;
    TuningParam bootstrap_;
    std::optional<TuningParam> cableRequested_;
    TuningParam* substitutedParam_=nullptr;
    HANDLE claim_=nullptr;
    void Log(const std::string& message) const { WriteLog("tuner="+std::to_string(tunerIndex_)+" "+message); }
    void Inspect(const char* stage) {
        if(!inspectWarm_ || !chip_) return;
        try { Log(std::string(stage)+": "+chip_->InspectWarmState()); }
        catch(const std::exception& e) { Log(std::string(stage)+": inspection failed: "+e.what()); }
    }
    void ConsumeCache() {
        if(!cachePath_.empty() && !DeleteFileW(cachePath_.c_str()) && GetLastError()!=ERROR_FILE_NOT_FOUND)
            throw std::runtime_error("cannot invalidate the experimental warm-start record");
    }
    void SaveCache() {
        if(cachePath_.empty() || !originalRecord_) return;
        const auto start=std::chrono::steady_clock::now();
        while(!chip_->Locked()) {
            if(std::chrono::steady_clock::now()-start>=std::chrono::milliseconds(lockTimeout_)) {
                Log("warm-cache not saved: restored carrier did not lock");
                return;
            }
            Sleep(5);
        }
        const auto temporary=cachePath_+L".tmp";
        tbs6812::WriteWarmRecord(temporary,deviceHash_,*originalRecord_);
        if(!MoveFileExW(temporary.c_str(),cachePath_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot publish the experimental warm-start record");
        Log("warm-cache published after locked restoration; wait_ms="+std::to_string(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()));
    }
    bool Terrestrial() const { return system_==tbs6812::DeliverySystem::ISDBT || Cable(); }
    bool Cable() const { return terrestrialCombined_ ? requestedCable_ : system_==tbs6812::DeliverySystem::J83B; }
    tbs6812::DeliverySystem ChipSystem() const { return terrestrialCombined_ ? tbs6812::DeliverySystem::J83B : system_; }
    void SelectCore() {
        if(!Terrestrial()) return;
        try {
            if(Cable() && !chip_->J83BActive()) {
                if(!vendorSettled_) {
                    chip_->Restore();
                    const auto start=std::chrono::steady_clock::now();
                    auto stableSince=start;
                    bool stable=false;
                    while(std::chrono::steady_clock::now()-start<std::chrono::seconds(5)) {
                        Sleep(20);
                        const auto now=std::chrono::steady_clock::now();
                        if(!chip_->Locked()) stableSince=now;
                        else if(now-stableSince>=std::chrono::milliseconds(1200)) { stable=true; break; }
                    }
                    if(!stable) throw std::runtime_error("ISDB-T bootstrap did not settle before J.83B takeover");
                    vendorSettled_=true;
                }
                chip_->BeginJ83B();
                Log("selected J.83B core on the current tuner");
            } else if(!Cable() && chip_->J83BActive()) {
                chip_->EndJ83B();
                Log("selected ISDB-T core on the current tuner");
            }
        } catch(...) { fatal_=true; throw; }
    }
    uint16_t Stream(const TuningParam& p) const { return Terrestrial() ? 0 : uint16_t(p.TSID); }
    uint32_t IF(const TuningParam& p) const {
        if(!Terrestrial()) return tbs6812::Cxd2857::IntermediateFrequency(p.Frequency,p.Antenna.LowOscillator);
        if(p.Frequency<90000 || p.Frequency>770000) throw std::runtime_error("terrestrial-side RF must be 90..770 MHz");
        return uint32_t(p.Frequency);
    }
    bool Supported(const TuningParam* p) {
        if(Terrestrial()) return p && p->Modulation.BandWidth==6 &&
            p->Polarisation<=BDA_POLARISATION_NOT_DEFINED && p->Antenna.DiSEqC<=0;
        return p && tbs6812::Cxd2857::SupportsID(p->TSID,system_) && (p->Polarisation==BDA_POLARISATION_CIRCULAR_R || p->Polarisation==BDA_POLARISATION_CIRCULAR_L) && p->Antenna.Tone==0 && p->Antenna.DiSEqC<=0;
    }
    const HRESULT Error(const char* stage,const std::exception& e) { Log(std::string(stage)+": "+e.what()); return E_FAIL; }
    void DisableTerrestrialWarmStart() {
        if(Terrestrial() && warmStart_) {
            warmStart_=false;
            Log("Terrestrial-side cached takeover is not enabled; vendor preparation is required on each open");
        }
    }
public:
    Specials(IBaseFilter* tuner,int index,const wchar_t* display):tuner_(tuner),tunerIndex_(index),identity_(Lower(display?display:L"")) {}
    ~Specials() { if(claim_) CloseHandle(claim_); }
    const HRESULT ReadIniFile(const WCHAR* path) override {
        lockTimeout_=std::clamp<DWORD>(GetPrivateProfileIntW(L"TBS6812",L"LockTimeoutMs",3000,path),100,10000);
        inspectWarm_=GetPrivateProfileIntW(L"TBS6812",L"InspectWarmState",0,path)!=0;
        warmStart_=GetPrivateProfileIntW(L"TBS6812",L"ExperimentalWarmStart",0,path)!=0;
        try { routing_=tbs6812::SystemRouting::Read(path); system_=routing_.fixed; terrestrialCombined_=routing_.combined && !routing_.mixed; }
        catch(const std::exception& e) { validSystem_=false; return Error("read delivery routing",e); }
        bootstrapFrequency_=GetPrivateProfileIntW(L"TBS6812",L"BootstrapFrequencyKHz",473143,path);
        if(!routing_.mixed) DisableTerrestrialWarmStart();
        wchar_t directory[MAX_PATH]{};
        GetPrivateProfileStringW(L"TBS6812",L"WarmStartCacheDirectory",L"",directory,MAX_PATH,path);
        cacheDirectory_=directory;
        return S_OK;
    }
    const HRESULT InitializeHook() override {
        std::lock_guard<std::mutex> guard(mutex_);
        try {
            if(!validSystem_) throw std::runtime_error("DeliverySystem must be ISDB-T, ISDB-S, ISDB-S3 or J.83B");
            if(tunerIndex_<0) throw std::runtime_error("only PCI TBS6812 Tuner 0/1 is supported");
            // Stable device-path hash keeps claims independent across slots and cards.
            uint64_t hash=14695981039346656037ULL;
            for(wchar_t c:identity_) { hash^=uint16_t(c); hash*=1099511628211ULL; }
            deviceHash_=hash;
            const auto name=L"Local\\BonDriver_TBS6812_Direct_"+std::to_wstring(hash)+L"_"+std::to_wstring(tunerIndex_);
            claim_=CreateMutexW(nullptr,FALSE,name.c_str());
            if(!claim_ || GetLastError()==ERROR_ALREADY_EXISTS) throw std::runtime_error("this tuner direct-control extension is already open");
            property_=Properties(tuner_);
            DWORD support=0; HRESULT hr=property_->QuerySupported(accessSet,0x19,&support);
            if(FAILED(hr) || (support&(KSPROPERTY_SUPPORT_GET|KSPROPERTY_SUPPORT_SET))!=(KSPROPERTY_SUPPORT_GET|KSPROPERTY_SUPPORT_SET)) throw std::runtime_error("raw I2C property 0x19 is unavailable");
            BYTE modes[8]{}; DWORD returned=0;
            hr=property_->Get(accessSet,0x3c,nullptr,0,modes,sizeof(modes),&returned);
            if(FAILED(hr)) throw std::runtime_error("read tuner modes: "+HResult(hr));
            if(returned<=DWORD(tunerIndex_)) throw std::runtime_error("tuner mode property returned a truncated slot array");
            if(routing_.mixed) {
                if(modes[tunerIndex_]!=tbs6812::BoardModeTerrestrial && modes[tunerIndex_]!=tbs6812::BoardModeSatellite) throw std::runtime_error("selected tuner reports an unknown board mode");
                terrestrialCombined_=modes[tunerIndex_]==tbs6812::BoardModeTerrestrial;
                system_=terrestrialCombined_ ? tbs6812::DeliverySystem::ISDBT : tbs6812::DeliverySystem::ISDBS;
                DisableTerrestrialWarmStart();
            }
            if((terrestrialCombined_ || Cable()) && !tbs6812::Cxd2857::ValidFrequency(bootstrapFrequency_,tbs6812::DeliverySystem::ISDBT)) throw std::runtime_error("invalid ISDB-T BootstrapFrequencyKHz");
            bus_=std::make_unique<KsBus>(property_); chip_=std::make_unique<tbs6812::Cxd2857>(*bus_,unsigned(tunerIndex_),ChipSystem());
            if(modes[tunerIndex_]!=(Terrestrial()?1:2)) throw std::runtime_error(
                Terrestrial() ? "selected tuner must be in terrestrial mode (1); close all card users and use the vendor mode tool first"
                              : "selected tuner must be in satellite mode (2); close all card users and use the vendor mode tool first");
            Log(warmStart_ ? "initialized: experimental cached warm start; vendor fallback available" : "initialized: warm ISDB takeover; vendor bootstrap required");
            if(warmStart_) {
                if(cacheDirectory_.empty() || !std::filesystem::path(cacheDirectory_).is_absolute()) throw std::runtime_error("experimental warm start requires an absolute cache directory");
                std::filesystem::create_directories(cacheDirectory_);
                cachePath_=(std::filesystem::path(cacheDirectory_)/(std::to_wstring(hash)+L"_"+std::to_wstring(tunerIndex_)+L".state")).wstring();
            }
            Inspect("opened-before-vendor");
            return S_OK;
        } catch(const std::exception& e) { fatal_=true; return Error("initialize",e); }
    }
    const HRESULT SetLNBPower(bool active) override {
        if(Terrestrial()) return S_OK; // Never change shared LNB supply for terrestrial reception.
        if(!property_) return E_FAIL;
        // Only adapter 0 owns the board's LNB supply. Slot 1 must not change it.
        if(tunerIndex_==1) return S_OK;
        BYTE request[0x94]{}; const DWORD value=active?2:0; // 13 V or off
        memcpy(request+0x0c,&value,sizeof(value));
        return property_->Set(accessSet,0x15,nullptr,0,request,sizeof(request));
    }
    const HRESULT Set22KHz(bool active) override { return active?E_INVALIDARG:S_OK; }
    const HRESULT Set22KHz(long tone) override { return tone==0?S_OK:E_INVALIDARG; }
    const HRESULT SetTSid(long id) override {
        if(Terrestrial()) return S_OK; // TSID does not select terrestrial RF.
        if(!property_ || !tbs6812::Cxd2857::SupportsID(id,system_)) return E_INVALIDARG;
        return property_->Set(accessSet,0x60,&id,sizeof(id),&id,sizeof(id));
    }
    const HRESULT LockChannel(const TuningParam* p) override {
        std::lock_guard<std::mutex> guard(mutex_);
        if(fatal_ || finalized_ || !chip_) return E_FAIL;
        if(!Supported(p)) { Log("rejected: unsupported channel: check delivery system, 6 MHz ISDB-T bandwidth or circular satellite settings"); return E_INVALIDARG; }
        try {
            const auto frequency=IF(*p);
            if(system_==tbs6812::DeliverySystem::ISDBS && frequency>2150000) throw std::runtime_error("ISDB-S IF must be 950..2150 MHz");
            if(!chip_->Adopted()) {
                if(!cachePath_.empty()) {
                    const auto cached=tbs6812::ReadWarmRecord(cachePath_,deviceHash_,system_);
                    ConsumeCache(); // A crash from this point cannot reuse stale state.
                    if(cached) {
                        try {
                            chip_->AdoptCached(cached->frequencyKHz,cached->streamID);
                            originalRecord_=cached;
                            Log("warm-cache adopted: skipping vendor TuneRequest");
                        } catch(const std::exception& e) {
                            Log(std::string("warm-cache rejected: ")+e.what());
                            chip_=std::make_unique<tbs6812::Cxd2857>(*bus_,unsigned(tunerIndex_),ChipSystem());
                        }
                    } else { Log("warm-cache absent/invalid: vendor bootstrap required"); }
                }
            }
            if(!chip_->Adopted()) {
                if(system_==tbs6812::DeliverySystem::ISDBS && p->TSID<8) {
                    Log("bootstrap requires an absolute TSID; relative TS indices are supported after adoption only");
                    return E_INVALIDARG;
                }
                bootstrap_=Cable() && cableRequested_ ? *cableRequested_ : *p; bootstrapPending_=true;
                const HRESULT hr=SetLNBPower(true); if(FAILED(hr)) throw std::runtime_error("LNB preparation failed: "+HResult(hr));
                Log("bootstrap: returning E_NOINTERFACE for the initial vendor tune only");
                return E_NOINTERFACE;
            }
            const auto start=std::chrono::steady_clock::now();
            SelectCore();
            chip_->Tune(frequency,Stream(*p));
            do {
                if(chip_->Locked()) {
                    std::ostringstream s; s<<"direct tune IF_kHz="<<frequency<<" stream_id=0x"<<std::hex<<p->TSID<<std::dec<<" lock_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); Log(s.str());
                    return S_OK;
                }
                Sleep(5);
            } while(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(lockTimeout_));
            Log("direct tune timed out: no configured ISDB lock; vendor tune was not resubmitted");
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        } catch(const std::exception& e) { return Error("direct tune",e); }
    }
    const HRESULT PreLockChannel(TuningParam* p) override {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto selected=p ? routing_.ForSpace(p->IniSpaceID) : std::nullopt;
        if(!selected) { Log("rejected: tuning space has no delivery system mapping"); return E_INVALIDARG; }
        if(routing_.mixed && tbs6812::BoardModeFor(*selected)!=tbs6812::BoardModeFor(system_)) { Log("rejected: tuning space belongs to the other board mode"); return E_INVALIDARG; }
        if(terrestrialCombined_) {
            if(!Supported(p) || p->Frequency<90000 || p->Frequency>770000) return E_INVALIDARG;
            requestedCable_=*selected==tbs6812::DeliverySystem::J83B;
        }
        if(!Cable() || fatal_ || !chip_ || chip_->Adopted()) return S_OK;
        if(!Supported(p) || p->Frequency<90000 || p->Frequency>770000) return E_INVALIDARG;
        cableRequested_=*p; substitutedParam_=p;
        p->Frequency=long(bootstrapFrequency_);
        p->Modulation.Modulation=BDA_MOD_ISDB_T_TMCC;
        p->Modulation.SymbolRate=-1; p->Modulation.BandWidth=6;
        p->Modulation.InnerFEC=BDA_FEC_VITERBI; p->Modulation.InnerFECRate=BDA_BCC_RATE_3_4;
        p->Modulation.OuterFEC=BDA_FEC_RS_204_188; p->Modulation.OuterFECRate=BDA_BCC_RATE_NOT_SET;
        p->ONID=-1; p->TSID=-1; p->SID=-1;
        Log("J.83B bootstrap: substitute complete ISDB-T tuning parameters at RF_kHz="+std::to_string(bootstrapFrequency_));
        return S_OK;
    }
    const HRESULT PostLockChannel(const TuningParam*) override {
        std::lock_guard<std::mutex> guard(mutex_);
        if(substitutedParam_ && cableRequested_) { *substitutedParam_=*cableRequested_; substitutedParam_=nullptr; }
        if(!bootstrapPending_ || !chip_ || fatal_) return S_OK;
        bootstrapPending_=false;
        try {
            bool locked=chip_->Locked();
            // A card that just restarted can lock shortly after the vendor tune returns.
            for(const auto start=std::chrono::steady_clock::now();!locked && routing_.mixed && Terrestrial() && std::chrono::steady_clock::now()-start<std::chrono::seconds(2);locked=chip_->Locked()) Sleep(20);
            if(!locked) {
                if(!Terrestrial()) throw std::runtime_error("initial vendor tune did not lock; close/reopen before retrying");
                Log("ISDB-T vendor bootstrap did not lock: no takeover; next channel will retry vendor preparation");
                return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            }
            chip_->Adopt(Cable()?bootstrapFrequency_:IF(bootstrap_),Stream(bootstrap_));
            originalRecord_=tbs6812::WarmStartRecord{IF(bootstrap_),Stream(bootstrap_),system_};
            if(Cable()) {
                try {
                    SelectCore();
                    chip_->Tune(IF(bootstrap_),0);
                    const auto start=std::chrono::steady_clock::now();
                    do {
                        if(chip_->Locked()) { Log("J.83B 5.6 Msym/s locked after ISDB-T preparation"); return S_OK; }
                        Sleep(5);
                    } while(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(lockTimeout_));
                    Log("J.83B did not lock; check optical signal and channel frequency; later direct tunes remain available");
                    return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                } catch(...) { fatal_=true; throw; }
            }
            Inspect("after-vendor");
            Log("bootstrap adopted: CXD2857ER already active; no chip reset or clock reinitialization");
            return S_OK;
        } catch(const std::exception& e) { fatal_=true; return Error("bootstrap adoption",e); }
    }
    const HRESULT GetSignalState(int* strength,int* quality,int* lock) override {
        std::lock_guard<std::mutex> guard(mutex_);
        if(strength) *strength=0; if(quality) *quality=0; if(lock) *lock=0;
        if(!chip_ || !chip_->Adopted()) return fatal_?E_FAIL:E_NOINTERFACE;
        try {
            const bool locked=chip_->Locked(); if(lock) *lock=locked;
            if(locked && (strength || quality)) { try { const float cnr=chip_->CNR(); if(strength) *strength=int(cnr*1000); if(quality) *quality=std::clamp(int(cnr*100/30),0,100); } catch(...) {} }
            return S_OK;
        } catch(const std::exception& e) { return Error("read lock",e); }
    }
    const HRESULT GetSignalStrength(float* value) override {
        std::lock_guard<std::mutex> guard(mutex_);
        if(!value) return E_POINTER;
        *value=0; if(!chip_ || !chip_->Adopted()) return E_NOINTERFACE;
        if(Cable()) Inspect("J83B-signal-check");
        try { if(chip_->Locked()) *value=chip_->CNR(); return S_OK; }
        catch(...) { return S_OK; } // unavailable C/N is not a fabricated measurement
    }
    const HRESULT FinalizeHook() override {
        std::lock_guard<std::mutex> guard(mutex_);
        if(finalized_) return S_OK;
        finalized_=true;
        try { if(chip_) chip_->Restore(); Inspect("after-restore"); if(!fatal_) SaveCache(); Log("closed: bootstrap tuning and original output-pin settings restored; no board-mode write"); return S_OK; }
        catch(const std::exception& e) { return Error("restore vendor state",e); }
    }
    void Release() override { delete this; }
};
}

namespace {
const GUID networkProviderClass = {0xb2f3a67c,0x29da,0x4c78,{0x88,0x31,0x09,0x1e,0xd5,0x09,0xa4,0x75}};
const GUID tunerCategory = {0x71985f48,0x1ca1,0x11d3,{0x9c,0xc8,0x00,0xc0,0x4f,0x79,0x71,0xe0}};
struct BoardTuner { std::wstring display; CComPtr<IMoniker> moniker; };
std::string ModeHResult(const char* stage,HRESULT hr) { return std::string(stage)+": "+HResult(hr); }
bool ReadBoardModes(IKsPropertySet* property,unsigned slot,BYTE* modes) {
    DWORD returned=0;
    return SUCCEEDED(property->Get(accessSet,0x3c,nullptr,0,modes,8,&returned)) && returned>slot;
}
CComPtr<IPin> FirstPin(IBaseFilter* filter,PIN_DIRECTION wanted) {
    CComPtr<IEnumPins> pins;
    if(FAILED(filter->EnumPins(&pins))) return {};
    for(;;) {
        CComPtr<IPin> pin;
        if(pins->Next(1,&pin,nullptr)!=S_OK) return {};
        PIN_DIRECTION direction{};
        if(SUCCEEDED(pin->QueryDirection(&direction)) && direction==wanted) return pin;
    }
}
class BoardAccess { // Only the network provider and tuner: the capture chain will not build in the mode being left.
    CComPtr<IGraphBuilder> graph_;
    CComPtr<IBaseFilter> provider_,tuner_;
public:
    CComPtr<IKsPropertySet> property;
    explicit BoardAccess(IMoniker* moniker) {
        HRESULT hr=graph_.CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER);
        if(FAILED(hr)) throw std::runtime_error(ModeHResult("create filter graph",hr));
        hr=provider_.CoCreateInstance(networkProviderClass,nullptr,CLSCTX_INPROC_SERVER);
        if(SUCCEEDED(hr)) hr=graph_->AddFilter(provider_,L"Network Provider");
        if(FAILED(hr)) throw std::runtime_error(ModeHResult("add network provider",hr));
        hr=moniker->BindToObject(nullptr,nullptr,IID_IBaseFilter,reinterpret_cast<void**>(&tuner_));
        if(SUCCEEDED(hr)) hr=graph_->AddFilter(tuner_,L"Tuner");
        if(FAILED(hr)) throw std::runtime_error(ModeHResult("add tuner",hr));
        const auto output=FirstPin(provider_,PINDIR_OUTPUT), input=FirstPin(tuner_,PINDIR_INPUT);
        if(!output || !input) throw std::runtime_error("network provider or tuner has no usable pin");
        hr=graph_->Connect(output,input);
        if(FAILED(hr)) throw std::runtime_error(ModeHResult("connect network provider",hr));
        property=Properties(tuner_);
    }
};
class BdaBoardDriver final : public tbs6812::BoardModeDriver {
    std::wstring board_;
    DWORD timeout_;
    void (*closeGraph_)(void*);
    void* context_;
    HANDLE claims_[tbs6812::BoardSlots]{};
    BoardTuner Find(unsigned slot) const {
        CComPtr<ICreateDevEnum> devices; CComPtr<IEnumMoniker> monikers;
        HRESULT hr=devices.CoCreateInstance(CLSID_SystemDeviceEnum,nullptr,CLSCTX_INPROC);
        if(SUCCEEDED(hr)) hr=devices->CreateClassEnumerator(tunerCategory,&monikers,CDEF_DEVMON_PNP_DEVICE);
        if(hr!=S_OK) throw std::runtime_error(ModeHResult("enumerate BDA tuners",hr));
        for(;;) {
            CComPtr<IMoniker> moniker;
            if(monikers->Next(1,&moniker,nullptr)!=S_OK) break;
            LPOLESTR name=nullptr;
            if(FAILED(moniker->GetDisplayName(nullptr,nullptr,&name))) continue;
            const std::wstring display=Lower(name);
            CoTaskMemFree(name);
            CComPtr<IPropertyBag> bag; CComVariant friendly;
            if(FAILED(moniker->BindToStorage(nullptr,nullptr,IID_IPropertyBag,reinterpret_cast<void**>(&bag))) ||
                FAILED(bag->Read(L"FriendlyName",&friendly,nullptr)) || friendly.vt!=VT_BSTR) continue;
            if(Target(display.c_str(),friendly.bstrVal)==int(slot) && (board_.empty() || BoardPath(display.c_str())==board_))
                return {display,moniker};
        }
        throw std::runtime_error("TBS6812 tuner "+std::to_string(slot)+" is not present");
    }
public:
    BdaBoardDriver(std::wstring board,DWORD timeout,void (*closeGraph)(void*),void* context):board_(std::move(board)),timeout_(timeout),closeGraph_(closeGraph),context_(context) {}
    bool TryClaim(unsigned slot) override {
        // The same object BonDriver_BDA holds for an open tuner, visible across sessions.
        std::wstring name=Find(slot).display;
        std::replace(name.begin(),name.end(),L'\\',L'/');
        name=L"Global\\"+name;
        HANDLE claim=CreateSemaphoreW(nullptr,1,1,name.c_str());
        if(!claim) return false;
        if(WaitForSingleObject(claim,0)!=WAIT_OBJECT_0) { CloseHandle(claim); return false; }
        claims_[slot]=claim;
        return true;
    }
    void Release(unsigned slot) override {
        ReleaseSemaphore(claims_[slot],1,nullptr);
        CloseHandle(claims_[slot]);
        claims_[slot]=nullptr;
    }
    tbs6812::BoardModes Read(unsigned slot) override {
        BoardAccess access(Find(slot).moniker);
        BYTE modes[8]{};
        if(!ReadBoardModes(access.property,1,modes)) throw std::runtime_error("cannot read the board modes");
        return {modes[0],modes[1]};
    }
    void Write(unsigned slot,const tbs6812::BoardModes& modes) override {
        BoardAccess access(Find(slot).moniker);
        BYTE data[tbs6812::BoardSlots]{modes[0],modes[1]};
        WriteLog("board mode write: "+std::to_string(modes[0])+","+std::to_string(modes[1])+"; the card restarts");
        const HRESULT hr=access.property->Set(accessSet,0x3c,data,sizeof(data),data,sizeof(data));
        if(FAILED(hr)) throw std::runtime_error(ModeHResult("write the board modes",hr));
    }
    bool WaitFor(unsigned slot,const tbs6812::BoardModes& modes) override {
        const auto start=std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(timeout_)) {
            // The card leaves and returns; failures are expected until a matching reading.
            MsgWaitForMultipleObjects(0,nullptr,FALSE,500,QS_ALLINPUT);
            MSG message;
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            try {
                if(Read(slot)==modes) {
                    WriteLog("board mode ready after ms="+std::to_string(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()));
                    return true;
                }
            } catch(const std::exception&) {}
        }
        return false;
    }
    void CloseOwnGraph() override { if(closeGraph_) closeGraph_(context_); }
    int SlotOf(const std::wstring& display) const {
        for(unsigned slot=0;slot<tbs6812::BoardSlots;++slot) {
            try { if(Find(slot).display==display) return int(slot); } catch(const std::exception&) {}
        }
        return -1;
    }
};
}

// Keep thread notifications enabled: this project links the static CRT.
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) { if(reason==DLL_PROCESS_ATTACH) selfModule=module; return TRUE; }
extern "C" __declspec(dllexport) HRESULT CheckAndInitTuner(IBaseFilter*,const WCHAR* display,const WCHAR* friendly,const WCHAR*) {
    return Target(display,friendly)>=0?S_OK:E_INVALIDARG;
}
static DWORD BoardSwitchTimeout(const WCHAR* path) {
    return std::clamp<DWORD>(GetPrivateProfileIntW(L"TBS6812",L"BoardModeSwitchTimeoutMs",45000,path),5000,120000);
}
// CheckAndInitTuner for a graph serving one tuning space: skip inputs in the other board mode.
extern "C" __declspec(dllexport) HRESULT CheckAndInitTunerForSpace(IBaseFilter* tuner,const WCHAR* display,const WCHAR* friendly,const WCHAR* path,DWORD space) {
    const int slot=Target(display,friendly);
    if(slot<0 || !tuner) return E_INVALIDARG;
    try {
        const auto system=tbs6812::SystemRouting::Read(path).ForSpace(space);
        if(!system) return E_INVALIDARG;
        auto property=Properties(tuner);
        BYTE modes[8]{};
        if(!ReadBoardModes(property,unsigned(slot),modes)) return E_FAIL;
        return modes[slot]==tbs6812::BoardModeFor(*system) ? S_OK : E_INVALIDARG;
    } catch(const std::exception& e) { WriteLog(std::string("check tuner mode: ")+e.what()); return E_FAIL; }
}
// Prepares an input in a space's board mode when no other receiver holds the card.
// HRESULT_FROM_WIN32(ERROR_BUSY) before closeGraph runs leaves the caller's graph untouched.
extern "C" __declspec(dllexport) HRESULT AlignBoardForSpace(const WCHAR* path,DWORD space,const WCHAR* ownDisplay,void (*closeGraph)(void*),void* context,BOOL* switched) {
    if(switched) *switched=FALSE;
    try {
        const auto system=tbs6812::SystemRouting::Read(path).ForSpace(space);
        if(!system) return E_INVALIDARG;
        const auto timeout=BoardSwitchTimeout(path);
        const auto own=Lower(ownDisplay?ownDisplay:L"");
        BdaBoardDriver driver(BoardPath(own.c_str()),timeout,closeGraph,context);
        const int ownSlot=own.empty() ? -1 : driver.SlotOf(own);
        if(!own.empty() && ownSlot<0) return E_INVALIDARG;
        const bool rewritten=tbs6812::AlignBoardModes(driver,ownSlot,tbs6812::BoardModeFor(*system));
        if(switched) *switched=rewritten;
        return S_OK;
    }
    catch(const tbs6812::BoardBusy& e) { WriteLog(std::string("board mode: ")+e.what()); return HRESULT_FROM_WIN32(ERROR_BUSY); }
    catch(const std::exception& e) { WriteLog(std::string("board mode: ")+e.what()); return E_FAIL; }
}
// Returns the card to BoardModeHome after this receiver closed. A busy card stays as it is.
extern "C" __declspec(dllexport) HRESULT RestoreBoardHome(const WCHAR* path,const WCHAR* boardDisplay) {
    try {
        wchar_t text[16]{};
        GetPrivateProfileStringW(L"TBS6812",L"BoardModeHome",L"",text,16,path);
        if(!*text) return S_FALSE;
        const auto home=tbs6812::ParseBoardHome(text);
        if(!home) { WriteLog("board mode: BoardModeHome must be like S,S or T,S"); return E_INVALIDARG; }
        const auto display=Lower(boardDisplay?boardDisplay:L"");
        BdaBoardDriver driver(BoardPath(display.c_str()),BoardSwitchTimeout(path),nullptr,nullptr);
        return tbs6812::RestoreHomeModes(driver,*home) ? S_OK : S_FALSE;
    } catch(const std::exception& e) { WriteLog(std::string("board mode: ")+e.what()); return E_FAIL; }
}
extern "C" __declspec(dllexport) HRESULT CheckCapture(const WCHAR* tunerDisplay,const WCHAR* tunerFriendly,const WCHAR* captureDisplay,const WCHAR* captureFriendly,const WCHAR*) {
    const int index=Target(tunerDisplay,tunerFriendly);
    const auto board=BoardPath(tunerDisplay);
    return index>=0 && Target(captureDisplay,captureFriendly,L"capture")==index &&
        !board.empty() && board==BoardPath(captureDisplay) ? S_OK : E_INVALIDARG;
}
extern "C" __declspec(dllexport) IBdaSpecials* CreateBdaSpecials2(CComPtr<IBaseFilter> tuner,CComPtr<IBaseFilter>,const WCHAR* display,const WCHAR* friendly,const WCHAR*,const WCHAR*) {
    try { return new Specials(tuner,Target(display,friendly),display); } catch(...) { return nullptr; }
}
