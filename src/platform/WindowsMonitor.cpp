#include "WindowsBackend.h"
#include "core/ddc_capabilities.h"
#include "internal/WindowsSupport.h"

#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

namespace peripheral::platform {
namespace {
using detail::systemError;

// DDC/CI 亮度与显示器身份映射。物理句柄仅在一次枚举中有效，不跨热插拔保存。
class PhysicalMonitors {
  public:
    explicit PhysicalMonitors(HMONITOR monitor) {
        DWORD count{};
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) || !count)
            throw std::runtime_error(systemError("Enumerate physical monitors"));
        items_.resize(count);
        if (!GetPhysicalMonitorsFromHMONITOR(monitor, count, items_.data())) {
            items_.clear();
            throw std::runtime_error(systemError("Open physical monitors"));
        }
    }
    ~PhysicalMonitors() {
        if (!items_.empty())
            DestroyPhysicalMonitors(static_cast<DWORD>(items_.size()), items_.data());
    }
    PhysicalMonitors(const PhysicalMonitors&) = delete;
    PhysicalMonitors& operator=(const PhysicalMonitors&) = delete;
    const std::vector<PHYSICAL_MONITOR>& items() const { return items_; }

  private:
    std::vector<PHYSICAL_MONITOR> items_;
};
std::wstring monitorDevicePath(const std::wstring& displayName) {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (EnumDisplayDevicesW(displayName.c_str(), 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) &&
        device.DeviceID[0]) {
        const std::wstring path = device.DeviceID;
        // DISPLAYn 在拔插后可能分配给另一台屏幕，不能用作写入目标身份。
        if (path.rfind(L"\\\\?\\", 0) == 0)
            return path;
    }
    return {};
}
std::wstring monitorFriendlyName(const std::wstring& displayName) {
    // 物理描述常为“通用即插即用显示器”，改从 DisplayConfig 取 EDID 名称。
    // 通过源 GDI 名称匹配；同源克隆到多个目标时不猜测具体设备。
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount{}, modeCount{};
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return {};
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const auto status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                               modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (status != ERROR_SUCCESS)
            return {};
        std::wstring result;
        std::wstring matchedPath;
        for (UINT32 index = 0; index < pathCount; ++index) {
            const auto& path = paths[index];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                displayName != source.viewGdiDeviceName)
                continue;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
                continue;
            if (!matchedPath.empty() && matchedPath != target.monitorDevicePath)
                return {};
            matchedPath = target.monitorDevicePath;
            result = target.monitorFriendlyDeviceName;
        }
        return result;
    }
    return {};
}
MonitorInfo monitorBase(const MONITORINFOEXW& info) {
    MonitorInfo result;
    result.displayName = info.szDevice;
    result.devicePath = monitorDevicePath(result.displayName);
    result.id = result.devicePath.empty() ? L"unidentified:" + result.displayName
                                          : result.devicePath + L"#physical:0";
    result.name = monitorFriendlyName(result.displayName);
    if (result.name.empty())
        result.name = result.displayName;
    return result;
}
// 高级控制需要唯一的活动显示路径；克隆输出不能凭物理下标猜测目标。
bool hasUniqueActiveTarget(const MonitorInfo& monitor) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount{}, modeCount{};
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return false;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const auto status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                               modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (status != ERROR_SUCCESS)
            return false;
        unsigned matches = 0;
        for (UINT32 index = 0; index < pathCount; ++index) {
            const auto& path = paths[index];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
                return false;
            if (monitor.displayName != source.viewGdiDeviceName)
                continue;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS ||
                _wcsicmp(target.monitorDevicePath, monitor.devicePath.c_str()) != 0)
                return false;
            ++matches;
        }
        return matches == 1 && monitorDevicePath(monitor.displayName) == monitor.devicePath;
    }
    return false;
}

constexpr std::array<std::uint8_t, 4> advancedCodes{0x12, 0x14, 0x60, 0xDC};

