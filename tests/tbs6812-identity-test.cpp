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
        FAILED(check(nullptr,correct,L"TBS 6812 ISDB-T/S/S3 Tuner 1",L"")) &&
        FAILED(check(nullptr,L"pci#ven_544d&dev_6178&subsys_00016814",L"TBS 6812 Tuner 0",L"")) &&
        FAILED(check(nullptr,nullptr,nullptr,L""));
    FreeLibrary(module);
    std::cout<<(passed?"PASS":"FAIL")<<": extension rejects Tuner 1, wrong PCI model and missing identity before hardware access\n";
    return passed?0:1;
}
