#include "WindowsBackend.h"
#include "internal/HidReportIO.h"
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

namespace peripheral::platform {
namespace {
struct CloseHandleDeleter { void operator()(void* h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
struct HeadsetDisconnected : std::runtime_error { using std::runtime_error::runtime_error; };
bool matches(const HidDeviceInfo& device) {
    return device.vid == 0x373B && (device.pid == 0x12D6 || device.pid == 0x12D5) &&
           device.usagePage == 0xFF90 && device.usage == 1 &&
           device.inputReportBytes == 64 && device.outputReportBytes == 64 && device.n9ControlCandidate;
}
void validate(HANDLE handle, const HidDeviceInfo& device) {
    HIDD_ATTRIBUTES attrs{};
    attrs.Size = sizeof(attrs);
    if (!HidD_GetAttributes(handle, &attrs) || attrs.VendorID != device.vid || attrs.ProductID != device.pid)
        throw std::runtime_error("N9 identity changed; refresh devices");
    PHIDP_PREPARSED_DATA data{};
    if (!HidD_GetPreparsedData(handle, &data))
        throw std::runtime_error("N9 descriptor unavailable");
    struct Release { PHIDP_PREPARSED_DATA value; ~Release() { HidD_FreePreparsedData(value); } } release{data};
    HIDP_CAPS caps{};
    if (HidP_GetCaps(data, &caps) != HIDP_STATUS_SUCCESS || caps.UsagePage != 0xFF90 || caps.Usage != 1 ||
        caps.InputReportByteLength != 64 || caps.OutputReportByteLength != 64)
        throw std::runtime_error("N9 report descriptor no longer matches");
    std::vector<HIDP_VALUE_CAPS> values(caps.NumberInputValueCaps);
    auto count = caps.NumberInputValueCaps;
    if (values.empty() || HidP_GetValueCaps(HidP_Input, values.data(), &count, data) != HIDP_STATUS_SUCCESS ||
        std::none_of(values.begin(), values.begin() + count, [](const auto& v) { return v.ReportID == 0x55; }))
        throw std::runtime_error("N9 input report 55 is not declared");
    // This device's constant output report is absent from HidP value/button caps.
    // Successful native report-55 read transactions were captured on 2026-10-19.
}
n9::Report query(HANDLE handle, n9::Report request) {
    if (detail::transferHidReport(handle, true, request, 700) != request.size())
        throw std::runtime_error("Incomplete N9 query transfer");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (std::chrono::steady_clock::now() < deadline) {
        n9::Report reply{};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const auto length = detail::transferHidReport(handle, false, reply, static_cast<DWORD>(std::max<std::int64_t>(1, remaining)));
        if (length != reply.size() || reply[0] != n9::kReportId)
            continue;
        if (reply[1] == 0x0A && reply[2] == 1)
            throw HeadsetDisconnected("N9 headset disconnected during query");
        if (reply[1] == request[1])
            return reply;
    }
    throw std::runtime_error("No matching N9 reply");
}
} // namespace
N9ControlsInfo WindowsBackend::queryN9Controls(const HidDeviceInfo& device, bool includePeq) const {
    N9ControlsInfo result;
    result.supported = matches(device);
    if (!result.supported) {
        result.error = "Not a supported N9 V2 Ultra control collection";
        return result;
    }
    try {
        Handle handle(CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE)
            throw std::runtime_error(detail::systemError("Open N9 control interface"));
        validate(handle.get(), device);
        result.status = n9::parseStatus(query(handle.get(), n9::statusQuery()));
        if (!result.status)
            throw std::runtime_error("Invalid N9 status response");
        if (includePeq)
            for (int point = 0; point < 10; ++point) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                result.peq[point] = n9::parsePeq(query(handle.get(), *n9::peqQuery(point)));
                if (!result.peq[point])
                    throw std::runtime_error("Invalid N9 PEQ response");
            }
    } catch (const HeadsetDisconnected& error) {
        result.status.reset();
        result.peq = {};
        result.error = error.what();
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}
N9EqModeResult WindowsBackend::setN9EqMode(const HidDeviceInfo& device, int mode) const {
    N9EqModeResult result;
    result.requested = mode;
    result.supported = matches(device);
    if (!result.supported) {
        result.error = "Not a supported N9 V2 Ultra control collection";
        return result;
    }
    const auto frame = n9::eqModeWriteQuery(mode);
    if (!frame) {
        result.error = "EQ mode is outside the observed range";
        return result;
    }
    try {
        Handle handle(CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE)
            throw std::runtime_error(detail::systemError("Open N9 control interface"));
        validate(handle.get(), device);
        const auto readMode = [&]() -> std::optional<int> {
            const auto status = n9::parseStatus(query(handle.get(), n9::statusQuery()));
            if (!status)
                return std::nullopt;
            return status->eqMode;
        };
        const auto current = readMode();
        if (!current)
            throw std::runtime_error("Invalid N9 status response before mode change");
        result.before = *current;
        if (*current == mode) {
            result.verified = true;
            result.after = current;
            return result;
        }
        auto request = *frame;
        result.sent = true;
        if (detail::transferHidReport(handle.get(), true, request, 700) != request.size())
            throw std::runtime_error("Incomplete N9 EQ mode transfer");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        result.after = readMode();
        if (!result.after)
            throw std::runtime_error("Invalid N9 status response after mode change");
        result.verified = *result.after == mode;
        if (!result.verified)
            result.error = "Read-back mode differs from the requested mode";
    } catch (const HeadsetDisconnected& error) {
        result.error = error.what();
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}
N9PeqWriteResult WindowsBackend::writeN9PeqPoint(const HidDeviceInfo& device, int point,
                                                 const n9::PeqPoint& value) const {
    N9PeqWriteResult result;
    result.requested = value;
    result.supported = matches(device);
    if (!result.supported) {
        result.error = "Not a supported N9 V2 Ultra control collection";
        return result;
    }
    const auto frame = n9::peqWriteQuery(point, value);
    if (!frame) {
        result.error = "PEQ point or field is outside the writable range";
        return result;
    }
    // Read-back compares parsed fields, not raw bytes: the device may legally
    // re-encode a value, and that is not a failed write.
    const auto same = [](const n9::PeqPoint& left, const n9::PeqPoint& right) {
        return left.frequencyHz == right.frequencyHz && left.gainTenthsDb == right.gainTenthsDb &&
            left.qTenths == right.qTenths && left.filterType == right.filterType &&
            left.enabled == right.enabled;
    };
    try {
        Handle handle(CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE)
            throw std::runtime_error(detail::systemError("Open N9 control interface"));
        validate(handle.get(), device);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto current = n9::parsePeq(query(handle.get(), *n9::peqQuery(point)));
        if (!current)
            throw std::runtime_error("Invalid N9 PEQ response before write");
        result.before = current;
        if (same(*current, value)) {
            result.unchanged = result.verified = true;
            result.after = current;
            return result;
        }
        auto request = *frame;
        result.sent = true;
        if (detail::transferHidReport(handle.get(), true, request, 700) != request.size())
            throw std::runtime_error("Incomplete N9 PEQ write transfer");
        // The vendor sends this frame without waiting for a reply, so the only
        // evidence of acceptance is the following point read.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        result.after = n9::parsePeq(query(handle.get(), *n9::peqQuery(point)));
        if (!result.after)
            throw std::runtime_error("Invalid N9 PEQ response after write");
        result.verified = same(*result.after, value);
        if (!result.verified)
            result.error = "Read-back differs from the requested point; the headset may have rejected it";
    } catch (const HeadsetDisconnected& error) {
        result.error = error.what();
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}
} // namespace peripheral::platform
