#pragma once
#include "core/device_features.h"
#include "core/device_model.h"
#include "platform/WindowsBackend.h"
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>
#include <QMap>
#include <QSet>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace peripheral::app {
struct RuntimeSnapshot {
    std::vector<platform::HidDeviceInfo> hid;
    std::vector<platform::AudioEndpointInfo> audio;
    std::vector<platform::MonitorInfo> monitors;
    struct BatteryReading {
        platform::VgnBatteryInfo value;
        qint64 sampledMs = 0;
        qint64 sampledMonotonicMs = 0; // Monotonic schedule; wall time is for display only.
        qint64 profileSampledMs = 0; // Last successful profile, not the last attempt.
        qint64 profileSampledMonotonicMs = 0;
        std::wstring containerId, serial;
    };
    std::map<std::wstring, BatteryReading> batteries;
    const BatteryReading* mouseForDevice(const DeviceSnapshot& device) const {
        if (device.kind != DeviceKind::Mouse)
            return nullptr;
        const BatteryReading* result = nullptr;
        for (const auto& endpoint : device.endpoints) {
            const auto path = QString::fromStdString(endpoint.endpointId).toStdWString();
            const auto it = batteries.find(path);
            if (it != batteries.end() && it->second.value.profile &&
                (!result || it->second.profileSampledMs > result->profileSampledMs))
                result = &it->second;
        }
        return result;
    }
    struct HeadsetReading {
        std::wstring containerId;
        platform::N9ControlsInfo value;
        qint64 sampledMs = 0;
        qint64 sampledMonotonicMs = 0;
    };
    std::map<std::wstring, HeadsetReading> headsets;
    // Audio and private HID are joined only by a unique, nonempty OS identity.
    // Names, VID/PID and generic serial strings do not establish this mapping.
    const HeadsetReading* headsetForContainer(const std::wstring& container) const {
        if (container.empty())
            return nullptr;
        const HeadsetReading* result = nullptr;
        for (const auto& [path, reading] : headsets)
            if (reading.containerId == container) {
                if (result)
                    return nullptr;
                result = &reading;
            }
        return result;
    }
    QStringList errors;
    qint64 sampledMs = 0;
    qint64 sampledMonotonicMs = 0;
    qint64 audioSampledMs = 0;
    qint64 audioSampledMonotonicMs = 0;
    qint64 monitorSampledMs = 0;
    qint64 monitorSampledMonotonicMs = 0;
    bool hidEnumerationSucceeded = false;
    bool audioEnumerationSucceeded = false;
    bool monitorEnumerationSucceeded = false;
};
class DeviceService final : public QObject {
    Q_OBJECT
  public:
    explicit DeviceService(bool passive, QObject* parent = nullptr);
    ~DeviceService() override;
    const RuntimeSnapshot& snapshot() const { return snapshot_; }
    void start();
    // 调整后台轮询，不改变连接通知、手动刷新和实时控制的派发间隔。
    bool setRefreshIntervalSeconds(int seconds);
    int refreshIntervalSeconds() const { return refreshTimer_.interval() / 1000; }
    void refresh(bool force = false);
    // 手动刷新使用票号核对完整新扫描，不能把局部回读当成该次刷新完成。
    quint64 requestRefresh();
    // 统一功能写入入口，复用各功能已有的队列与后端能力验证。
    void setFeature(const QString& id, DeviceFeature feature, int value);
    // 先选择目标再提交数值。切换或清空目标会取消待写值，旧目标的迟到事件被忽略。
    void setBrightness(const QString& id, int percent);
    void setVolume(const QString& id, int percent);
    void selectBrightnessTarget(const QString& id);
    void selectVolumeTarget(const QString& id, bool capture = false);
    void finishBrightness();
    void finishVolume(const QString& id = {});
    bool brightnessUpdatePending(const QString& id) const;
    bool volumeUpdatePending(const QString& id) const;
    void setMute(const QString& id, bool mute);
    void selectMonitorControlsTarget(const QString& id);
    void requestMonitorControls(const QString& id);
    void setMonitorFeature(const QString& id, int code, int value);
    const platform::MonitorControlsInfo& monitorControls() const { return monitorControls_; }
    bool monitorControlsBusy() const { return monitorControlsBusy_; }
    bool monitorControlsReadAttempted() const { return monitorControlsReadAttempted_; }
    void restoreSettings(const QString& profileId, const QString& targetId, bool monitor,
                         const QMap<QString, int>& values);
    // 实验性设备写入：默认关闭，只有用户显式开启后界面才调用。每次只写一条白名单
    // 记录，写后立即回读；不回滚、不重试。passive 模式下始终拒绝。
    void setExperimentalWritesEnabled(bool enabled) { experimentalWrites_ = enabled; }
    bool experimentalWritesEnabled() const { return experimentalWrites_; }
    void writeVgnDpiStage(const QString& deviceId, int stage, int x, int y);
    void writeVgnPollingRate(const QString& deviceId, int hertz);
    void writeVgnActiveStage(const QString& deviceId, int stage);
  signals:
    void refreshed();
    void refreshFinished(quint64 ticket, bool success, const QString& message);
    void monitorControlsChanged();
    void monitorFeatureProgress(const QString& message);
    void monitorFeatureFinished(const QString& message);
    void settingsRestored(const QString& profileId, bool success, const QString& message);
    void operationFinished(const QString& message, bool success);

