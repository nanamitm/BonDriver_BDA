// Drives BonDriverProxy.dll -> BonDriverHost.exe -> a mock BonDriver, and terminates
// the client in the ways that would stop a real graph out of order.
// Usage: bon-driver-host-test <folder with BonDriver_Mock.dll>
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "IBonDriver2.h"

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << __LINE__ << ": " #condition << std::endl; ++failures; } } while (0)

std::wstring folder;
std::wstring logPath;

using Factory = IBonDriver * (*)();

Factory LoadFactory(const std::wstring & name)
{
    const HMODULE module = LoadLibraryW((folder + L"\\" + name).c_str());
    return module ? reinterpret_cast<Factory>(GetProcAddress(module, "CreateBonDriver")) : nullptr;
}

void NewLog(const wchar_t * name)
{
    logPath = folder + L"\\" + name;
    DeleteFileW(logPath.c_str());
    SetEnvironmentVariableW(L"BONDRIVER_MOCK_LOG", logPath.c_str());
}

std::vector<std::string> ReadLog()
{
    std::vector<std::string> lines;
    std::ifstream file(logPath);
    for (std::string line; std::getline(file, line);) lines.push_back(line);
    return lines;
}

bool Contains(const std::vector<std::string> & lines, const char * line)
{
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

bool WaitForRelease()
{
    for (int i = 0; i < 100; ++i) {
        if (Contains(ReadLog(), "release")) return true;
        Sleep(100);
    }
    return false;
}

bool ReadChunk(IBonDriver * driver, DWORD * sequence, DWORD * purges)
{
    for (int i = 0; i < 100; ++i) {
        driver->WaitTsStream(100);
        BYTE * data = nullptr; DWORD size = 0, remain = 0;
        if (driver->GetTsStream(&data, &size, &remain) && data && size >= 8) {
            *sequence = reinterpret_cast<DWORD *>(data)[0];
            *purges = reinterpret_cast<DWORD *>(data)[1];
            return true;
        }
    }
    return false;
}

void TestForwarding(Factory factory)
{
    NewLog(L"forwarding.log");
    auto * driver = factory();
    CHECK(driver != nullptr);
    if (!driver) return;
    auto * driver2 = dynamic_cast<IBonDriver2 *>(driver);
    CHECK(driver2 != nullptr);
    if (!driver2) return;

    CHECK(std::wstring(driver2->GetTunerName()) == L"Mock tuner");
    CHECK(std::wstring(driver2->EnumTuningSpace(0)) == L"UHF");
    CHECK(driver2->EnumTuningSpace(1) == nullptr);
    const auto * name = driver2->EnumChannelName(0, 1);
    CHECK(std::wstring(name) == L"Channel1");
    CHECK(driver2->EnumChannelName(0, 1) == name);
    CHECK(driver2->EnumChannelName(0, 2) == nullptr);

    CHECK(driver->WaitTsStream(10) == WAIT_ABANDONED);
    CHECK(driver->OpenTuner() == TRUE);
    CHECK(driver2->IsTunerOpening() == TRUE);
    CHECK(driver2->SetChannel(0UL, 1UL) == TRUE);
    CHECK(driver2->GetCurSpace() == 0 && driver2->GetCurChannel() == 1);
    CHECK(driver2->SetChannel(5UL, 0UL) == FALSE);
    CHECK(driver->GetSignalLevel() == 12.5F);

    // Chunks arrive complete and in order
    driver->PurgeTsStream();
    DWORD previous = 0, sequence = 0, purges = 0;
    CHECK(ReadChunk(driver, &previous, &purges));
    for (int i = 0; i < 100; ++i) {
        CHECK(ReadChunk(driver, &sequence, &purges));
        CHECK(sequence == previous + 1);
        previous = sequence;
    }

    // A slow reader must not deadlock a purge, and nothing older survives it
    Sleep(1500);
    DWORD before = 0;
    CHECK(ReadChunk(driver, &sequence, &before));
    driver->PurgeTsStream();
    CHECK(ReadChunk(driver, &sequence, &purges));
    CHECK(purges > before);

    driver->CloseTuner();
    CHECK(driver2->IsTunerOpening() == FALSE);
    CHECK(driver->WaitTsStream(10) == WAIT_ABANDONED);
    CHECK(Contains(ReadLog(), "close"));

    // Reopening in the same host, then a clean release that frees the tuner at once
    CHECK(driver->OpenTuner() == TRUE);
    CHECK(ReadChunk(driver, &sequence, &purges));
    driver->Release();
    const auto lines = ReadLog();
    CHECK(!lines.empty() && lines.back() == "release");
}

int RunChild(Factory factory)
{
    auto * driver = factory();
    if (!driver || !driver->OpenTuner()) return 1;
    DWORD sequence = 0, purges = 0;
    if (!ReadChunk(driver, &sequence, &purges)) return 1;
    std::cout << "receiving" << std::endl;
    for (;;) ReadChunk(driver, &sequence, &purges);
}

// Starts this program as a receiving client, and returns when it is receiving
PROCESS_INFORMATION StartClient(HANDLE job = nullptr)
{
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    CreatePipe(&readEnd, &writeEnd, &inherit, 0);
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    wchar_t self[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring commandLine = L"\"" + std::wstring(self) + L"\" \"" + folder + L"\" --child";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process = {};
    CHECK(CreateProcessW(self, &commandLine[0], nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process));
    if (job) CHECK(AssignProcessToJobObject(job, process.hProcess));
    ResumeThread(process.hThread);
    CloseHandle(writeEnd);

    char buffer[32] = {};
    DWORD got = 0;
    CHECK(ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0);
    CloseHandle(readEnd);
    return process;
}

// The mock tuner is exclusive and closes slowly, so this open only succeeds
// when it waits for the host that is still closing it
void CheckClosedAndReopen(Factory factory)
{
    Sleep(100);
    auto * driver = factory();
    CHECK(driver != nullptr);
    if (driver) {
        CHECK(driver->OpenTuner() == TRUE);
        const auto lines = ReadLog();
        CHECK(Contains(lines, "close"));
        CHECK(!Contains(lines, "open_busy"));
        driver->Release();
    }
    CHECK(WaitForRelease());
}

void TestTerminate(Factory factory)
{
    NewLog(L"terminate.log");
    const auto client = StartClient();
    TerminateProcess(client.hProcess, 1);
    CheckClosedAndReopen(factory);
}

void TestProcessTreeKill(Factory factory)
{
    NewLog(L"tree.log");
    const auto client = StartClient();
    const std::string command = "taskkill /F /T /PID " + std::to_string(client.dwProcessId) + " >nul";
    CHECK(std::system(command.c_str()) == 0);
    CheckClosedAndReopen(factory);
}

void TestJobKill(Factory factory)
{
    NewLog(L"job.log");
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    StartClient(job);
    CloseHandle(job);
    CheckClosedAndReopen(factory);
}

void TestMissingInner()
{
    const auto factory = LoadFactory(L"BonDriver_NoInner.dll");
    CHECK(factory != nullptr);
    if (factory) CHECK(factory() == nullptr);
}

}

int wmain(int argc, wchar_t ** argv)
{
    if (argc < 2) return 2;
    folder = argv[1];
    const auto factory = LoadFactory(L"BonDriver_Mock.dll");
    if (!factory) { std::cerr << "cannot load the proxy" << std::endl; return 3; }
    if (argc == 3 && std::wstring(argv[2]) == L"--child") return RunChild(factory);

    TestForwarding(factory);
    TestTerminate(factory);
    TestProcessTreeKill(factory);
    TestJobKill(factory);
    TestMissingInner();

    if (failures) { std::cerr << failures << " check(s) failed" << std::endl; return 1; }
    std::cout << "bon-driver-host-test OK" << std::endl;
    return 0;
}
