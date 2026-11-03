// The BonDriver an application loads. It forwards every call to BonDriverHost.exe,
// which runs the real BonDriver of the same name in the BonDriverHost subfolder.
// See doc/bondriver-host.md.

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "IBonDriver2.h"
#include "HostProtocol.h"

using namespace hostproto;

namespace {

constexpr DWORD HOST_START_TIMEOUT_MS = 30000;
constexpr DWORD ORPHAN_WAIT_MS = 10000;
// Backpressure beyond this reaches the driver's own buffer and its overflow policy
constexpr size_t MAX_QUEUED_FRAMES = 64;

HMODULE g_module = NULL;

std::wstring NewPipeName(const wchar_t * kind)
{
	GUID guid = {};
	::CoCreateGuid(&guid);
	wchar_t text[64] = L"";
	::StringFromGUID2(guid, text, ARRAYSIZE(text));
	return std::wstring(L"\\\\.\\pipe\\BonDriverHost-") + kind + L"-" + text;
}

// The application owns the server end, so its death is what breaks the pipes.
// The default DACL limits the pipe to this user, and the name is not guessable.
HANDLE CreatePipeServer(const std::wstring & name, DWORD bufferSize)
{
	return ::CreateNamedPipeW(name.c_str(),
		PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		1, bufferSize, bufferSize, 0, NULL);
}

bool WaitForPipeClient(HANDLE pipe, DWORD timeout)
{
	OVERLAPPED overlapped = {};
	overlapped.hEvent = ::CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!overlapped.hEvent)
		return false;
	bool connected = false;
	if (::ConnectNamedPipe(pipe, &overlapped)) {
		connected = true;
	}
	else {
		const DWORD error = ::GetLastError();
		if (error == ERROR_PIPE_CONNECTED) {
			connected = true;
		}
		else if (error == ERROR_IO_PENDING) {
			DWORD ignored = 0;
			if (::WaitForSingleObject(overlapped.hEvent, timeout) == WAIT_OBJECT_0) {
				connected = ::GetOverlappedResult(pipe, &overlapped, &ignored, FALSE) != FALSE;
			}
			else {
				::CancelIoEx(pipe, &overlapped);
				::GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
			}
		}
	}
	::CloseHandle(overlapped.hEvent);
	return connected;
}

// A host whose application was terminated may still hold its tuner for a moment
void WaitForOrphanHosts(void)
{
	const HANDLE orphans = ::OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, ORPHAN_SEMAPHORE);
	if (!orphans)
		return;
	const ULONGLONG deadline = ::GetTickCount64() + ORPHAN_WAIT_MS;
	while (::GetTickCount64() < deadline && ::WaitForSingleObject(orphans, 0) == WAIT_OBJECT_0) {
		::ReleaseSemaphore(orphans, 1, NULL);
		::Sleep(100);
	}
	::CloseHandle(orphans);
}

