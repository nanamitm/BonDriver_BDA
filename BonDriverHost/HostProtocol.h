// Wire format and pipe helpers shared by BonDriverProxy.dll and BonDriverHost.exe
#pragma once

#include <windows.h>
#include <cstdint>

namespace hostproto {

// Counts the hosts that lost their client and are still closing a tuner,
// so that a new open does not collide with them.
constexpr const wchar_t * ORPHAN_SEMAPHORE = L"Local\\BonDriverHost.Orphans";
constexpr LONG ORPHAN_SEMAPHORE_MAX = 1024;

constexpr DWORD CONTROL_PIPE_BUFFER = 64 * 1024;
constexpr DWORD DATA_PIPE_BUFFER = 4 * 1024 * 1024;
constexpr uint32_t MAX_STRING_CHARS = 4096;
constexpr uint32_t MAX_FRAME_BYTES = 64 * 1024 * 1024;

enum class Command : uint32_t {
	OpenTuner,
	CloseTuner,
	SetChannelByte,
	SetChannel,
	GetSignalLevel,
	PurgeTsStream,
	GetTunerName,
	IsTunerOpening,
	EnumTuningSpace,
	EnumChannelName,
	GetCurSpace,
	GetCurChannel,
	Shutdown,
};

struct Request {
	Command command;
	uint32_t arg1;
	uint32_t arg2;
	// Stream generation that applies from this request on
	uint32_t generation;
};

struct Response {
	uint32_t value;
	float level;
	// NULL_STRING means a null string; otherwise this many WCHARs follow
	uint32_t stringChars;
};

constexpr uint32_t NULL_STRING = UINT32_MAX;

// Sent by the host once it has loaded the BonDriver or failed to
struct Hello {
	uint32_t ok;
};

// Precedes each TS chunk on the data pipe
struct FrameHeader {
	uint32_t generation;
	uint32_t size;
};

// Moves exactly `size` bytes on an overlapped pipe. stopEvent may be NULL.
// Returns false on a broken pipe, the stop event, or the timeout.
inline bool PipeTransfer(HANDLE pipe, bool write, void * buffer, DWORD size, HANDLE stopEvent, DWORD timeout = INFINITE)
{
	OVERLAPPED overlapped = {};
	overlapped.hEvent = ::CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!overlapped.hEvent)
		return false;

	bool result = true;
	BYTE * position = static_cast<BYTE *>(buffer);
	while (size > 0) {
		DWORD transferred = 0;
		::ResetEvent(overlapped.hEvent);
		const BOOL completed = write
			? ::WriteFile(pipe, position, size, NULL, &overlapped)
			: ::ReadFile(pipe, position, size, NULL, &overlapped);
		if (!completed && ::GetLastError() != ERROR_IO_PENDING) {
			result = false;
			break;
		}
		const HANDLE handles[2] = { overlapped.hEvent, stopEvent };
		const DWORD waited = ::WaitForMultipleObjects(stopEvent ? 2 : 1, handles, FALSE, timeout);
		if (waited != WAIT_OBJECT_0) {
			// The buffer must stay valid until the kernel has let go of the request
			::CancelIoEx(pipe, &overlapped);
			::GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
			result = false;
			break;
		}
		if (!::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
			result = false;
			break;
		}
		position += transferred;
		size -= transferred;
	}
	::CloseHandle(overlapped.hEvent);
	return result;
}

inline bool PipeRead(HANDLE pipe, void * buffer, DWORD size, HANDLE stopEvent, DWORD timeout = INFINITE)
{
	return PipeTransfer(pipe, false, buffer, size, stopEvent, timeout);
}

inline bool PipeWrite(HANDLE pipe, const void * buffer, DWORD size, HANDLE stopEvent)
{
	return PipeTransfer(pipe, true, const_cast<void *>(buffer), size, stopEvent);
}

}
