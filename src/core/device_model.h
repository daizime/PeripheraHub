#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace peripheral {

// 墙上时钟只用于显示和档案时间；连接新鲜度、缓存年龄和迟到判断使用单调时钟，
// 避免系统时间校正后把旧读数当成新读数（或反过来）。
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
using MonotonicClock = std::chrono::steady_clock;
using MonotonicTimePoint = MonotonicClock::time_point;

enum class DeviceKind { Unknown, Mouse, Keyboard, Headset, Monitor };
enum class TransportKind { Receiver24G, WiredUsb, DisplayPort, Unknown };
enum class LinkState { Unknown, Online, Offline, TransportLost, Reconnecting };

struct Endpoint {
    std::string endpointId; // The OS interface path, not a VID/PID pair.
    std::string groupKey;   // Optional, trustworthy physical grouping supplied by discovery.
    std::string displayName;
    std::string containerId;
    std::string serial;
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    TransportKind transport = TransportKind::Unknown;
    DeviceKind kind = DeviceKind::Unknown;
    int interfaceNumber = -1;
    std::uint16_t usagePage = 0;
    std::uint16_t usage = 0;
};

struct DeviceCapabilities {
    bool battery = false;
    bool charging = false;
    bool brightness = false;
    bool volume = false;
    bool mute = false;
    bool dpi = false;
    bool pollingRate = false;
    bool rgb = false;
    bool keyMapping = false;
    bool eq = false;
};

struct BatteryState {
    std::optional<int> reportedPercent;
    std::optional<bool> charging;
};

struct DeviceSnapshot {
    std::string logicalId;
    std::string name;
    DeviceKind kind = DeviceKind::Unknown;
    std::vector<Endpoint> endpoints; // Currently enumerated endpoints only.
    bool receiverPresent = false;
    bool wiredPresent = false;
    LinkState linkState = LinkState::Unknown;
    BatteryState battery;
    std::optional<TimePoint> lastUpdated; // Timestamp of the cached battery reading.
    bool stale = true;
    std::string statusDetail;
    DeviceCapabilities caps;
};

struct DeviceObservation {
    LinkState linkState = LinkState::Unknown;
    std::optional<int> reportedPercent;
    std::optional<bool> charging;
    std::optional<DeviceCapabilities> caps;
    std::string statusDetail;
    // 仅供展示的墙上时间；注册表排序和新鲜度使用 observe/snapshots 的单调时间参数。
    // 为空时快照不提供 lastUpdated，避免用到达界面时间冒充采样时间。
    std::optional<TimePoint> sampledAt;
};

// Exact VID/PID evidence identifies a candidate. It does not establish that the
// device is online, nor does it authorize configuration writes.
bool isVgnF2MasterPlus(const Endpoint& endpoint);
bool isLangtuL98Candidate(const Endpoint& endpoint);

} // namespace peripheral