class ProxyBonDriver : public IBonDriver2 {
public:
	~ProxyBonDriver()
	{
		if (m_control != INVALID_HANDLE_VALUE) {
			// Returns once the host has closed the tuner, so it can be reopened at once
			Response response = {};
			Call(Command::Shutdown, 0, 0, &response, nullptr);
		}
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_stopping = true;
		}
		m_changed.notify_all();
		if (m_stopReader)
			::SetEvent(m_stopReader);
		if (m_reader) {
			::WaitForSingleObject(m_reader, INFINITE);
			::CloseHandle(m_reader);
		}
		if (m_stopReader)
			::CloseHandle(m_stopReader);
		if (m_control != INVALID_HANDLE_VALUE)
			::CloseHandle(m_control);
		if (m_data != INVALID_HANDLE_VALUE)
			::CloseHandle(m_data);
	}

	bool Start(void)
	{
		wchar_t modulePath[MAX_PATH * 4] = L"";
		::GetModuleFileNameW(g_module, modulePath, ARRAYSIZE(modulePath));
		const std::wstring path(modulePath);
		const size_t separator = path.find_last_of(L'\\');
		const std::wstring hostDirectory = path.substr(0, separator) + L"\\BonDriverHost";
		const std::wstring innerPath = hostDirectory + path.substr(separator);
		const std::wstring executable = hostDirectory + L"\\BonDriverHost.exe";

		const std::wstring controlName = NewPipeName(L"control");
		const std::wstring dataName = NewPipeName(L"data");
		m_control = CreatePipeServer(controlName, CONTROL_PIPE_BUFFER);
		m_data = CreatePipeServer(dataName, DATA_PIPE_BUFFER);
		if (m_control == INVALID_HANDLE_VALUE || m_data == INVALID_HANDLE_VALUE)
			return false;

		std::wstring commandLine = L"\"" + executable + L"\" --detach \"" + innerPath + L"\" \"" + controlName + L"\" \"" + dataName + L"\"";
		STARTUPINFOW startup = {};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process = {};
		if (!::CreateProcessW(executable.c_str(), &commandLine[0], NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB,
				NULL, hostDirectory.c_str(), &startup, &process)
			&& !::CreateProcessW(executable.c_str(), &commandLine[0], NULL, NULL, FALSE, 0,
				NULL, hostDirectory.c_str(), &startup, &process)) {
			OutputDebugStringW(L"BonDriverProxy: cannot start BonDriverHost.exe\n");
			return false;
		}
		::CloseHandle(process.hThread);
		::CloseHandle(process.hProcess);

		if (!WaitForPipeClient(m_control, HOST_START_TIMEOUT_MS) || !WaitForPipeClient(m_data, HOST_START_TIMEOUT_MS))
			return false;
		Hello hello = {};
		if (!PipeRead(m_control, &hello, sizeof(hello), NULL, HOST_START_TIMEOUT_MS) || !hello.ok) {
			OutputDebugStringW(L"BonDriverProxy: the host could not load the BonDriver\n");
			return false;
		}

		m_stopReader = ::CreateEventW(NULL, TRUE, FALSE, NULL);
		// A reference of its own keeps this code loaded for as long as the thread runs
		HMODULE self = NULL;
		::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ReaderThread), &self);
		m_reader = ::CreateThread(NULL, 0, ReaderThread, this, 0, NULL);
		if (!m_reader) {
			::FreeLibrary(self);
			return false;
		}
		return true;
	}

	// IBonDriver
	const BOOL OpenTuner(void) override
	{
		WaitForOrphanHosts();
		Response response = {};
		if (!Call(Command::OpenTuner, 0, 0, &response, nullptr) || !response.value)
			return FALSE;
		m_opened = true;
		return TRUE;
	}

	void CloseTuner(void) override
	{
		// Frees the queue first: the host finishes its current frame before it stops
		DiscardQueuedFrames();
		Response response = {};
		Call(Command::CloseTuner, 0, 0, &response, nullptr);
		m_opened = false;
		DiscardQueuedFrames();
		m_changed.notify_all();
	}

	const BOOL SetChannel(const BYTE bCh) override
	{
		DiscardQueuedFrames();
		Response response = {};
		return Call(Command::SetChannelByte, bCh, 0, &response, nullptr) && response.value;
	}

	const float GetSignalLevel(void) override
	{
		Response response = {};
		return Call(Command::GetSignalLevel, 0, 0, &response, nullptr) ? response.level : 0.0F;
	}

	// As in CBonTuner, a timeout of 0 waits without limit
	const DWORD WaitTsStream(const DWORD dwTimeOut = 0) override
	{
		std::unique_lock<std::mutex> lock(m_lock);
		const auto ready = [this] { return !m_frames.empty() || !m_opened || m_hostGone || m_stopping; };
		if (dwTimeOut) {
			if (!m_changed.wait_for(lock, std::chrono::milliseconds(dwTimeOut), ready))
				return WAIT_TIMEOUT;
		}
		else {
			m_changed.wait(lock, ready);
		}
		return m_frames.empty() ? WAIT_ABANDONED : WAIT_OBJECT_0;
	}

	const DWORD GetReadyCount(void) override
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return static_cast<DWORD>(m_frames.size());
	}

	const BOOL GetTsStream(BYTE * pDst, DWORD * pdwSize, DWORD * pdwRemain) override
	{
		BYTE * source = nullptr;
		if (!GetTsStream(&source, pdwSize, pdwRemain))
			return FALSE;
		if (*pdwSize)
			::memcpy(pDst, source, *pdwSize);
		return TRUE;
	}

	const BOOL GetTsStream(BYTE ** ppDst, DWORD * pdwSize, DWORD * pdwRemain) override
	{
		std::unique_lock<std::mutex> lock(m_lock);
		if (m_frames.empty()) {
			*ppDst = nullptr;
			*pdwSize = 0;
			*pdwRemain = 0;
			return FALSE;
		}
		// The returned pointer stays valid until the next call
		m_lastFrame = std::move(m_frames.front());
		m_frames.pop_front();
		*ppDst = m_lastFrame.data();
		*pdwSize = static_cast<DWORD>(m_lastFrame.size());
		*pdwRemain = static_cast<DWORD>(m_frames.size());
		lock.unlock();
		m_changed.notify_all();
		return TRUE;
	}

	void PurgeTsStream(void) override
	{
		DiscardQueuedFrames();
		Response response = {};
		Call(Command::PurgeTsStream, 0, 0, &response, nullptr);
	}

	void Release(void) override
	{
		delete this;
	}

	// IBonDriver2
	LPCTSTR GetTunerName(void) override
	{
		std::lock_guard<std::mutex> lock(m_stringLock);
		if (!m_hasTunerName) {
			Response response = {};
			std::wstring text;
			if (!Call(Command::GetTunerName, 0, 0, &response, &text))
				return nullptr;
			m_tunerName = std::move(text);
			m_tunerNameIsNull = response.stringChars == NULL_STRING;
			m_hasTunerName = true;
		}
		return m_tunerNameIsNull ? nullptr : m_tunerName.c_str();
	}

	const BOOL IsTunerOpening(void) override
	{
		Response response = {};
		return Call(Command::IsTunerOpening, 0, 0, &response, nullptr) && response.value;
	}

	LPCTSTR EnumTuningSpace(const DWORD dwSpace) override
	{
		return CachedString(Command::EnumTuningSpace, dwSpace, 0);
	}

	LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) override
	{
		return CachedString(Command::EnumChannelName, dwSpace, dwChannel);
	}

	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) override
	{
		DiscardQueuedFrames();
		Response response = {};
		return Call(Command::SetChannel, dwSpace, dwChannel, &response, nullptr) && response.value;
	}

	const DWORD GetCurSpace(void) override
	{
		Response response = {};
		return Call(Command::GetCurSpace, 0, 0, &response, nullptr) ? response.value : 0xFFFFFFFFUL;
	}

	const DWORD GetCurChannel(void) override
	{
		Response response = {};
		return Call(Command::GetCurChannel, 0, 0, &response, nullptr) ? response.value : 0xFFFFFFFFUL;
	}

