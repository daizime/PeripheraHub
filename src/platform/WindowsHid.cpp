#include "WindowsBackend.h"
#include "internal/WindowsSupport.h"
#include "internal/HidReportIO.h"
#include "core/vgn_protocol.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <thread>
#include <utility>

namespace peripheral::platform {
namespace {
using detail::guidString;
using detail::systemError;

// HID 枚举和 F2 Master+ 电量读取。每次调用独立管理句柄，切换连接后重新打开。
struct HandleCloser {
    void operator()(void* handle) const {
        if (handle && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;
struct DeviceSetCloser {
    void operator()(void* set) const {
        if (set && set != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(set);
    }
};
using DeviceSet = std::unique_ptr<void, DeviceSetCloser>;
struct HidPreparsedCloser {
    void operator()(PHIDP_PREPARSED_DATA data) const {
        if (data)
            HidD_FreePreparsedData(data);
    }
};
using HidPreparsed = std::unique_ptr<std::remove_pointer_t<PHIDP_PREPARSED_DATA>, HidPreparsedCloser>;

std::wstring containerIdentity(HDEVINFO set, SP_DEVINFO_DATA& info) {
    GUID value{};
    DEVPROPTYPE type{};
    if (SetupDiGetDevicePropertyW(set, &info, &DEVPKEY_Device_ContainerId, &type,
                                  reinterpret_cast<PBYTE>(&value), sizeof(value), nullptr, 0) &&
        type == DEVPROP_TYPE_GUID)
        return guidString(value);
    return {};
}
std::wstring instanceIdentity(HDEVINFO set, SP_DEVINFO_DATA& info) {
    DWORD required = 0;
    SetupDiGetDeviceInstanceIdW(set, &info, nullptr, 0, &required);
    if (required == 0)
        return {};
    std::vector<wchar_t> buffer(required);
    if (SetupDiGetDeviceInstanceIdW(set, &info, buffer.data(), required, nullptr))
        return buffer.data();
    return {};
}
std::wstring propertyString(HDEVINFO set, SP_DEVINFO_DATA& info, DWORD property) {
    DWORD type{}, required{};
    SetupDiGetDeviceRegistryPropertyW(set, &info, property, &type, nullptr, 0, &required);
    if (!required)
        return {};
    std::vector<BYTE> buffer(required + sizeof(wchar_t), 0);
    if (SetupDiGetDeviceRegistryPropertyW(set, &info, property, &type, buffer.data(),
                                          static_cast<DWORD>(buffer.size()), nullptr) &&
        (type == REG_SZ || type == REG_MULTI_SZ))
        return reinterpret_cast<const wchar_t*>(buffer.data());
    return {};
}
int interfaceNumber(const std::wstring& path) {
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    const auto position = lower.find(L"&mi_");
    if (position == std::wstring::npos || position + 6 > lower.size())
        return -1;
    const auto digit = [](wchar_t character) -> int {
        if (character >= L'0' && character <= L'9')
            return character - L'0';
        if (character >= L'a' && character <= L'f')
            return character - L'a' + 10;
        return -1;
    };
    const int upper = digit(lower[position + 4]);
    const int lowerDigit = digit(lower[position + 5]);
    return upper < 0 || lowerDigit < 0 ? -1 : upper * 16 + lowerDigit;
}
bool reportIdPresent(PHIDP_PREPARSED_DATA data, HIDP_REPORT_TYPE type, USHORT valueCount, USHORT buttonCount,
                     UCHAR id) {
    if (valueCount) {
        std::vector<HIDP_VALUE_CAPS> values(valueCount);
        auto count = valueCount;
        if (HidP_GetValueCaps(type, values.data(), &count, data) == HIDP_STATUS_SUCCESS) {
            for (USHORT i = 0; i < count; ++i)
                if (values[i].ReportID == id)
                    return true;
        }
    }
    if (buttonCount) {
        std::vector<HIDP_BUTTON_CAPS> buttons(buttonCount);
        auto count = buttonCount;
        if (HidP_GetButtonCaps(type, buttons.data(), &count, data) == HIDP_STATUS_SUCCESS) {
            for (USHORT i = 0; i < count; ++i)
                if (buttons[i].ReportID == id)
                    return true;
        }
    }
    return false;
}
bool exactVgnInterface(const HidDeviceInfo& device) {
    return device.vid == 0x3554 && (device.pid == 0xFB56 || device.pid == 0xFB57) &&
           device.usagePage == 0xFF02 && device.usage == 2 && device.inputReportBytes == 17 && device.outputReportBytes == 17 &&
           device.vgnBatteryCandidate;
}
bool liveVgnInterface(HANDLE device) {
    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);
    if (!HidD_GetAttributes(device, &attributes) || attributes.VendorID != 0x3554 ||
        (attributes.ProductID != 0xFB56 && attributes.ProductID != 0xFB57))
        return false;
    PHIDP_PREPARSED_DATA raw{};
    if (!HidD_GetPreparsedData(device, &raw))
        return false;
    HidPreparsed data(raw);
    HIDP_CAPS caps{};
    if (HidP_GetCaps(data.get(), &caps) != HIDP_STATUS_SUCCESS || caps.UsagePage != 0xFF02 || caps.Usage != 2 ||
        caps.InputReportByteLength != 17 || caps.OutputReportByteLength != 17)
        return false;
    return reportIdPresent(data.get(), HidP_Input, caps.NumberInputValueCaps, caps.NumberInputButtonCaps,
                           8) &&
           reportIdPresent(data.get(), HidP_Output, caps.NumberOutputValueCaps, caps.NumberOutputButtonCaps,
                           8);
}

// 超时后先取消并等待 I/O 结束，再释放 OVERLAPPED 和缓冲区，避免驱动继续写入
// 已销毁的内存。异常内核驱动仍可能延迟取消，不能安全地强行丢弃其缓冲区。
DWORD overlappedTransfer(HANDLE device, bool writing, vgn::Report& buffer, DWORD timeoutMilliseconds) {
    return detail::transferHidReport(device, writing, buffer, timeoutMilliseconds);
}
vgn::Response vgnReadQuery(HANDLE device, vgn::Command command) {
    // 只读协议参考 BatteryMonitor（MIT）的 VgnHidProvider.cpp::queryMouseEnc；
    // 此函数只有在线和电量查询，不含配置或固件写入命令。
    auto request = vgn::makeQuery(command);
    if (overlappedTransfer(device, true, request, 700) != request.size())
        throw std::runtime_error("Incomplete HID query write");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (std::chrono::steady_clock::now() < deadline) {
        vgn::Report reply{};
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                .count();
        const auto length = overlappedTransfer(device, false, reply,
                                               static_cast<DWORD>(std::max<std::int64_t>(1, remaining)));
        const auto parsed =
            vgn::parseResponse(std::vector<std::uint8_t>(reply.begin(), reply.begin() + length), command);
        if (!parsed) {
            // 等待期间可能收到其他报文；本次查询的坏报文不能更新在线状态或电量。
            if (parsed.error == vgn::ParseError::ReportId || parsed.error == vgn::ParseError::Command)
                continue;
            throw std::runtime_error("VGN response validation failed (protocol error " +
                                     std::to_string(static_cast<int>(parsed.error)) + ")");
        }
        return *parsed.value;
    }
    throw std::runtime_error("No matching VGN response before timeout");
}

// A bounded exchange on the same validated handle, used only for identity and
// whitelisted profile reads. Other report notifications cannot satisfy it.
template<class Match>
vgn::Report vgnConfigQuery(HANDLE device, vgn::Report request, Match matches) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (overlappedTransfer(device, true, request, 700) != request.size())
        throw std::runtime_error("Incomplete VGN configuration query");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (std::chrono::steady_clock::now() < deadline) {
        vgn::Report reply{};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        const auto count = overlappedTransfer(device, false, reply,
            static_cast<DWORD>(std::max<std::int64_t>(1, remaining)));
        if (matches(std::span(reply).first(count)))
            return reply;
    }
    throw std::runtime_error("No validated VGN configuration response before timeout");
}
vgn::Profile readVgnProfile(HANDLE device) {
    vgnConfigQuery(device, vgn::identityQuery(), vgn::matchesF2Identity);
    vgn::ProfileCapture capture;
    vgn::Report first{};
    for (const auto block : vgn::kProfileReads) {
        const auto reply = vgnConfigQuery(device, vgn::profileQuery(block).value(),
            [block](auto frame) { return vgn::matchesProfileReply(frame, block); });
        if (!capture.accept(block, reply))
            throw std::runtime_error("Rejected VGN profile fragment");
        if (block.address == 0)
            first = reply;
    }
    // The physical DPI button may change the selected stage during this scan.
    // Discard that mixed sample instead of presenting the wrong active DPI.
    const auto header = vgn::kProfileReads.front();
    const auto last = vgnConfigQuery(device, vgn::profileQuery(header).value(),
        [header](auto frame) { return vgn::matchesProfileReply(frame, header); });
    if (!std::equal(first.begin() + 6, first.begin() + 16, last.begin() + 6))
        throw std::runtime_error("VGN configuration changed during the read; refresh again");
    const auto result = capture.decode();
    if (!result)
        throw std::runtime_error("VGN profile contains incomplete or invalid fields");
    return *result;
}

// The block that fully contains one 4-byte record, taken from the existing read
// whitelist: this path cannot reach an address the read path may not reach.
std::array<std::uint8_t, 4> readVgnRecord(HANDLE device, std::uint16_t address) {
    for (const auto block : vgn::kProfileReads) {
        if (address < block.address || address + 4 > block.address + block.length)
            continue;
        const auto reply = vgnConfigQuery(device, vgn::profileQuery(block).value(),
            [block](auto frame) { return vgn::matchesProfileReply(frame, block); });
        std::array<std::uint8_t, 4> record{};
        std::copy_n(reply.begin() + 6 + (address - block.address), 4, record.begin());
        return record;
    }
    throw std::runtime_error("Record is outside the whitelisted read blocks");
}
// A scalar record is the value plus its complement; both bytes must agree
// before the reading counts as a confirmed device value.
std::optional<int> readVgnScalar(HANDLE device, std::uint16_t address) {
    const auto record = readVgnRecord(device, address);
    if (static_cast<std::uint8_t>(0x55u - record[0]) != record[1])
        return std::nullopt;
    return record[0];
}
void sendVgnFlashWrite(HANDLE device, vgn::Report request) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (overlappedTransfer(device, true, request, 700) != request.size())
        throw std::runtime_error("Incomplete VGN flash write");
    // The device answers a write. Drain at most one reply so it cannot be taken
    // for the read-back; a missing acknowledgement is reported by the read-back.
    try {
        vgn::Report reply{};
        overlappedTransfer(device, false, reply, 300);
    } catch (const std::exception&) {
    }
}
VgnScalarWriteResult writeVgnScalar(const HidDeviceInfo& device, std::uint16_t address, int value,
                                    bool stageIndex) {
    VgnScalarWriteResult result;
    result.requested = value;
    if (value < 0 || value > 255) {
        result.error = "Scalar value is outside the record format";
        return result;
    }
    if (!exactVgnInterface(device)) {
        result.error = "Not an exact, report-validated VGN F2 Master+ control interface";
        return result;
    }
    UniqueHandle handle(CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        result.error = systemError("Open VGN write interface");
        return result;
    }
    try {
        if (!liveVgnInterface(handle.get()))
            throw std::runtime_error("Opened HID interface no longer matches the VGN whitelist");
        const auto profile = readVgnProfile(handle.get());
        if (stageIndex && value >= static_cast<int>(profile.stages.size()))
            throw std::runtime_error("Stage is not enabled on this device");
        const auto before = readVgnScalar(handle.get(), address);
        if (!before)
            throw std::runtime_error("Invalid scalar record before write");
        result.before = *before;
        if (*before == value) {
            result.unchanged = result.verified = true;
            result.after = before;
            return result;
        }
        const auto record = vgn::encodeScalarRecord(value);
        const auto frame = vgn::flashWriteQuery(address, record);
        if (!frame)
            throw std::runtime_error("Scalar address is not whitelisted for writing");
        result.sent = true;
        sendVgnFlashWrite(handle.get(), *frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        result.after = readVgnScalar(handle.get(), address);
        if (!result.after)
            throw std::runtime_error("Invalid scalar record after write");
        result.verified = *result.after == value;
        if (!result.verified)
            result.error = "Read-back differs from the requested value; device state is unknown";
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}

} // namespace

VgnFlashWriteResult WindowsBackend::writeVgnDpiStage(const HidDeviceInfo& device, int stage, int x, int y) const {
    VgnFlashWriteResult result;
    const auto record = vgn::encodeDpiRecord(x, y);
    if (!record) {
        result.error = "DPI has no exact on-device representation";
        return result;
    }
    result.requested = *record;
    if (!exactVgnInterface(device)) {
        result.error = "Not an exact, report-validated VGN F2 Master+ control interface";
        return result;
    }
    if (stage < 0 || stage >= static_cast<int>(vgn::kFlashDpiStages)) {
        result.error = "Stage is outside the range this build may write";
        return result;
    }
    UniqueHandle handle(CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        result.error = systemError("Open VGN write interface");
        return result;
    }
    try {
        if (!liveVgnInterface(handle.get()))
            throw std::runtime_error("Opened HID interface no longer matches the VGN whitelist");
        // Identity and a complete profile read both have to succeed first: this
        // also proves the mouse answers while it is awake.
        const auto profile = readVgnProfile(handle.get());
        if (stage >= static_cast<int>(profile.stages.size()))
            throw std::runtime_error("Stage is not enabled on this device");
        const auto address = static_cast<std::uint16_t>(vgn::kFlashDpiBase + stage * 4);
        result.before = readVgnRecord(handle.get(), address);
        // Same decoded value means nothing to write, even when the stored bytes
        // use another legal encoding (a stage may store 42000 with or without a
        // multiplier bit). Avoids rewriting flash just to change representation.
        const auto existing = vgn::decodeDpi(result.before);
        if (existing && existing->x == x && existing->y == y) {
            result.unchanged = result.verified = true;
            result.after = result.before;
            return result;
        }
        const auto frame = vgn::flashWriteQuery(address, *record);
        if (!frame)
            throw std::runtime_error("Record address is not whitelisted for writing");
        result.sent = true;
        sendVgnFlashWrite(handle.get(), *frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        result.after = readVgnRecord(handle.get(), address);
        result.verified = result.after == *record;
        if (!result.verified)
            result.error = "Read-back differs from the requested record; device state is unknown";
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}

VgnScalarWriteResult WindowsBackend::writeVgnPollingRate(const HidDeviceInfo& device, int hertz) const {
    VgnScalarWriteResult result;
    result.requested = hertz;
    const auto code = vgn::encodePollingRate(hertz);
    if (!code) {
        result.error = "Polling rate is not one of the observed values";
        return result;
    }
    auto written = writeVgnScalar(device, vgn::kFlashReportRate, *code, false);
    const auto toHertz = [](int raw) {
        return raw < 0 ? -1 : vgn::decodePollingRate(raw).value_or(-1);
    };
    written.requested = hertz;
    written.before = toHertz(written.before);
    if (written.after)
        written.after = toHertz(*written.after);
    return written;
}

VgnScalarWriteResult WindowsBackend::writeVgnActiveStage(const HidDeviceInfo& device, int stage) const {
    auto result = writeVgnScalar(device, vgn::kFlashActiveStage, stage, true);
    result.requested = stage;
    return result;
}

std::vector<HidDeviceInfo> WindowsBackend::enumerateHidDevices() const {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    DeviceSet set(SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (set.get() == INVALID_HANDLE_VALUE)
        throw std::runtime_error(systemError("Enumerate HID interfaces"));
    std::vector<HidDeviceInfo> result;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA deviceInterface{};
        deviceInterface.cbSize = sizeof(deviceInterface);
        if (!SetupDiEnumDeviceInterfaces(set.get(), nullptr, &hidGuid, index, &deviceInterface)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
                break;
            throw std::runtime_error(systemError("Read HID interface"));
        }
        DWORD required{};
        SetupDiGetDeviceInterfaceDetailW(set.get(), &deviceInterface, nullptr, 0, &required, nullptr);
        if (!required)
            continue;
        std::vector<BYTE> buffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        if (!SetupDiGetDeviceInterfaceDetailW(set.get(), &deviceInterface, detail, required, nullptr, &info))
            continue;
        HidDeviceInfo item;
        item.path = detail->DevicePath;
        item.instanceId = instanceIdentity(set.get(), info);
        item.containerId = containerIdentity(set.get(), info);
        item.product = propertyString(set.get(), info, SPDRP_FRIENDLYNAME);
        if (item.product.empty())
            item.product = propertyString(set.get(), info, SPDRP_DEVICEDESC);
        item.interfaceNumber = interfaceNumber(item.path);
        UniqueHandle device(CreateFileW(item.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, 0, nullptr));
        if (device.get() == INVALID_HANDLE_VALUE) {
            item.error = systemError("Open HID metadata");
            result.push_back(std::move(item));
            continue;
        }
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (HidD_GetAttributes(device.get(), &attributes)) {
            item.vid = attributes.VendorID;
            item.pid = attributes.ProductID;
        }
        wchar_t text[256]{};
        if (HidD_GetProductString(device.get(), text, sizeof(text)))
            item.product = text;
        std::fill(std::begin(text), std::end(text), L'\0');
        if (HidD_GetManufacturerString(device.get(), text, sizeof(text)))
            item.manufacturer = text;
        std::fill(std::begin(text), std::end(text), L'\0');
        if (HidD_GetSerialNumberString(device.get(), text, sizeof(text)))
            item.serial = text;
        PHIDP_PREPARSED_DATA raw{};
        if (HidD_GetPreparsedData(device.get(), &raw)) {
            HidPreparsed data(raw);
            HIDP_CAPS caps{};
            if (HidP_GetCaps(data.get(), &caps) == HIDP_STATUS_SUCCESS) {
                item.usagePage = caps.UsagePage;
                item.usage = caps.Usage;
                item.inputReportBytes = caps.InputReportByteLength;
                item.outputReportBytes = caps.OutputReportByteLength;
                item.featureReportBytes = caps.FeatureReportByteLength;
                if (item.vid == 0x373B && (item.pid == 0x12D6 || item.pid == 0x12D5) &&
                    item.usagePage == 0xFF90 && item.usage == 1 &&
                    item.inputReportBytes == 64 && item.outputReportBytes == 64)
                    item.n9ControlCandidate = reportIdPresent(data.get(), HidP_Input,
                        caps.NumberInputValueCaps, caps.NumberInputButtonCaps, 0x55);
                if (item.vid == 0x3554 && (item.pid == 0xFB56 || item.pid == 0xFB57) &&
                    item.usagePage == 0xFF02 && item.usage == 2 && item.inputReportBytes == 17 && item.outputReportBytes == 17) {
                    item.vgnBatteryCandidate =
                        reportIdPresent(data.get(), HidP_Input, caps.NumberInputValueCaps,
                                        caps.NumberInputButtonCaps, 8) &&
                        reportIdPresent(data.get(), HidP_Output, caps.NumberOutputValueCaps,
                                        caps.NumberOutputButtonCaps, 8);
                }
            } else
                item.error = "Cannot parse HID report capabilities";
        } else
            item.error = systemError("Read HID report capabilities");
        result.push_back(std::move(item));
    }
    return result;
}

VgnBatteryInfo WindowsBackend::queryVgnBattery(const HidDeviceInfo& device) const {
    VgnBatteryInfo result;
    result.supported = exactVgnInterface(device);
    if (!result.supported) {
        result.error = "Not an exact, report-validated VGN F2 Master+ control interface";
        return result;
    }
    UniqueHandle handle(CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        result.error = systemError("Open VGN battery interface");
        return result;
    }
    try {
        if (!liveVgnInterface(handle.get()))
            throw std::runtime_error("Opened HID interface no longer matches the VGN read-only whitelist");
        const auto online = vgnReadQuery(handle.get(), vgn::Command::Online);
        result.onlineKnown = online.online.has_value();
        result.deviceOnline = online.online.value_or(false);
        // A zero hint from this firmware is inconclusive. A validated battery
        // reply can independently confirm that the mouse responded.
        const auto battery = vgnReadQuery(handle.get(), vgn::Command::Battery);
        result.percentage = battery.reportedPercent.value();
        result.charging = battery.charging.value();
        result.voltageMillivolts = battery.rawVoltage.value();
        result.batteryValid = true;
        result.onlineKnown = result.deviceOnline = true;
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    if (result.batteryValid) {
        try {
            result.profile = readVgnProfile(handle.get());
        } catch (const std::exception& error) {
            result.profileError = error.what();
        }
    } else {
        result.profileError = "Mouse did not return a valid battery response; profile was not read";
    }
    return result;
}

} // namespace peripheral::platform
