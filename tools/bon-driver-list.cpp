// Read and enumerate the BonDriver INI without opening any tuner.
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <iostream>
#include "IBonDriver2.h"

int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    const auto path=std::filesystem::absolute(argv[1]);
    SetDllDirectoryW(path.parent_path().c_str());
    const auto module=LoadLibraryW(path.c_str());
    if(!module) return 3;
    const auto factory=reinterpret_cast<IBonDriver*(*)()>(GetProcAddress(module,"CreateBonDriver"));
    if(!factory) { FreeLibrary(module); return 4; }
    auto* driver=factory();
    auto* extended=dynamic_cast<IBonDriver2*>(driver);
    if(!extended) { if(driver) driver->Release(); FreeLibrary(module); return 5; }
    unsigned count=0;
    for(DWORD space=0;space<100;++space) {
        if(!extended->EnumTuningSpace(space)) continue;
        for(DWORD channel=0;channel<1000;++channel) {
            const auto* name=extended->EnumChannelName(space,channel);
            if(!name) break;
            std::wcout<<space<<L'\t'<<channel<<L'\t'<<name<<L'\n';
            ++count;
        }
    }
    driver->Release();
    FreeLibrary(module);
    return count ? 0 : 6;
}
