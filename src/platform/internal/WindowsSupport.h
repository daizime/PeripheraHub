#pragma once

// 只共享 Win32 错误信息与 GUID 格式转换；资源封装留在各自模块内部。
#ifndef _WIN32
#error WindowsBackend requires Windows.
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <sstream>
#include <string>

namespace peripheral::platform::detail {
inline std::string systemError(const char* operation, DWORD code = GetLastError()) {
    std::ostringstream stream;
    stream << operation << " failed (Windows error " << code << ")";
    return stream.str();
}

inline std::wstring guidString(const GUID& guid) {
    wchar_t buffer[40]{};
    return StringFromGUID2(guid, buffer, 40) > 0 ? buffer : L"";
}

} // namespace peripheral::platform::detail