void requireAdvancedCode(std::uint8_t code) {
    if (std::find(advancedCodes.begin(), advancedCodes.end(), code) == advancedCodes.end())
        throw std::invalid_argument("Only contrast, color preset, input source and display mode are allowed");
}

MonitorControlsInfo readMonitorControls(HANDLE handle, const std::wstring& id,
                                        std::optional<std::uint8_t> requested = std::nullopt,
                                        const std::string* declaredCapabilities = nullptr) {
    MonitorControlsInfo result;
    result.id = id;
    for (const auto code : advancedCodes) {
        if (requested && code != *requested)
            continue;
        MonitorFeatureInfo feature;
        feature.code = code;
        result.features.push_back(std::move(feature));
    }

    if (declaredCapabilities) {
        result.capabilities = *declaredCapabilities;
    } else {
        DWORD length{};
        if (!GetCapabilitiesStringLength(handle, &length)) {
            result.capabilitiesError = systemError("Read DDC/CI capabilities length");
        } else if (length == 0 || length > 64 * 1024) {
            result.capabilitiesError = "DDC/CI capabilities length must be between 1 and 65536 bytes";
        } else {
            std::vector<char> buffer(length, '\0');
            if (!CapabilitiesRequestAndCapabilitiesReply(handle, buffer.data(), length)) {
                result.capabilitiesError = systemError("Read DDC/CI capabilities");
            } else {
                const auto end = std::find(buffer.begin(), buffer.end(), '\0');
                if (end == buffer.end())
                    result.capabilitiesError = "DDC/CI capabilities string is not terminated";
                else
                    result.capabilities.assign(buffer.begin(), end);
            }
        }
    }
    const auto capabilities = ddc::parseCapabilities(result.capabilities);
    result.version = capabilities.version;
    if (result.capabilitiesError.empty() && !capabilities.valid)
        result.capabilitiesError = capabilities.error;

    for (auto& feature : result.features) {
        if (!capabilities.valid || !result.capabilitiesError.empty()) {
            feature.error = "A valid DDC/CI capabilities declaration is required";
            continue;
        }
        const auto declaration = capabilities.features.find(feature.code);
        feature.advertised = declaration != capabilities.features.end();
        if (!feature.advertised) {
            feature.error = "This feature is not advertised by the monitor";
            continue;
        }
        feature.allowedValues = declaration->second;
        // MCCS 3.x 的输入源是表控制，不能通过标量 Get/Set VCP 实现。
        if (feature.code == 0x60 && capabilities.version.rfind("3.", 0) == 0) {
            feature.error = "MCCS 3.x input-source table control is not implemented";
            continue;
        }
        MC_VCP_CODE_TYPE type{};
        DWORD current{}, maximum{};
        if (!GetVCPFeatureAndVCPFeatureReply(handle, feature.code, &type, &current, &maximum)) {
            feature.error = systemError("Read advertised DDC/CI feature");
            continue;
        }
        feature.readable = true;
        feature.current = current;
        feature.maximum = maximum;
        if (current > 0xFFFF) {
            feature.error = "Monitor returned a VCP current value outside the 16-bit range";
            continue;
        }
        if (feature.code == 0x12) {
            if (maximum == 0 || maximum > 0xFFFF || current > maximum) {
                feature.error = "Monitor returned an invalid contrast range";
                continue;
            }
        } else {
            // 非连续功能的 maximum 不是可写上限，只信任能力串中的明确枚举。
            if (feature.allowedValues.empty()) {
                feature.error = "Monitor does not advertise selectable values for this feature";
                continue;
            }
            if (std::find(feature.allowedValues.begin(), feature.allowedValues.end(),
                          static_cast<std::uint16_t>(current)) == feature.allowedValues.end()) {
                feature.error = "Current VCP value is absent from advertised values; do not infer its mode";
                // H24T7 returned an unmapped input value and produced persistent
                // corruption after source switching. A declaration alone cannot
                // authorize a write when the live device contradicts it. This
                // guard also applies to cached-capabilities and direct API calls.
                continue;
            }
        }
        feature.writable = true;
    }
    return result;
}

