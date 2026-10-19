#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "core/n9_protocol.h"
#include "core/vgn_config.h"

namespace peripheral::platform {

// 纯 C++ 数据结构，不依赖 Qt。路径和 ID 均来自枚举结果，不用设备名称猜测目标。
struct HidDeviceInfo {
    std::wstring path;
    std::wstring instanceId;
    std::wstring containerId;
    std::wstring serial;
    std::wstring manufacturer;
    std::wstring product;
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::uint16_t usagePage = 0;
    std::uint16_t usage = 0;
    std::uint16_t inputReportBytes = 0;
    std::uint16_t outputReportBytes = 0;
    std::uint16_t featureReportBytes = 0;
    int interfaceNumber = -1;
    bool vgnBatteryCandidate = false;
    bool n9ControlCandidate = false;
    std::string error;
};

struct AudioEndpointInfo {
    std::wstring id;
    std::wstring name;
    std::wstring containerId;
    bool capture = false;
    float volumePercent = 0.0F;
    bool muted = false;
    bool volumeReadable = false; // 为 true 时，volumePercent 与 muted 才是有效读数。
    std::string error;
};

struct MonitorInfo {
    // 使用稳定显示器接口身份；每次写入前重新枚举，不保存 HMONITOR/HANDLE。
    // unidentified: 开头的条目只供显示，不能用于亮度控制。
    std::wstring id;
    std::wstring name;
    std::wstring displayName;
    std::wstring devicePath;
    std::uint32_t minBrightness = 0;
    std::uint32_t maxBrightness = 0;
    std::uint32_t currentBrightness = 0;
    int brightnessPercent = 0;
    bool brightnessReadable = false;
    bool usesVcp = false;
    std::string error;
};

struct MonitorFeatureInfo {
    std::uint8_t code = 0;
    bool advertised = false;
    bool readable = false;
    bool writable = false; // 允许尝试设备声明的值，不等于已通过实际写入验收。
    std::uint32_t current = 0;
    std::uint32_t maximum = 0; // 仅连续控制有效；不能用作输入源/模式枚举上限。
    std::vector<std::uint16_t> allowedValues;
    std::string error;
};

struct MonitorControlsInfo {
    std::wstring id;
    std::int64_t sampledMs = 0; // 上层填入完成实际读取的时间，不能用到达 UI 的时间代替。
    std::int64_t sampledMonotonicMs = 0; // 调度与新鲜度使用的单调时间；sampledMs 仅用于显示。
    std::string capabilities;
    std::string capabilitiesError;
    std::string version;
    std::vector<MonitorFeatureInfo> features;
};

struct MonitorWriteTimings {
    std::int64_t validationMs = 0; // 身份、能力串和目标单项读数校验。
    std::int64_t writeMs = 0;      // 仅本次 SetVCPFeature 调用。
};

struct VgnBatteryInfo {
    bool supported = false;
    bool onlineKnown = false;
    bool deviceOnline = false;
    bool batteryValid = false; // 为 true 时，电量、充电状态与电压字段才有效。
    int percentage = 0;
    bool charging = false;
    int voltageMillivolts = 0;
    std::string error;
    std::optional<vgn::Profile> profile;
    std::string profileError; // Independent from a successful battery read.
};

struct N9ControlsInfo {
    bool supported = false;
    std::optional<n9::Status> status;
    std::array<std::optional<n9::PeqPoint>, 10> peq;
    std::string error;
};

struct N9EqModeResult {
    bool supported = false;
    bool sent = false;
    bool verified = false;
    int before = -1; // 读取到的当前模式；未知时为 -1。
    int requested = -1;
    std::optional<int> after;
    std::string error;
};

struct N9PeqWriteResult {
    bool supported = false;
    bool sent = false;      // 本次确实发出了 PEQ 写入帧。
    bool verified = false;  // 回读的参数与请求一致。
    bool unchanged = false; // 请求值与现值相同，未发送任何写入。
    std::optional<n9::PeqPoint> before;
    n9::PeqPoint requested;
    std::optional<n9::PeqPoint> after;
    std::string error;
};

struct VgnScalarWriteResult {
    bool sent = false;
    bool verified = false;
    bool unchanged = false;
    int before = -1; // 人类可读单位（Hz 或档位序号），未知时为 -1。
    int requested = -1;
    std::optional<int> after;
    std::string error;
};

struct VgnFlashWriteResult {
    bool sent = false;      // 本次确实发出了写入帧。
    bool verified = false;  // 回读字节与请求记录完全一致。
    bool unchanged = false; // 请求值与设备现值相同，未发送任何写入。
    std::array<std::uint8_t, 4> before{};
    std::array<std::uint8_t, 4> requested{};
    std::array<std::uint8_t, 4> after{};
    std::string error;
};

class WindowsBackend {
  public:
    // 无长期设备句柄，可直接创建并复用。所有调用需在工作线程串行执行；
    // HID/DDC 可能阻塞。枚举整体失败抛异常，单个设备失败记录到其 error 字段。
    std::vector<HidDeviceInfo> enumerateHidDevices() const;
    std::vector<AudioEndpointInfo> enumerateAudioEndpoints() const;
    std::vector<MonitorInfo> enumerateMonitors() const;

