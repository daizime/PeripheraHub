#include "hid_discovery.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>

namespace peripheral {
namespace {

// Windows 的 wstring 是 UTF-16；也接受其他平台的 UTF-32，便于无设备的核心测试。
std::string utf8(const std::wstring& value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto code = static_cast<std::uint32_t>(value[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (code >= 0xD800 && code <= 0xDBFF && i + 1 < value.size()) {
                const auto next = static_cast<std::uint32_t>(value[i + 1]);
                if (next >= 0xDC00 && next <= 0xDFFF) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (next - 0xDC00);
                    ++i;
                }
            }
        }
        if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            code = 0xFFFD;
        if (code < 0x80) {
            result.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return result;
}

std::string trimmed(std::string value) {
    const auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), whitespace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string folded(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool genericName(const std::string& value) {
    static const std::set<std::string> names{"",
                                             "mouse",
                                             "keyboard",
                                             "usb mouse",
                                             "usb keyboard",
                                             "gaming mouse",
                                             "gaming keyboard",
                                             "usb gaming mouse",
                                             "usb gaming keyboard",
                                             "hid-compliant mouse",
                                             "hid keyboard device",
                                             "hid-compliant keyboard",
                                             "usb input device",
                                             "hid device",
                                             "usb device",
                                             "usb receiver",
                                             "2.4g receiver",
                                             "2.4ghz receiver",
                                             "2.4g wireless receiver",
                                             "wireless receiver",
                                             "receiver",
                                             "鼠标",
                                             "键盘",
                                             "usb 鼠标",
                                             "usb 键盘",
                                             "hid 标准鼠标",
                                             "usb 输入设备",
                                             "usb 接收器",
                                             "无线接收器"};
    return names.contains(folded(value));
}

bool verifiedVgnInterface(const platform::HidDeviceInfo& device) {
    return device.vid == 0x3554 && (device.pid == 0xFB56 || device.pid == 0xFB57) &&
           device.usagePage == 0xFF02 && device.inputReportBytes == 17 && device.outputReportBytes == 17 &&
           device.vgnBatteryCandidate;
}

DeviceKind classify(const platform::HidDeviceInfo& device) {
    const bool vgnMouse = device.vid == 0x3554 && (device.pid == 0xFB56 || device.pid == 0xFB57);
    if (device.usagePage == 0x01 && device.usage == 0x02)
        return DeviceKind::Mouse;
    // 已知鼠标的宏键集合不是另一台键盘；未知复合设备仍按其声明用途分类。
    if (!vgnMouse && device.usagePage == 0x01 && device.usage == 0x06)
        return DeviceKind::Keyboard;
    return verifiedVgnInterface(device) ? DeviceKind::Mouse : DeviceKind::Unknown;
}

std::string groupKey(const std::string& containerId, DeviceKind kind) {
    if (containerId.empty())
        return {};
    auto normalized = folded(containerId);
    normalized.erase(
        std::remove_if(normalized.begin(), normalized.end(), [](char c) { return c == '{' || c == '}'; }),
        normalized.end());
    return "hid:" + normalized + ":" + std::to_string(static_cast<int>(kind));
}

struct Name {
    std::string text;
    int rank = 0;
};

Name displayName(const platform::HidDeviceInfo& device, const Endpoint& endpoint) {
    if (endpoint.kind == DeviceKind::Mouse && isVgnF2MasterPlus(endpoint))
        return {"VGN Dragonfly F2 Master+", 2};
    const auto product = trimmed(utf8(device.product));
    if (genericName(product))
        return {endpoint.kind == DeviceKind::Mouse ? "USB 鼠标（型号未提供）" : "USB 键盘（型号未提供）", 0};
    const auto manufacturer = trimmed(utf8(device.manufacturer));
    if (!manufacturer.empty() && folded(product).find(folded(manufacturer)) == std::string::npos)
        return {manufacturer + " " + product, 1};
    return {product, 1};
}

struct GroupName {
    Name best;
    std::string serial;
    bool conflictingSerials = false;
};

} // namespace

std::vector<Endpoint> discoverHidEndpoints(const std::vector<platform::HidDeviceInfo>& devices) {
    std::vector<Endpoint> result;
    std::set<std::string> seenPaths;
    std::map<std::string, GroupName> names;
    for (const auto& device : devices) {
        const auto kind = classify(device);
        if (device.path.empty() || kind == DeviceKind::Unknown)
            continue;
        Endpoint endpoint;
        endpoint.endpointId = utf8(device.path);
        if (!seenPaths.insert(endpoint.endpointId).second)
            continue;
        endpoint.containerId = utf8(device.containerId);
        endpoint.groupKey = groupKey(endpoint.containerId, kind);
        endpoint.serial = utf8(device.serial);
        endpoint.vid = device.vid;
        endpoint.pid = device.pid;
        endpoint.kind = kind;
        endpoint.interfaceNumber = device.interfaceNumber;
        endpoint.usagePage = device.usagePage;
        endpoint.usage = device.usage;
        if (kind == DeviceKind::Mouse && isVgnF2MasterPlus(endpoint)) {
            endpoint.transport =
                endpoint.pid == 0xFB56 ? TransportKind::Receiver24G : TransportKind::WiredUsb;
        }
        // 通用 USB HID 不能区分“有线外设”和“无线接收器”，保留 Unknown。
        const auto name = displayName(device, endpoint);
        endpoint.displayName = name.text;
        if (!endpoint.groupKey.empty()) {
            auto& group = names[endpoint.groupKey];
            if (group.best.text.empty() || name.rank > group.best.rank)
                group.best = name;
            if (!endpoint.serial.empty()) {
                group.conflictingSerials |= !group.serial.empty() && group.serial != endpoint.serial;
                group.serial = endpoint.serial;
            }
        }
        result.push_back(std::move(endpoint));
    }
    for (auto& endpoint : result) {
        if (endpoint.groupKey.empty())
            continue;
        const auto& group = names.at(endpoint.groupKey);
        // 同一容器的普通集合不应覆盖专用集合已提供的型号；序列号冲突时不传播名称。
        if (!group.conflictingSerials)
            endpoint.displayName = group.best.text;
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.endpointId < b.endpointId; });
    return result;
}

} // namespace peripheral