void readBrightness(HANDLE handle, MonitorInfo& monitor) {
    DWORD minimum{}, current{}, maximum{};
    if (GetMonitorBrightness(handle, &minimum, &current, &maximum)) {
        monitor.usesVcp = false;
    } else {
        MC_VCP_CODE_TYPE type{};
        if (!GetVCPFeatureAndVCPFeatureReply(handle, 0x10, &type, &current, &maximum)) {
            monitor.error = systemError("Read DDC/CI brightness (check DDC/CI in monitor menu)");
            return;
        }
        minimum = 0;
        monitor.usesVcp = true;
    }
    monitor.minBrightness = minimum;
    monitor.maxBrightness = maximum;
    monitor.currentBrightness = current;
    if (maximum <= minimum || current < minimum || current > maximum) {
        monitor.error = "Display returned an invalid brightness range";
        return;
    }
    monitor.brightnessPercent =
        static_cast<int>(std::lround(100.0 * (current - minimum) / (maximum - minimum)));
    monitor.brightnessReadable = true;
}
struct MonitorCollectionContext {
    std::vector<MonitorInfo> monitors;
    std::wstring requestedId;
    int requestedPercent = 0;
    bool found = false;
    std::string writeError;
    bool writing = false;
    // 高级功能复用同一套接口身份和物理句柄枚举；普通刷新不会执行此分支。
    std::function<void(HANDLE, const MonitorInfo&)> selectedAction;
    std::exception_ptr exception;
};
BOOL CALLBACK collectMonitors(HMONITOR handle, HDC, LPRECT, LPARAM parameter) {
    auto& context = *reinterpret_cast<MonitorCollectionContext*>(parameter);
    const bool targeted = context.writing || static_cast<bool>(context.selectedAction);
    try {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(handle, reinterpret_cast<MONITORINFO*>(&info)))
            return TRUE;
        auto base = monitorBase(info);
        if (base.devicePath.empty()) {
            base.error = "Stable monitor interface identity unavailable; brightness control disabled";
            if (!targeted)
                context.monitors.push_back(std::move(base));
            return TRUE;
        }
        if (targeted && base.id != context.requestedId)
            return TRUE;
        try {
            PhysicalMonitors physical(handle);
            if (physical.items().size() != 1) {
                // 驱动把多个面板归为一组时，物理下标不再是稳定身份。
                // 显示设备组，但不凭已保存下标猜测要控制哪块面板。
                base.error = "Multiple physical monitors share this interface; brightness control disabled";
                if (!targeted)
                    context.monitors.push_back(std::move(base));
                else if (base.id == context.requestedId) {
                    context.found = true;
                    context.writeError = base.error;
                    return FALSE;
                }
                return TRUE;
            }
            for (std::size_t i = 0; i < physical.items().size(); ++i) {
                auto monitor = base;
                monitor.id = base.devicePath + L"#physical:" + std::to_wstring(i);
                const auto& item = physical.items()[i];
                if (monitor.name == monitor.displayName && item.szPhysicalMonitorDescription[0])
                    monitor.name = item.szPhysicalMonitorDescription;
                if (targeted && monitor.id != context.requestedId)
                    continue;
                if (context.selectedAction) {
                    context.found = true;
                    if (!hasUniqueActiveTarget(monitor)) {
                        context.writeError = "Monitor topology is ambiguous or changed; refresh devices";
                        return FALSE;
                    }
                    context.selectedAction(item.hPhysicalMonitor, monitor);
                    return FALSE;
                }
                readBrightness(item.hPhysicalMonitor, monitor);
                if (context.writing) {
                    context.found = true;
                    if (!monitor.brightnessReadable) {
                        context.writeError = monitor.error;
                        return FALSE;
                    }
                    if (monitorDevicePath(base.displayName) != base.devicePath) {
                        context.writeError = "Monitor topology changed; refresh before setting brightness";
                        return FALSE;
                    }
                    const auto range = monitor.maxBrightness - monitor.minBrightness;
                    const DWORD raw = monitor.minBrightness +
                                      static_cast<DWORD>(std::lround(static_cast<double>(range) *
                                                                     context.requestedPercent / 100.0));
                    const BOOL success = monitor.usesVcp ? SetVCPFeature(item.hPhysicalMonitor, 0x10, raw)
                                                         : SetMonitorBrightness(item.hPhysicalMonitor, raw);
                    if (!success)
                        context.writeError = systemError("Set monitor brightness");
                    // 部分屏幕异步应用亮度；下次刷新再读回，不因暂时不一致重复写入。
                    return FALSE;
                }
                context.monitors.push_back(std::move(monitor));
            }
        } catch (const std::exception& error) {
            if (targeted) {
                if (base.id == context.requestedId) {
                    context.found = true;
                    context.writeError = error.what();
                    return FALSE;
                }
            } else {
                base.error = error.what();
                context.monitors.push_back(std::move(base));
            }
        }
    } catch (...) {
        context.exception = std::current_exception();
        return FALSE;
    }
    return TRUE;
}

