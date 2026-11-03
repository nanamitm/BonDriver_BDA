// Holds the BDA graph outside the application, so that the graph is still stopped
// in the correct order when the application is terminated. See doc/bondriver-host.md.
//
//   BonDriverHost.exe --detach <BonDriver> <control pipe> <data pipe>
//   BonDriverHost.exe --serve  <BonDriver> <control pipe> <data pipe>
//
// --detach only starts --serve and exits, which leaves the serving process without
// a living parent. Stopping the application's process tree then does not reach it.
//
// Never terminate this process from code, even when closing the tuner hangs:
// terminating the process that owns the graph is the failure this host exists to avoid.

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <string>

#include "IBonDriver2.h"
#include "HostProtocol.h"

using namespace hostproto;

namespace {

HANDLE OpenPipe(const wchar_t * name)
{
	return ::CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
}

class Host {
public:
	Host(HANDLE control, HANDLE data)
		: m_control(control), m_data(data),
		m_stopStream(::CreateEventW(NULL, TRUE, FALSE, NULL)),
		m_abortWrite(::CreateEventW(NULL, TRUE, FALSE, NULL)),
		m_orphans(::CreateSemaphoreW(NULL, 0, ORPHAN_SEMAPHORE_MAX, ORPHAN_SEMAPHORE))
	{
	}

	bool Load(const wchar_t * path)
	{
		m_module = ::LoadLibraryW(path);
		if (!m_module)
			return false;
		const auto factory = reinterpret_cast<IBonDriver * (*)()>(::GetProcAddress(m_module, "CreateBonDriver"));
		if (!factory)
			return false;
		m_driver = factory();
		m_driver2 = dynamic_cast<IBonDriver2 *>(m_driver);
		return m_driver2 != nullptr;
	}

	void Serve()
	{
		Request request;
		bool clientLost = true;
		while (PipeRead(m_control, &request, sizeof(request), NULL)) {
			if (request.command == Command::Shutdown) {
				clientLost = false;
				break;
			}
			if (!Handle(request))
				break;
		}

		// Announced before the tuner is closed, so a new open waits for the tuner
		if (clientLost && m_orphans)
			::ReleaseSemaphore(m_orphans, 1, NULL);

		// The pipe is not used again, and a full client queue must not hold up the close
		::SetEvent(m_abortWrite);
		StopStream();
		if (m_driver) {
			m_driver->CloseTuner();
			m_driver->Release();
			m_driver = nullptr;
			m_driver2 = nullptr;
		}
		if (m_module) {
			::FreeLibrary(m_module);
			m_module = NULL;
		}

		if (clientLost && m_orphans)
			::WaitForSingleObject(m_orphans, 1000);
		else if (!clientLost) {
			// The client waits for this reply, after which the tuner is free for it
			const Response response = {};
			PipeWrite(m_control, &response, sizeof(response), NULL);
		}
	}

private:
	bool Handle(const Request & request)
	{
		Response response = {};
		response.stringChars = NULL_STRING;
		const wchar_t * text = nullptr;

		m_generation = request.generation;

		switch (request.command) {
		case Command::OpenTuner:
			response.value = m_driver->OpenTuner();
			if (response.value)
				StartStream();
			break;
		case Command::CloseTuner:
			// CloseTuner frees the buffers the stream thread reads
			StopStream();
			m_driver->CloseTuner();
			break;
		case Command::SetChannelByte:
			response.value = m_driver->SetChannel(static_cast<BYTE>(request.arg1));
			break;
		case Command::SetChannel:
			response.value = m_driver2->SetChannel(request.arg1, request.arg2);
			break;
		case Command::GetSignalLevel:
			response.level = m_driver->GetSignalLevel();
			break;
		case Command::PurgeTsStream:
			m_driver->PurgeTsStream();
			break;
		case Command::GetTunerName:
			text = m_driver2->GetTunerName();
			break;
		case Command::IsTunerOpening:
			response.value = m_driver2->IsTunerOpening();
			break;
		case Command::EnumTuningSpace:
			text = m_driver2->EnumTuningSpace(request.arg1);
			break;
		case Command::EnumChannelName:
			text = m_driver2->EnumChannelName(request.arg1, request.arg2);
			break;
		case Command::GetCurSpace:
			response.value = m_driver2->GetCurSpace();
			break;
		case Command::GetCurChannel:
			response.value = m_driver2->GetCurChannel();
			break;
		default:
			return false;
		}

		if (text) {
			const size_t length = ::wcslen(text);
			response.stringChars = static_cast<uint32_t>(length < MAX_STRING_CHARS ? length : MAX_STRING_CHARS);
		}
		if (!PipeWrite(m_control, &response, sizeof(response), NULL))
			return false;
		if (text && response.stringChars > 0)
			return PipeWrite(m_control, text, response.stringChars * sizeof(wchar_t), NULL);
		return true;
	}

	void StartStream()
	{
		if (m_streamThread)
			return;
		::ResetEvent(m_stopStream);
		m_streamThread = ::CreateThread(NULL, 0, StreamThread, this, 0, NULL);
	}

	void StopStream()
	{
		if (!m_streamThread)
			return;
		::SetEvent(m_stopStream);
		::WaitForSingleObject(m_streamThread, INFINITE);
		::CloseHandle(m_streamThread);
		m_streamThread = NULL;
	}

	static DWORD WINAPI StreamThread(LPVOID parameter)
	{
		static_cast<Host *>(parameter)->Stream();
		return 0;
	}

