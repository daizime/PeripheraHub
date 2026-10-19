#pragma once
#include "WindowsSupport.h"
#include <span>
#include <cstdint>
#include <stdexcept>

namespace peripheral::platform::detail {
// Bounded asynchronous transfer shared by device adapters. Cancellation must
// complete before stack buffers/OVERLAPPED are destroyed, including on timeout.
inline DWORD transferHidReport(HANDLE device, bool writing, std::span<std::uint8_t> buffer,
                               DWORD timeoutMilliseconds) {
    const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event)
        throw std::runtime_error(systemError("Create HID event"));
    struct EventScope { HANDLE value; ~EventScope() { CloseHandle(value); } } scope{event};
    OVERLAPPED io{};
    io.hEvent = event;
    DWORD bytes{};
    const BOOL ok = writing ? WriteFile(device, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, &io)
                            : ReadFile(device, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, &io);
    if (ok)
        return bytes;
    if (GetLastError() != ERROR_IO_PENDING)
        throw std::runtime_error(systemError(writing ? "Send HID query" : "Read HID reply"));
    const auto wait = WaitForSingleObject(event, timeoutMilliseconds);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(device, &io);
        GetOverlappedResult(device, &io, &bytes, TRUE);
        throw std::runtime_error(wait == WAIT_TIMEOUT ? "HID query timed out; state unconfirmed"
                                                     : systemError("Wait for HID reply"));
    }
    if (!GetOverlappedResult(device, &io, &bytes, FALSE))
        throw std::runtime_error(systemError("Complete HID transfer"));
    return bytes;
}
} // namespace peripheral::platform::detail