void withSelectedMonitor(const std::wstring& monitorId,
                         std::function<void(HANDLE, const MonitorInfo&)> action) {
    if (monitorId.rfind(L"\\\\?\\", 0) != 0)
        throw std::invalid_argument("A stable monitor interface identity is required for advanced control");
    MonitorCollectionContext context;
    context.requestedId = monitorId;
    context.selectedAction = std::move(action);
    EnumDisplayMonitors(nullptr, nullptr, collectMonitors, reinterpret_cast<LPARAM>(&context));
    if (context.exception)
        std::rethrow_exception(context.exception);
    if (!context.found)
        throw std::runtime_error("Monitor disappeared or its display identity changed; refresh devices");
    if (!context.writeError.empty())
        throw std::runtime_error(context.writeError);
}

} // namespace

std::vector<MonitorInfo> WindowsBackend::enumerateMonitors() const {
    MonitorCollectionContext context;
    SetLastError(ERROR_SUCCESS);
    const BOOL success =
        EnumDisplayMonitors(nullptr, nullptr, collectMonitors, reinterpret_cast<LPARAM>(&context));
    if (context.exception)
        std::rethrow_exception(context.exception);
    if (!success)
        throw std::runtime_error(systemError("Enumerate monitors"));
    return context.monitors;
}
void WindowsBackend::setMonitorBrightness(const std::wstring& monitorId, int percent) const {
    if (percent < 0 || percent > 100)
        throw std::invalid_argument("Brightness must be between 0 and 100 percent");
    if (monitorId.rfind(L"\\\\?\\", 0) != 0)
        throw std::invalid_argument("A stable monitor interface identity is required for brightness control");
    MonitorCollectionContext context;
    context.writing = true;
    context.requestedId = monitorId;
    context.requestedPercent = percent;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitors, reinterpret_cast<LPARAM>(&context));
    if (context.exception)
        std::rethrow_exception(context.exception);
    if (!context.found)
        throw std::runtime_error("Monitor disappeared or its display identity changed; refresh devices");
    if (!context.writeError.empty())
        throw std::runtime_error(context.writeError);
}

MonitorControlsInfo WindowsBackend::queryMonitorControls(const std::wstring& monitorId) const {
    MonitorControlsInfo result;
    withSelectedMonitor(monitorId, [&](HANDLE handle, const MonitorInfo& monitor) {
        result = readMonitorControls(handle, monitor.id);
        if (!hasUniqueActiveTarget(monitor))
            throw std::runtime_error("Monitor topology changed while reading controls; refresh devices");
    });
    return result;
}