	void Stream()
	{
		while (::WaitForSingleObject(m_stopStream, 0) == WAIT_TIMEOUT) {
			const DWORD waited = m_driver->WaitTsStream(100);
			if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT) {
				// The driver is reopening internally
				::WaitForSingleObject(m_stopStream, 50);
				continue;
			}
			DWORD remain = 0;
			do {
				// Read before the data: a chunk purged meanwhile is dropped by the client
				const uint32_t generation = m_generation;
				BYTE * chunk = nullptr;
				DWORD size = 0;
				if (!m_driver->GetTsStream(&chunk, &size, &remain) || !chunk || !size)
					break;
				// A frame is never cut short while the pipe stays in use: the client
				// discards stale frames without queueing them, so this write completes
				const FrameHeader header = { generation, size };
				if (!PipeWrite(m_data, &header, sizeof(header), m_abortWrite)
					|| !PipeWrite(m_data, chunk, size, m_abortWrite))
					return;
			} while (remain && ::WaitForSingleObject(m_stopStream, 0) == WAIT_TIMEOUT);
		}
	}

	HANDLE m_control;
	HANDLE m_data;
	HANDLE m_stopStream;
	HANDLE m_abortWrite;
	HANDLE m_orphans;
	HANDLE m_streamThread = NULL;
	HMODULE m_module = NULL;
	IBonDriver * m_driver = nullptr;
	IBonDriver2 * m_driver2 = nullptr;
	std::atomic<uint32_t> m_generation{ 0 };
};

HANDLE g_controlPipe = NULL;
HANDLE g_serveThread = NULL;

struct ServeContext {
	const wchar_t * path;
	HANDLE control;
	HANDLE data;
};

DWORD WINAPI ServeThread(LPVOID parameter)
{
	const auto * context = static_cast<const ServeContext *>(parameter);
	Host host(context->control, context->data);
	const Hello hello = { host.Load(context->path) ? 1u : 0u };
	if (PipeWrite(context->control, &hello, sizeof(hello), NULL) && hello.ok)
		host.Serve();
	return 0;
}

// Logoff and shutdown end this process once WM_ENDSESSION returns
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_QUERYENDSESSION:
		return TRUE;
	case WM_ENDSESSION:
		if (wParam) {
			::CancelIoEx(g_controlPipe, NULL);
			::WaitForSingleObject(g_serveThread, INFINITE);
		}
		return 0;
	case WM_DESTROY:
		::PostQuitMessage(0);
		return 0;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

int Serve(HINSTANCE instance, const wchar_t * path, const wchar_t * controlName, const wchar_t * dataName)
{
	const HANDLE control = OpenPipe(controlName);
	if (control == INVALID_HANDLE_VALUE)
		return 3;
	const HANDLE data = OpenPipe(dataName);
	if (data == INVALID_HANDLE_VALUE)
		return 3;

	ServeContext context = { path, control, data };
	g_controlPipe = control;
	g_serveThread = ::CreateThread(NULL, 0, ServeThread, &context, CREATE_SUSPENDED, NULL);
	if (!g_serveThread)
		return 4;

	// A hidden top-level window: message-only windows do not get WM_ENDSESSION
	WNDCLASSW windowClass = {};
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = instance;
	windowClass.lpszClassName = L"BonDriverHostWindow";
	::RegisterClassW(&windowClass);
	const HWND window = ::CreateWindowExW(0, windowClass.lpszClassName, L"BonDriverHost", WS_POPUP,
		0, 0, 0, 0, NULL, NULL, instance, NULL);
	::ResumeThread(g_serveThread);

	for (;;) {
		const DWORD waited = ::MsgWaitForMultipleObjects(1, &g_serveThread, FALSE, INFINITE, QS_ALLINPUT);
		if (waited == WAIT_OBJECT_0)
			break;
		MSG message;
		while (::PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&message);
			::DispatchMessageW(&message);
		}
	}
	if (window)
		::DestroyWindow(window);
	return 0;
}

// Falls back to staying in the caller's job when the job forbids breaking away
bool StartDetached(const std::wstring & executable, std::wstring commandLine, const wchar_t * directory, PROCESS_INFORMATION * process)
{
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	if (::CreateProcessW(executable.c_str(), &commandLine[0], NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB,
		NULL, directory, &startup, process))
		return true;
	return ::CreateProcessW(executable.c_str(), &commandLine[0], NULL, NULL, FALSE, 0,
		NULL, directory, &startup, process) != FALSE;
}

int Detach(const wchar_t * path, const wchar_t * controlName, const wchar_t * dataName)
{
	wchar_t executable[MAX_PATH * 4] = L"";
	::GetModuleFileNameW(NULL, executable, ARRAYSIZE(executable));
	std::wstring commandLine = std::wstring(L"\"") + executable + L"\" --serve \"" + path + L"\" \"" + controlName + L"\" \"" + dataName + L"\"";

	std::wstring directory(executable);
	directory.resize(directory.find_last_of(L'\\'));

	PROCESS_INFORMATION process = {};
	if (!StartDetached(executable, commandLine, directory.c_str(), &process))
		return 5;
	::CloseHandle(process.hThread);
	::CloseHandle(process.hProcess);
	return 0;
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	int count = 0;
	wchar_t ** arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
	if (!arguments || count != 5)
		return 2;
	const std::wstring mode(arguments[1]);
	if (mode == L"--detach")
		return Detach(arguments[2], arguments[3], arguments[4]);
	if (mode == L"--serve")
		return Serve(instance, arguments[2], arguments[3], arguments[4]);
	return 2;
}
