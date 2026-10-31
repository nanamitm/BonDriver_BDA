// SPDX-License-Identifier: GPL-2.0-or-later
// Reads the TBS6812 board modes, or writes them while both inputs are free. Writing restarts the card.
#define NOMINMAX
#include <windows.h>
#include <atlbase.h>
#include <dshow.h>
#include <ks.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#pragma comment(lib,"strmiids")
#pragma comment(lib,"ole32")
#pragma comment(lib,"oleaut32")

namespace {
const GUID accessSet={0xfaa8f3e5,0x31d4,0x4e41,{0x88,0xef,0xd9,0xeb,0x71,0x6f,0x6e,0xc9}};
const GUID providerClass={0xb2f3a67c,0x29da,0x4c78,{0x88,0x31,0x09,0x1e,0xd5,0x09,0xa4,0x75}};
const GUID tunerCategory={0x71985f48,0x1ca1,0x11d3,{0x9c,0xc8,0x00,0xc0,0x4f,0x79,0x71,0xe0}};
struct Tuner { std::wstring display; CComPtr<IMoniker> moniker; };
std::wstring Lower(std::wstring value) { std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return wchar_t(towlower(c));}); return value; }
bool Find(int slot,Tuner& found) {
    CComPtr<ICreateDevEnum> devices; CComPtr<IEnumMoniker> monikers;
    if(FAILED(devices.CoCreateInstance(CLSID_SystemDeviceEnum)) || devices->CreateClassEnumerator(tunerCategory,&monikers,CDEF_DEVMON_PNP_DEVICE)!=S_OK) return false;
    for(;;) {
        CComPtr<IMoniker> moniker;
        if(monikers->Next(1,&moniker,nullptr)!=S_OK) return false;
        LPOLESTR name=nullptr;
        if(FAILED(moniker->GetDisplayName(nullptr,nullptr,&name))) continue;
        const auto display=Lower(name); CoTaskMemFree(name);
        CComPtr<IPropertyBag> bag; CComVariant text;
        if(FAILED(moniker->BindToStorage(nullptr,nullptr,IID_IPropertyBag,reinterpret_cast<void**>(&bag))) || FAILED(bag->Read(L"FriendlyName",&text,nullptr)) || text.vt!=VT_BSTR) continue;
        const auto friendly=Lower(text.bstrVal);
        const auto suffix=L"tuner "+std::to_wstring(slot);
        if(display.find(L"subsys_00016812")!=display.npos && friendly.size()>=suffix.size() && friendly.compare(friendly.size()-suffix.size(),suffix.size(),suffix)==0) { found={display,moniker}; return true; }
    }
}
CComPtr<IPin> Pin(IBaseFilter* filter,PIN_DIRECTION wanted) {
    CComPtr<IEnumPins> pins;
    if(FAILED(filter->EnumPins(&pins))) return {};
    for(;;) {
        CComPtr<IPin> pin; PIN_DIRECTION direction{};
        if(pins->Next(1,&pin,nullptr)!=S_OK) return {};
        if(SUCCEEDED(pin->QueryDirection(&direction)) && direction==wanted) return pin;
    }
}
struct Access {
    CComPtr<IGraphBuilder> graph; CComPtr<IBaseFilter> provider,tuner; CComPtr<IKsPropertySet> property;
    bool Open(IMoniker* moniker) {
        if(FAILED(graph.CoCreateInstance(CLSID_FilterGraph)) || FAILED(provider.CoCreateInstance(providerClass)) || FAILED(graph->AddFilter(provider,L"Provider"))) return false;
        if(FAILED(moniker->BindToObject(nullptr,nullptr,IID_IBaseFilter,reinterpret_cast<void**>(&tuner))) || FAILED(graph->AddFilter(tuner,L"Tuner"))) return false;
        auto output=Pin(provider,PINDIR_OUTPUT), input=Pin(tuner,PINDIR_INPUT);
        if(!output || !input || FAILED(graph->Connect(output,input))) return false;
        property=CComQIPtr<IKsPropertySet>(input);
        return property!=nullptr;
    }
};
bool Read(int slot,BYTE* modes) {
    Tuner tuner; Access access; DWORD returned=0;
    return Find(slot,tuner) && access.Open(tuner.moniker) && SUCCEEDED(access.property->Get(accessSet,0x3c,nullptr,0,modes,8,&returned)) && returned>=2;
}
HANDLE Claim(int slot) {
    Tuner tuner;
    if(!Find(slot,tuner)) return nullptr;
    auto name=tuner.display; std::replace(name.begin(),name.end(),L'\\',L'/');
    HANDLE claim=CreateSemaphoreW(nullptr,1,1,(L"Global\\"+name).c_str());
    if(claim && WaitForSingleObject(claim,0)==WAIT_OBJECT_0) return claim;
    if(claim) CloseHandle(claim);
    return nullptr;
}
}
int wmain(int argc,wchar_t** argv) {
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 2;
    HANDLE first=Claim(0), second=Claim(1);
    std::cout<<"free slot0="<<(first!=nullptr)<<" slot1="<<(second!=nullptr)<<std::endl;
    int result=0; BYTE modes[8]{};
    if(!first || !second) { std::cerr<<"both inputs must be free"<<std::endl; result=3; }
    else if(!Read(0,modes)) { std::cerr<<"cannot read modes"<<std::endl; result=4; }
    else {
        std::cout<<"modes="<<int(modes[0])<<","<<int(modes[1])<<std::endl;
        if(argc==4 && std::wstring(argv[1])==L"write") {
            BYTE wanted[2]{BYTE(_wtoi(argv[2])),BYTE(_wtoi(argv[3]))};
            if((wanted[0]!=1 && wanted[0]!=2) || (wanted[1]!=1 && wanted[1]!=2)) result=5;
            else if(wanted[0]!=modes[0] || wanted[1]!=modes[1]) {
                { Tuner tuner; Access access;
                  if(!Find(0,tuner) || !access.Open(tuner.moniker) || FAILED(access.property->Set(accessSet,0x3c,wanted,2,wanted,2))) result=6; }
                const auto start=std::chrono::steady_clock::now(); bool ready=false;
                while(!result && !ready && std::chrono::steady_clock::now()-start<std::chrono::seconds(60)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    BYTE now[8]{}; ready=Read(0,now) && now[0]==wanted[0] && now[1]==wanted[1];
                }
                std::cout<<"ready="<<ready<<" ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<std::endl;
                if(!result && !ready) result=7;
            }
        }
    }
    if(first) { ReleaseSemaphore(first,1,nullptr); CloseHandle(first); }
    if(second) { ReleaseSemaphore(second,1,nullptr); CloseHandle(second); }
    CoUninitialize();
    return result;
}