MonitorFeatureInfo WindowsBackend::queryMonitorFeature(const std::wstring& monitorId,
                                                       std::uint8_t code) const {
    requireAdvancedCode(code);
    MonitorFeatureInfo result;
    result.code = code;
    withSelectedMonitor(monitorId, [&](HANDLE handle, const MonitorInfo& monitor) {
        MC_VCP_CODE_TYPE type{};
        DWORD current{}, maximum{};
        if (GetVCPFeatureAndVCPFeatureReply(handle, code, &type, &current, &maximum)) {
            result.current = current;
            result.maximum = maximum;
            result.readable = current <= 0xFFFF;
            if (!result.readable)
                result.error = "Monitor returned a VCP current value outside the 16-bit range";
        } else {
            result.error = systemError("Read selected DDC/CI feature");
        }
        if (!hasUniqueActiveTarget(monitor))
            throw std::runtime_error("Monitor topology changed while reading a feature; refresh devices");
    });
    return result;
}

void WindowsBackend::setMonitorFeature(const std::wstring& monitorId, std::uint8_t code,
                                       std::uint16_t value) const {
    (void)setMonitorFeatureTimed(monitorId, code, value);
}

namespace {
MonitorWriteTimings writeMonitorFeatureChecked(const std::wstring& monitorId, std::uint8_t code,
                                               std::uint16_t value, const MonitorControlsInfo* declaration) {
    requireAdvancedCode(code);
    MonitorWriteTimings timings;
    const auto validationStart = std::chrono::steady_clock::now();
    withSelectedMonitor(monitorId, [&](HANDLE handle, const MonitorInfo& monitor) {
        // 声明文本重新解析；本项读数始终来自本次打开的同一物理句柄。
        // 仅查询用户本次选择的功能，避免写一次却读取全部高级功能。
        const auto controls =
            readMonitorControls(handle, monitor.id, code, declaration ? &declaration->capabilities : nullptr);
        if (!controls.capabilitiesError.empty())
            throw std::runtime_error(controls.capabilitiesError);
        const auto feature =
            std::find_if(controls.features.begin(), controls.features.end(),
                         [code](const MonitorFeatureInfo& item) { return item.code == code; });
        if (feature == controls.features.end() || !feature->advertised || !feature->readable ||
            !feature->writable)
            throw std::runtime_error(feature == controls.features.end() || feature->error.empty()
                                         ? "Monitor feature is unavailable for scalar VCP control"
                                         : feature->error);
        if (code == 0x12) {
            if (value > feature->maximum)
                throw std::out_of_range("Contrast exceeds the monitor's current maximum");
        } else if (std::find(feature->allowedValues.begin(), feature->allowedValues.end(), value) ==
                   feature->allowedValues.end()) {
            throw std::invalid_argument("Requested VCP value is not advertised by this monitor");
        }
        if (!hasUniqueActiveTarget(monitor))
            throw std::runtime_error("Monitor topology changed before setting a feature; refresh devices");
        const auto writeStart = std::chrono::steady_clock::now();
        timings.validationMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(writeStart - validationStart).count();
        // 仅发一次用户选择的命令。输入源可能断开当前链路，不保存或自动恢复旧值。
        const BOOL success = SetVCPFeature(handle, code, value);
        timings.writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - writeStart)
                              .count();
        if (!success)
            throw std::runtime_error(systemError("Set advertised DDC/CI feature"));
    });
    return timings;
}
} // namespace

MonitorWriteTimings WindowsBackend::setMonitorFeatureTimed(const std::wstring& monitorId, std::uint8_t code,
                                                           std::uint16_t value) const {
    return writeMonitorFeatureChecked(monitorId, code, value, nullptr);
}

MonitorWriteTimings
WindowsBackend::setMonitorFeatureFromCapabilities(const std::wstring& monitorId, std::uint8_t code,
                                                  std::uint16_t value,
                                                  const MonitorControlsInfo& declaration) const {
    if (declaration.id != monitorId)
        throw std::invalid_argument("Capabilities declaration belongs to a different monitor identity");
    if (!declaration.capabilitiesError.empty())
        throw std::invalid_argument("A failed capabilities read cannot authorize a cached write");
    // 只复用声明文本，不信任缓存中的 current、maximum 或 writable 字段。
    return writeMonitorFeatureChecked(monitorId, code, value, &declaration);
}

} // namespace peripheral::platform
