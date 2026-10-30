#include <windows.h>
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    HMODULE module=LoadLibraryW(argv[1]); if(!module) return 3;
    using Check=HRESULT(*)(void*,const WCHAR*,const WCHAR*,const WCHAR*);
    auto check=reinterpret_cast<Check>(GetProcAddress(module,"CheckAndInitTuner"));
    const wchar_t* correct=L"pci#ven_544d&dev_6178&subsys_00016812";
    const bool passed=check &&
        check(nullptr,correct,L"TBS 6812 ISDB-T/S/S3 Tuner 0",L"")==S_OK &&
        check(nullptr,correct,L"TBS 6812 ISDB-T/S/S3 Tuner 1",L"")==S_OK &&
        FAILED(check(nullptr,correct,L"TBS 6812 ISDB-T/S/S3 Tuner 2",L"")) &&
        FAILED(check(nullptr,L"pci#ven_544d&dev_6178&subsys_00016814",L"TBS 6812 Tuner 0",L"")) &&
        FAILED(check(nullptr,nullptr,nullptr,L""));
    using PairCheck=HRESULT(*)(const WCHAR*,const WCHAR*,const WCHAR*,const WCHAR*,const WCHAR*);
    auto pair=reinterpret_cast<PairCheck>(GetProcAddress(module,"CheckCapture"));
    const wchar_t* tuner=L"@device:pnp:\\\\?\\pci#ven_544d&dev_6178&subsys_00016812#card-a#{tuner-guid}\\pin";
    const wchar_t* capture=L"@device:pnp:\\\\?\\pci#ven_544d&dev_6178&subsys_00016812#card-a#{capture-guid}\\pin";
    const wchar_t* other=L"@device:pnp:\\\\?\\pci#ven_544d&dev_6178&subsys_00016812#card-b#{capture-guid}\\pin";
    const bool pairs=pair &&
        pair(tuner,L"TBS 6812 Tuner 0",capture,L"TBS 6812 Capture 0",L"")==S_OK &&
        pair(tuner,L"TBS 6812 Tuner 1",capture,L"TBS 6812 Capture 1",L"")==S_OK &&
        FAILED(pair(tuner,L"TBS 6812 Tuner 0",capture,L"TBS 6812 Capture 1",L"")) &&
        FAILED(pair(tuner,L"TBS 6812 Tuner 1",capture,L"TBS 6812 Capture 0",L"")) &&
        FAILED(pair(tuner,L"TBS 6812 Tuner 0",other,L"TBS 6812 Capture 0",L"")) &&
        FAILED(pair(nullptr,L"TBS 6812 Tuner 0",nullptr,L"TBS 6812 Capture 0",L""));
    FreeLibrary(module);
    std::cout<<(passed && pairs?"PASS":"FAIL")<<": tuner identity and capture matching reject invalid slots, PCI models, cross-slot/cross-board pairs and missing identity\n";
    return passed && pairs?0:1;
}