    // 仅修改明确选择的当前端点，范围均为 0..100；不切换系统默认音频设备。
    // 目标离线或写入失败抛异常。调用后由上层重新读取，以确认设备实际状态。
    void setAudioVolume(const std::wstring& endpointId, float percent) const;
    void setAudioMute(const std::wstring& endpointId, bool muted) const;
    void setMonitorBrightness(const std::wstring& monitorId, int percent) const;

    // 高级控制按需查询；只开放对比度、色彩预设、输入源、显示模式白名单。
    // capabilities 是设备自述，不能据此宣称实际模式名称或写入已验证。
    MonitorControlsInfo queryMonitorControls(const std::wstring& monitorId) const;
    // 写后只读回一个白名单功能，不再读取能力串；advertised/writable 保持 false，
    // 不能把该原始读回结果作为新的写入授权。调用时仍重新核对唯一目标和句柄。
    MonitorFeatureInfo queryMonitorFeature(const std::wstring& monitorId, std::uint8_t code) const;
    void setMonitorFeature(const std::wstring& monitorId, std::uint8_t code, std::uint16_t value) const;
    MonitorWriteTimings setMonitorFeatureTimed(const std::wstring& monitorId, std::uint8_t code,
                                               std::uint16_t value) const;
    // 显式复用本连接已验证的能力串；调用者负责同目标/generation、最长 5 分钟 TTL，
    // 并在强制刷新、拓扑或模式变化后失效。不能传入持久化或外部构造的声明。
    // 本次仍重新校验唯一身份、目标单项现值/范围；不再读取设备能力串。
    MonitorWriteTimings setMonitorFeatureFromCapabilities(const std::wstring& monitorId, std::uint8_t code,
                                                          std::uint16_t value,
                                                          const MonitorControlsInfo& declaration) const;

    // 查询电量后，再为 CID 118 / MID 35 的 F2 Master+ 读取配置；两个结果独立。
    // 不对其他 HID 发送私有查询，不包含配置或固件写入。
    VgnBatteryInfo queryVgnBattery(const HidDeviceInfo& device) const;
    // 单条 DPI 档位记录的写入验收入口：先通过精确身份与完整配置读取，再只写
    // 白名单中的那条 4 字节记录，最后回读同一记录。不重试、不写整幅镜像、
    // 不开放其他地址。发送成功不等于写入生效，只有 verified 为 true 才算回读一致。
    VgnFlashWriteResult writeVgnDpiStage(const HidDeviceInfo& device, int stage, int x, int y) const;
    // 两条标量记录（回报率、当前档位）的写入验收入口。写入前同样要求精确身份与
    // 完整配置读取；写入后回读同一记录。档位数量不开放写入：减少档位会让已有
    // 档位记录失去归属，收益不足以抵消风险。
    VgnScalarWriteResult writeVgnPollingRate(const HidDeviceInfo& device, int hertz) const;
    VgnScalarWriteResult writeVgnActiveStage(const HidDeviceInfo& device, int stage) const;
    // N9 V2 Ultra only: exact identity, FF90 collection and report 55/64 bytes.
    // Private control reads are separate from Windows endpoint volume/mute.
    N9ControlsInfo queryN9Controls(const HidDeviceInfo& device, bool includePeq = true) const;
    // 单点 PEQ 写入验收入口：先读该点现值，再只写这一条白名单帧，最后回读同一点。
    // 厂商实现不等待写入回包，所以只有 verified 才代表设备真的接受并保留了新值。
    N9PeqWriteResult writeN9PeqPoint(const HidDeviceInfo& device, int point, const n9::PeqPoint& value) const;
    // EQ 模式切换（0=Game1、1=Game2、2=Custom）。厂商在写 PEQ 点之前先切到 Custom，
    // 因此这是 PEQ 写入的前置步骤，本身也需要读回确认。
    N9EqModeResult setN9EqMode(const HidDeviceInfo& device, int mode) const;
};

} // namespace peripheral::platform
