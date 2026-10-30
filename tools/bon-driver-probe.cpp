#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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
    DWORD space = ~0UL, channel = ~0UL;
    for (DWORD s = 0; s < 100; ++s) {
        if (!driver2->EnumTuningSpace(s)) continue;
        for (DWORD c = 0; c < 1000; ++c) {
            const auto* name = driver2->EnumChannelName(s, c);
            if (name && std::wstring(name).find(argv[2]) != std::wstring::npos) { space = s; channel = c; break; }
        }
        if (space != ~0UL) break;
    }
    if (space == ~0UL) { driver->CloseTuner(); driver->Release(); FreeLibrary(module); return 7; }
    std::cout << "space=" << space << " channel=" << channel << std::endl;
    start = Clock::now();
    const bool tuned = driver2->SetChannel(space, channel) != FALSE;
    std::cout << "tune_ok=" << tuned << " tune_ms=" << elapsed(start) << std::endl;
    std::ofstream output(outputPath, std::ios::binary);
    if (!output) { driver->CloseTuner(); driver->Release(); FreeLibrary(module); return 8; }
    const auto deadline = Clock::now() + std::chrono::seconds(std::stoi(argv[3]));
    unsigned long long total = 0, nonFF = 0;
    while (Clock::now() < deadline) {
        driver->WaitTsStream(100);
        DWORD remain = 0;
        do {
            BYTE* data = nullptr; DWORD size = 0;
            if (!driver->GetTsStream(&data, &size, &remain) || !data || !size) break;
            if (!total) std::cout << "first_data_ms=" << elapsed(start) << std::endl;
            output.write(reinterpret_cast<const char*>(data), size);
            total += size;
            for (DWORD i = 0; i < size; ++i) if (data[i] != 0xff) ++nonFF;
        } while (remain);
    }
    output.close();
    std::cout << "bytes=" << total << " non_ff_bytes=" << nonFF << " signal=" << driver->GetSignalLevel() << std::endl;
    driver->CloseTuner(); driver->Release(); FreeLibrary(module);
    return tuned && total && nonFF ? 0 : 9;
}
