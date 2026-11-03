// A fake tuner for bon-driver-host-test: logs its calls and streams numbered chunks.
// Like a real tuner it can be opened once at a time, and closing it takes a while.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include "IBonDriver2.h"

namespace {

void Log(const char * line)
{
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    wchar_t path[MAX_PATH] = L"";
    if (!GetEnvironmentVariableW(L"BONDRIVER_MOCK_LOG", path, MAX_PATH))
        return;
    FILE * file = nullptr;
    if (_wfopen_s(&file, path, L"a") == 0 && file) {
        std::fprintf(file, "%s\n", line);
        std::fclose(file);
    }
}

constexpr DWORD CHUNK_BYTES = 188 * 1024;

class Mock : public IBonDriver2 {
public:
    Mock() : m_chunk(CHUNK_BYTES) {}

    const BOOL OpenTuner(void) override {
        m_busy = CreateMutexW(nullptr, TRUE, L"Local\\BonDriverHostMockTuner");
        if (m_busy && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(m_busy);
            m_busy = nullptr;
        }
        Log(m_busy ? "open" : "open_busy");
        m_opened = m_busy != nullptr;
        return m_opened ? TRUE : FALSE;
    }
    void CloseTuner(void) override {
        if (!m_opened) return;
        m_opened = false;
        Sleep(500);
        Log("close");
        ReleaseMutex(m_busy);
        CloseHandle(m_busy);
        m_busy = nullptr;
    }
    const BOOL SetChannel(const BYTE) override { return FALSE; }
    const float GetSignalLevel(void) override { return 12.5F; }
    const DWORD WaitTsStream(const DWORD timeout) override {
        if (!m_opened) return WAIT_ABANDONED;
        Sleep(timeout < 5 ? timeout : 5);
        return WAIT_OBJECT_0;
    }
    const DWORD GetReadyCount(void) override { return 1; }
    const BOOL GetTsStream(BYTE * destination, DWORD * size, DWORD * remain) override {
        BYTE * source = nullptr;
        if (!GetTsStream(&source, size, remain)) return FALSE;
        std::memcpy(destination, source, *size);
        return TRUE;
    }
    const BOOL GetTsStream(BYTE ** destination, DWORD * size, DWORD * remain) override {
        const DWORD header[2] = { m_sequence++, m_purges.load() };
        std::memcpy(m_chunk.data(), header, sizeof(header));
        *destination = m_chunk.data();
        *size = CHUNK_BYTES;
        *remain = 0;
        return TRUE;
    }
    void PurgeTsStream(void) override { ++m_purges; }
    void Release(void) override { CloseTuner(); Log("release"); delete this; }
    LPCTSTR GetTunerName(void) override { return L"Mock tuner"; }
    const BOOL IsTunerOpening(void) override { return m_opened ? TRUE : FALSE; }
    LPCTSTR EnumTuningSpace(const DWORD space) override { return space == 0 ? L"UHF" : nullptr; }
    LPCTSTR EnumChannelName(const DWORD space, const DWORD channel) override {
        return space == 0 && channel == 0 ? L"Channel0" : space == 0 && channel == 1 ? L"Channel1" : nullptr;
    }
    const BOOL SetChannel(const DWORD space, const DWORD channel) override {
        m_space = space; m_channel = channel;
        ++m_purges;
        return space == 0 && channel < 2 ? TRUE : FALSE;
    }
    const DWORD GetCurSpace(void) override { return m_space; }
    const DWORD GetCurChannel(void) override { return m_channel; }

private:
    HANDLE m_busy = nullptr;
    std::atomic<bool> m_opened{ false };
    std::vector<BYTE> m_chunk;
    DWORD m_sequence = 0;
    std::atomic<DWORD> m_purges{ 0 };
    DWORD m_space = 0xFFFFFFFF;
    DWORD m_channel = 0xFFFFFFFF;
};

}

#pragma warning(disable : 4273)
extern "C" __declspec(dllexport) IBonDriver * CreateBonDriver() { return new Mock; }
