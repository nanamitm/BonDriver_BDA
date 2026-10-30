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

namespace {
const GUID accessSet = {0xfaa8f3e5,0x31d4,0x4e41,{0x88,0xef,0xd9,0xeb,0x71,0x6f,0x6e,0xc9}};
HMODULE selfModule = nullptr;
std::mutex logMutex;
void Log(const std::string& message) {
    std::lock_guard<std::mutex> guard(logMutex);
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(selfModule,path,MAX_PATH);
    std::wstring name(path); const auto dot=name.rfind(L'.'); if(dot!=std::wstring::npos) name.resize(dot); name+=L".log";
    FILE* file=nullptr; _wfopen_s(&file,name.c_str(),L"ab");
    SYSTEMTIME now{}; GetLocalTime(&now);
    if(file) { fprintf(file,"%02u:%02u:%02u.%03u %s\n",now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,message.c_str()); fclose(file); }
    OutputDebugStringA(("TBS6812: "+message+"\n").c_str());
}
std::string HResult(HRESULT hr) { std::ostringstream out; out<<"HRESULT=0x"<<std::hex<<std::setw(8)<<std::setfill('0')<<uint32_t(hr); return out.str(); }
std::wstring Lower(std::wstring value) { std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){ return wchar_t(towlower(c)); }); return value; }
bool Target(const wchar_t* display,const wchar_t* friendly) {
    const std::wstring d=Lower(display?display:L""), n=Lower(friendly?friendly:L"");
    return d.find(L"ven_544d")!=d.npos && d.find(L"dev_6178")!=d.npos && d.find(L"subsys_00016812")!=d.npos &&
        n.find(L"6812")!=n.npos && n.size()>=7 && n.compare(n.size()-7,7,L"tuner 0")==0;
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
    bool validTarget_, fatal_=false, bootstrapPending_=false, finalized_=false;
    DWORD lockTimeout_=3000;
    TuningParam bootstrap_;
    HANDLE claim_=nullptr;
    static uint32_t IF(const TuningParam& p) { return tbs6812::Cxd2857::IntermediateFrequency(p.Frequency,p.Antenna.LowOscillator); }
    bool Supported(const TuningParam* p) {
        return p && tbs6812::Cxd2857::IsS3(p->TSID) && (p->Polarisation==BDA_POLARISATION_CIRCULAR_R || p->Polarisation==BDA_POLARISATION_CIRCULAR_L) && p->Antenna.Tone==0 && p->Antenna.DiSEqC<=0;
    }
    const HRESULT Error(const char* stage,const std::exception& e) { Log(std::string(stage)+": "+e.what()); return E_FAIL; }
public:
    Specials(IBaseFilter* tuner,bool target):tuner_(tuner),validTarget_(target) {}
    ~Specials() { if(claim_) CloseHandle(claim_); }
    const HRESULT ReadIniFile(const WCHAR* path) override {
        lockTimeout_=std::clamp<DWORD>(GetPrivateProfileIntW(L"TBS6812",L"LockTimeoutMs",3000,path),100,10000);
        return S_OK;
    }
    const HRESULT InitializeHook() override {
        std::lock_guard<std::mutex> guard(mutex_);
        try {
            if(!validTarget_) throw std::runtime_error("only PCI TBS6812 Tuner 0 is supported");
            claim_=CreateMutexW(nullptr,FALSE,L"Local\\BonDriver_TBS6812_Direct_Tuner0");
            if(!claim_ || GetLastError()==ERROR_ALREADY_EXISTS) throw std::runtime_error("Tuner 0 direct-control extension is already open");
            property_=Properties(tuner_);
            DWORD support=0; HRESULT hr=property_->QuerySupported(accessSet,0x19,&support);
            if(FAILED(hr) || (support&(KSPROPERTY_SUPPORT_GET|KSPROPERTY_SUPPORT_SET))!=(KSPROPERTY_SUPPORT_GET|KSPROPERTY_SUPPORT_SET)) throw std::runtime_error("raw I2C property 0x19 is unavailable");
            bus_=std::make_unique<KsBus>(property_); chip_=std::make_unique<tbs6812::Cxd2857>(*bus_);
            BYTE modes[8]{}; DWORD returned=0;
            hr=property_->Get(accessSet,0x3c,nullptr,0,modes,sizeof(modes),&returned);
            if(FAILED(hr) || modes[0]!=2) throw std::runtime_error("Tuner 0 must already be in satellite mode; no card-wide mode changes are performed");
            Log("initialized: warm ISDB-S3 takeover; vendor bootstrap required");
            return S_OK;
        } catch(const std::exception& e) { fatal_=true; return Error("initialize",e); }
    }
    const HRESULT SetLNBPower(bool active) override {
        if(!property_) return E_FAIL;
        BYTE request[0x94]{}; const DWORD value=active?2:0; // 13 V or off
        memcpy(request+0x0c,&value,sizeof(value));
        return property_->Set(accessSet,0x15,nullptr,0,request,sizeof(request));
    }
    const HRESULT Set22KHz(bool active) override { return active?E_INVALIDARG:S_OK; }
    const HRESULT Set22KHz(long tone) override { return tone==0?S_OK:E_INVALIDARG; }
    const HRESULT SetTSid(long id) override {
        if(!property_ || !tbs6812::Cxd2857::IsS3(id)) return E_INVALIDARG;
        return property_->Set(accessSet,0x60,&id,sizeof(id),&id,sizeof(id));
    }
    const HRESULT LockChannel(const TuningParam* p) override {
        std::lock_guard<std::mutex> guard(mutex_);
        if(fatal_ || finalized_ || !chip_) return E_FAIL;
        if(!Supported(p)) { Log("rejected: this extension accepts circular ISDB-S3 channels with tone off only"); return E_INVALIDARG; }
        try {
            const auto frequency=IF(*p);
            if(!chip_->Adopted()) {
                bootstrap_=*p; bootstrapPending_=true;
                const HRESULT hr=SetLNBPower(true); if(FAILED(hr)) throw std::runtime_error("LNB preparation failed: "+HResult(hr));
                Log("bootstrap: returning E_NOINTERFACE for the initial vendor tune only");
                return E_NOINTERFACE;
            }
            const auto start=std::chrono::steady_clock::now();
            chip_->Tune(frequency,uint16_t(p->TSID));
            do {
                if(chip_->Locked()) {
                    std::ostringstream s; s<<"direct tune IF_kHz="<<frequency<<" stream_id=0x"<<std::hex<<p->TSID<<std::dec<<" lock_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); Log(s.str());
                    return S_OK;
                }
                Sleep(5);
            } while(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(lockTimeout_));
            Log("direct tune timed out: no ISDB-S3 lock; vendor tune was not resubmitted");
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        } catch(const std::exception& e) { return Error("direct tune",e); }
    }
    const HRESULT PostLockChannel(const TuningParam*) override {
        std::lock_guard<std::mutex> guard(mutex_);
        if(!bootstrapPending_ || !chip_ || fatal_) return S_OK;
        bootstrapPending_=false;
        try {
            if(!chip_->Locked()) throw std::runtime_error("initial vendor tune did not lock; close/reopen before retrying");
            chip_->Adopt(IF(bootstrap_),uint16_t(bootstrap_.TSID));
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
        try { if(chip_->Locked()) *value=chip_->CNR(); return S_OK; }
        catch(...) { return S_OK; } // unavailable C/N is not a fabricated measurement
    }
    const HRESULT FinalizeHook() override {
        std::lock_guard<std::mutex> guard(mutex_);
        if(finalized_) return S_OK;
        finalized_=true;
        try { if(chip_) chip_->Restore(); Log("closed: bootstrap tuning and original output-pin settings restored; no board-mode write"); return S_OK; }
        catch(const std::exception& e) { return Error("restore vendor state",e); }
    }
    void Release() override { delete this; }
};
}

// Keep thread notifications enabled: this project links the static CRT.
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) { if(reason==DLL_PROCESS_ATTACH) selfModule=module; return TRUE; }
extern "C" __declspec(dllexport) HRESULT CheckAndInitTuner(IBaseFilter*,const WCHAR* display,const WCHAR* friendly,const WCHAR*) {
    return Target(display,friendly)?S_OK:E_INVALIDARG;
}
extern "C" __declspec(dllexport) IBdaSpecials* CreateBdaSpecials2(CComPtr<IBaseFilter> tuner,CComPtr<IBaseFilter>,const WCHAR* display,const WCHAR* friendly,const WCHAR*,const WCHAR*) {
    try { return new Specials(tuner,Target(display,friendly)); } catch(...) { return nullptr; }
}