private:
	bool Call(Command command, uint32_t arg1, uint32_t arg2, Response * response, std::wstring * text)
	{
		std::lock_guard<std::mutex> lock(m_callLock);
		const Request request = { command, arg1, arg2, m_generation.load() };
		if (!PipeWrite(m_control, &request, sizeof(request), NULL)
			|| !PipeRead(m_control, response, sizeof(*response), NULL)) {
			HostGone();
			return false;
		}
		if (command != Command::Shutdown && response->stringChars != NULL_STRING && response->stringChars > 0) {
			if (response->stringChars > MAX_STRING_CHARS) {
				HostGone();
				return false;
			}
			std::wstring received(response->stringChars, L'\0');
			if (!PipeRead(m_control, &received[0], response->stringChars * sizeof(wchar_t), NULL)) {
				HostGone();
				return false;
			}
			if (text)
				*text = std::move(received);
		}
		return true;
	}

	// Strings must outlive the call, and std::map keeps its nodes in place
	LPCTSTR CachedString(Command command, uint32_t arg1, uint32_t arg2)
	{
		std::lock_guard<std::mutex> lock(m_stringLock);
		const auto key = std::make_pair(static_cast<uint32_t>(command), std::make_pair(arg1, arg2));
		const auto found = m_strings.find(key);
		if (found != m_strings.end())
			return found->second.c_str();
		Response response = {};
		std::wstring text;
		if (!Call(command, arg1, arg2, &response, &text) || response.stringChars == NULL_STRING)
			return nullptr;
		return m_strings.emplace(key, std::move(text)).first->second.c_str();
	}

	// Frames of older generations, including those still in the pipe, are dropped on arrival
	void DiscardQueuedFrames(void)
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			++m_generation;
			m_frames.clear();
		}
		m_changed.notify_all();
	}

	void HostGone(void)
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_hostGone = true;
		}
		m_changed.notify_all();
	}

	static DWORD WINAPI ReaderThread(LPVOID parameter)
	{
		static_cast<ProxyBonDriver *>(parameter)->ReadFrames();
		::FreeLibraryAndExitThread(g_module, 0);
	}

	void ReadFrames(void)
	{
		for (;;) {
			FrameHeader header = {};
			if (!PipeRead(m_data, &header, sizeof(header), m_stopReader) || header.size > MAX_FRAME_BYTES)
				break;
			std::vector<BYTE> frame(header.size);
			if (!PipeRead(m_data, frame.data(), header.size, m_stopReader))
				break;

			std::unique_lock<std::mutex> lock(m_lock);
			m_changed.wait(lock, [&] {
				return m_stopping || m_frames.size() < MAX_QUEUED_FRAMES || header.generation != m_generation.load();
			});
			if (m_stopping)
				return;
			if (header.generation == m_generation.load()) {
				m_frames.push_back(std::move(frame));
				lock.unlock();
				m_changed.notify_all();
			}
		}
		HostGone();
	}

	HANDLE m_control = INVALID_HANDLE_VALUE;
	HANDLE m_data = INVALID_HANDLE_VALUE;
	HANDLE m_reader = NULL;
	HANDLE m_stopReader = NULL;

	std::mutex m_callLock;
	std::mutex m_stringLock;
	std::map<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>, std::wstring> m_strings;
	std::wstring m_tunerName;
	bool m_tunerNameIsNull = false;
	bool m_hasTunerName = false;

	std::mutex m_lock;
	std::condition_variable m_changed;
	std::deque<std::vector<BYTE>> m_frames;
	std::vector<BYTE> m_lastFrame;
	std::atomic<uint32_t> m_generation{ 0 };
	std::atomic<bool> m_opened{ false };
	bool m_hostGone = false;
	bool m_stopping = false;
};

}

#pragma warning(disable : 4273)
extern "C" __declspec(dllexport) IBonDriver * CreateBonDriver()
{
	auto * driver = new ProxyBonDriver;
	if (!driver->Start()) {
		delete driver;
		return nullptr;
	}
	return driver;
}
#pragma warning(default : 4273)

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /* lpReserved */)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
		g_module = hModule;
		::DisableThreadLibraryCalls(hModule);
	}
	return TRUE;
}