  private:
    void command(std::function<void(const platform::WindowsBackend&)> action, QString description);
    bool findVgnDevice(const QString& deviceId, platform::HidDeviceInfo& device) const;
    // 所有实验性写入的统一闸门：未开启或 passive 模式时拒绝并说明原因。
    bool allowExperimentalWrite();
    // perform 在 worker 线程执行；失败时抛异常，成功时返回给用户看的说明。
    void applyVgnWrite(QString description, platform::HidDeviceInfo device,
                       std::function<QString(const platform::WindowsBackend&)> perform);
    bool experimentalWrites_ = false;
    enum class ControlKind { Brightness, Volume, InputVolume };
    struct LiveControl;
    LiveControl& control(ControlKind kind);
    const LiveControl& control(ControlKind kind) const;
    void selectTarget(ControlKind kind, const QString& id);
    void queueValue(ControlKind kind, const QString& id, int percent);
    void dispatchControl(ControlKind kind);
    void readBackControl(ControlKind kind);
    void resumeRefresh();
    bool liveControlsPending() const;
    void invalidateMonitorCapabilities();
    void readBackMonitorFeature(const QString& id, int code, int value, std::uint64_t generation,
                                qint64 requestedAt, bool finalAttempt = false);
    void finishMonitorFeature(const QString& id, int code, std::uint64_t generation, bool sent, bool verified,
                              const QString& error);
    struct WorkerState;
    QThread thread_;
    QObject* worker_ = nullptr;
    QTimer refreshTimer_;
    RuntimeSnapshot snapshot_;
    std::shared_ptr<WorkerState> state_;
    std::unique_ptr<LiveControl> brightnessControl_;
    std::unique_ptr<LiveControl> volumeControl_;
    std::unique_ptr<LiveControl> inputVolumeControl_;
    QElapsedTimer controlClock_;
    bool passive_ = false;
    bool busy_ = false;
    bool refreshAgain_ = false;
    bool refreshRequested_ = false;
    bool stopping_ = false;
    quint64 refreshTicketCounter_ = 0;
    quint64 pendingRefreshTicket_ = 0;
    QString monitorControlsTarget_;
    platform::MonitorControlsInfo monitorControls_;
    platform::MonitorControlsInfo monitorCapabilities_;
    qint64 monitorCapabilitiesAt_ = 0;
    std::uint64_t monitorCapabilitiesRevision_ = 0;
    std::uint64_t monitorControlsGeneration_ = 0;
    bool monitorControlsBusy_ = false;
    bool monitorControlsReadAttempted_ = false;
    bool monitorFeaturePending_ = false;
    QSet<QString> restoringProfiles_;
};
} // namespace peripheral::app
