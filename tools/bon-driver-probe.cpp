#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "IBonDriver2.h"

using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) {
        std::wcerr << L"Usage: bon-driver-probe DLL channel-name seconds output-file\n";
        return 2;
    }
    const auto dll = std::filesystem::absolute(argv[1]);
    const auto outputPath = std::filesystem::absolute(argv[4]);
    SetDllDirectoryW(dll.parent_path().c_str());
    const auto module = LoadLibraryW(dll.c_str());
    if (!module) { std::cerr << "LoadLibrary error=" << GetLastError() << std::endl; return 3; }
    const auto factory = reinterpret_cast<IBonDriver* (*)()>(GetProcAddress(module, "CreateBonDriver"));
    if (!factory) { FreeLibrary(module); return 4; }
    auto* driver = factory();
    auto* driver2 = dynamic_cast<IBonDriver2*>(driver);
    if (!driver2) { if (driver) driver->Release(); FreeLibrary(module); return 5; }
    auto start = Clock::now();
    const bool opened = driver->OpenTuner() != FALSE;
    std::cout << "open_ok=" << opened << " open_ms=" << elapsed(start) << std::endl;
    if (!opened) { driver->Release(); FreeLibrary(module); return 6; }
    std::vector<std::wstring> requested;
    std::wstring names(argv[2]); size_t offset=0;
    do { const auto comma=names.find(L',',offset); requested.push_back(names.substr(offset,comma==names.npos?comma:comma-offset)); if(comma==names.npos) break; offset=comma+1; } while(offset<names.size());
    bool success=true;
    for(size_t round=0;round<requested.size();++round) {
        DWORD space=~0UL,channel=~0UL;
        for(DWORD s=0;s<100;++s) {
            if(!driver2->EnumTuningSpace(s)) continue;
            for(DWORD c=0;c<1000;++c) { const auto* name=driver2->EnumChannelName(s,c); if(name && std::wstring(name).find(requested[round])!=std::wstring::npos) { space=s; channel=c; break; } }
            if(space!=~0UL) break;
        }
        if(space==~0UL) { std::cerr<<"channel not found at round="<<round+1<<std::endl; success=false; break; }
        std::cout<<"round="<<round+1<<" space="<<space<<" channel="<<channel<<std::endl;
        start=Clock::now(); const bool tuned=driver2->SetChannel(space,channel)!=FALSE;
        std::cout<<"tune_ok="<<tuned<<" tune_ms="<<elapsed(start)<<std::endl;
        auto capturePath=outputPath;
        if(requested.size()>1) capturePath=outputPath.parent_path()/(outputPath.stem().wstring()+L"-"+std::to_wstring(round+1)+outputPath.extension().wstring());
        std::ofstream output(capturePath,std::ios::binary);
        if(!output) { success=false; break; }
        const auto deadline=Clock::now()+std::chrono::seconds(std::stoi(argv[3]));
        unsigned long long total=0,nonFF=0;
        while(Clock::now()<deadline) {
            driver->WaitTsStream(100); DWORD remain=0;
            do {
                BYTE* data=nullptr; DWORD size=0;
                if(!driver->GetTsStream(&data,&size,&remain) || !data || !size) break;
                if(!total) std::cout<<"first_data_ms="<<elapsed(start)<<std::endl;
                output.write(reinterpret_cast<const char*>(data),size); total+=size;
                for(DWORD i=0;i<size;++i) if(data[i]!=0xff) ++nonFF;
            } while(remain);
        }
        output.close(); std::cout<<"bytes="<<total<<" non_ff_bytes="<<nonFF<<" signal="<<driver->GetSignalLevel()<<std::endl;
        if(!tuned || !total || !nonFF) { success=false; break; }
    }
    std::cout<<"close_begin"<<std::endl;
    driver->CloseTuner(); std::cout<<"close_done"<<std::endl;
    driver->Release(); std::cout<<"release_done"<<std::endl;
    FreeLibrary(module); std::cout<<"unload_done"<<std::endl;
    return success?0:9;
}
