#include "DeviceService.h"
#include "core/live_value_control.h"
#include "core/private_error_tracker.h"
#include "SettingsRestore.h"
#include <QDateTime>
#include <QDebug>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <array>
#include <atomic>
#include <set>
#include <chrono>
#include <algorithm>
#include <stdexcept>

namespace peripheral::app {
// 默认关闭逐次调节日志；验收时用 QT_LOGGING_RULES=peripheral.controls.debug=true 开启。
Q_LOGGING_CATEGORY(controlLog, "peripheral.controls", QtInfoMsg)
namespace {
qint64 monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace
struct DeviceService::WorkerState {
    std::map<std::wstring, RuntimeSnapshot::BatteryReading> batteryCache;
    std::map<std::wstring, RuntimeSnapshot::HeadsetReading> headsetCache;
    // 私有读数失败只在错误内容变化时记一次，避免轮询刷屏，同时留下故障起止时间。
    core::PrivateErrorTracker privateErrors;
    // 启动后第一次扫描只记录一次耗时，后续轮询不重复写日志。
    std::atomic<bool> initialScanLogged{false};
    std::vector<platform::MonitorInfo> monitors;
    qint64 monitorsMs = 0;
    qint64 monitorsMonotonicMs = 0;
    bool monitorsInvalidated = true;
    std::array<std::atomic<std::uint64_t>, 3> targetGenerations{};
    std::atomic<std::uint64_t> monitorControlsGeneration{0};
    std::atomic<bool> monitorFeaturePending{false};
    std::atomic<std::uint64_t> monitorCapabilitiesRevision{0};
};
struct DeviceService::LiveControl {
    explicit LiveControl(qint64 interval) : writes(interval) {
        dispatchTimer.setSingleShot(true);
        readbackTimer.setSingleShot(true);
    }
    LiveValueControl writes;
    QTimer dispatchTimer, readbackTimer;
    std::uint64_t revision = 0;
    bool awaitingReadback = false;
    bool readbackInFlight = false;
};
DeviceService::DeviceService(bool passive, QObject* parent)
    : QObject(parent), state_(std::make_shared<WorkerState>()),
      brightnessControl_(std::make_unique<LiveControl>(200)),
      volumeControl_(std::make_unique<LiveControl>(60)),
      inputVolumeControl_(std::make_unique<LiveControl>(60)), passive_(passive) {
    worker_ = new QObject;
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    refreshTimer_.setInterval(3000);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { refresh(); });
    controlClock_.start();
    for (const auto kind : {ControlKind::Brightness, ControlKind::Volume, ControlKind::InputVolume}) {
        connect(&control(kind).dispatchTimer, &QTimer::timeout, this,
                [this, kind] { dispatchControl(kind); });
        connect(&control(kind).readbackTimer, &QTimer::timeout, this,
                [this, kind] { readBackControl(kind); });
    }
    thread_.setObjectName("DeviceIO");
    thread_.start();
}
DeviceService::~DeviceService() {
    stopping_ = true;
    refreshTimer_.stop();
    for (const auto kind : {ControlKind::Brightness, ControlKind::Volume, ControlKind::InputVolume}) {
        control(kind).dispatchTimer.stop();
        control(kind).readbackTimer.stop();
    }
    thread_.quit();
    thread_.wait();
}
void DeviceService::start() {
    refreshTimer_.start();
    refresh(true);
}
bool DeviceService::setRefreshIntervalSeconds(int seconds) {
    if (seconds < 3 || seconds > 60)
        return false;
    if (refreshTimer_.interval() != seconds * 1000)
        refreshTimer_.setInterval(seconds * 1000);
    return true;
}
quint64 DeviceService::requestRefresh() {
    if (stopping_)
        return 0;
    const auto ticket = ++refreshTicketCounter_;
    pendingRefreshTicket_ = ticket;
    refresh(true);
    return ticket;
}

void DeviceService::refresh(bool force) {
    if (stopping_)
        return;
    if (force)
        invalidateMonitorCapabilities();
    // 拖动期间先合并常规扫描，避免 USB/DDC 读取挡在下一次调节前；平稳后补一次扫描。
    if (liveControlsPending() || monitorFeaturePending_) {
        refreshRequested_ = true;
        refreshAgain_ = refreshAgain_ || force;
        return;
    }
    if (busy_) {
        refreshAgain_ = refreshAgain_ || force;
        return;
    }
    busy_ = true;
    const auto ticket = pendingRefreshTicket_;
    force = force || ticket != 0;
    const auto state = state_;
    const bool passive = passive_;
    QPointer<DeviceService> self(this);
    QMetaObject::invokeMethod(
        worker_,
        [self, state, force, passive, ticket] {
            // 无法打断已进入驱动的调用；阶段之间可让出后台整轮扫描，先执行用户写入。
            const auto deferForMonitorWrite = [&] {
                if (!state->monitorFeaturePending.load())
                    return false;
                if (self)
                    QMetaObject::invokeMethod(
                        self,
                        [self, force] {
                            if (!self || self->stopping_)
                                return;
                            self->busy_ = false;
                            self->refreshRequested_ = true;
                            self->refreshAgain_ = self->refreshAgain_ || force;
                            self->resumeRefresh();
                        },
                        Qt::QueuedConnection);
                return true;
            };
            if (deferForMonitorWrite())
                return;
            RuntimeSnapshot result;
            platform::WindowsBackend backend;
            const auto now = QDateTime::currentMSecsSinceEpoch();
            const auto monoNow = monotonicMs();
            result.sampledMs = now;
            result.sampledMonotonicMs = monoNow;
            try {
                result.hid = backend.enumerateHidDevices();
                result.hidEnumerationSucceeded = true;
            } catch (const std::exception& e) {
                result.errors << QString("USB 设备读取失败：%1").arg(QString::fromUtf8(e.what()));
            }
            if (deferForMonitorWrite())
                return;
            try {
                result.audio = backend.enumerateAudioEndpoints();
                result.audioSampledMs = QDateTime::currentMSecsSinceEpoch();
                result.audioSampledMonotonicMs = monotonicMs();
                result.audioEnumerationSucceeded = true;
            } catch (const std::exception& e) {
                result.errors << QString("音频设备读取失败：%1").arg(QString::fromUtf8(e.what()));
            }
            if (deferForMonitorWrite())
                return;
            if (force || state->monitorsInvalidated || monoNow - state->monitorsMonotonicMs >= 15000) {
                try {
                    state->monitors = backend.enumerateMonitors();
                    state->monitorsMs = QDateTime::currentMSecsSinceEpoch();
                    state->monitorsMonotonicMs = monotonicMs();
                    state->monitorsInvalidated = false;
                } catch (const std::exception& e) {
                    state->monitors.clear();
                    state->monitorsInvalidated = true;
                    result.errors << QString("显示器读取失败：%1").arg(QString::fromUtf8(e.what()));
                }
            }
            result.monitors = state->monitors;
            result.monitorSampledMs = state->monitorsMs;
            result.monitorSampledMonotonicMs = state->monitorsMonotonicMs;
            result.monitorEnumerationSucceeded = !state->monitorsInvalidated;
            if (!passive && result.hidEnumerationSucceeded) {
                std::set<std::wstring> present;
                std::set<std::wstring> presentHeadsets;
                // Edge-triggered: a private read that starts failing, changes
                // its reason, or recovers is recorded once, not every poll.
                const auto notePrivateError = [&state](const std::wstring& path, const std::string& error,
                                                       const char* kind) {
                    switch (state->privateErrors.update(path, error)) {
                    case core::PrivateErrorTracker::Change::Failed:
                        qCInfo(controlLog) << kind << "read failed" << QString::fromStdWString(path)
                                           << QString::fromStdString(error);
                        break;
                    case core::PrivateErrorTracker::Change::Recovered:
                        qCInfo(controlLog) << kind << "read recovered" << QString::fromStdWString(path);
                        break;
                    case core::PrivateErrorTracker::Change::None:
                        break;
                    }
                };
                for (const auto& device : result.hid) {
                    if (deferForMonitorWrite())
                        return;
                    if (device.n9ControlCandidate) {
                        presentHeadsets.insert(device.path);
                        const auto it = state->headsetCache.find(device.path);
                        if (force || it == state->headsetCache.end() ||
                            monoNow - it->second.sampledMonotonicMs >= 20000) {
                            RuntimeSnapshot::HeadsetReading reading;
                            reading.containerId = device.containerId;
                            try {
                                reading.value = backend.queryN9Controls(device);
                            } catch (const std::exception& error) {
                                reading.value.error = error.what();
                            }
                            reading.sampledMs = QDateTime::currentMSecsSinceEpoch();
                            reading.sampledMonotonicMs = monotonicMs();
                            const auto headsetError = reading.value.supported ? reading.value.error : std::string{};
                            state->headsetCache[device.path] = std::move(reading);
                            notePrivateError(device.path, headsetError, "headset");
                        }
                    }
                    if (!device.vgnBatteryCandidate)
                        continue;
                    present.insert(device.path);
                    auto it = state->batteryCache.find(device.path);
                    if (it != state->batteryCache.end() &&
                        (it->second.containerId != device.containerId || it->second.serial != device.serial)) {
                        // A reused OS path does not transfer the old physical
                        // device's profile to a newly enumerated identity.
                        state->batteryCache.erase(it);
                        it = state->batteryCache.end();
                    }
                    if (force || it == state->batteryCache.end() ||
                        monoNow - it->second.sampledMonotonicMs >= 20000) {
                        RuntimeSnapshot::BatteryReading reading;
                        reading.containerId = device.containerId;
                        reading.serial = device.serial;
                        try {
                            reading.value = backend.queryVgnBattery(device);
                        } catch (const std::exception& e) {
                            reading.value.error = e.what();
                        }
                        reading.sampledMs = QDateTime::currentMSecsSinceEpoch();
                        reading.sampledMonotonicMs = monotonicMs();
                        if (reading.value.profile) {
                            reading.profileSampledMs = reading.sampledMs;
                            reading.profileSampledMonotonicMs = reading.sampledMonotonicMs;
                        } else if (it != state->batteryCache.end()) {
                            // Keep the last successful sample with its own age;
                            // a failed attempt must never make cached DPI fresh.
                            reading.value.profile = it->second.value.profile;
                            reading.profileSampledMs = it->second.profileSampledMs;
                            reading.profileSampledMonotonicMs =
                                it->second.profileSampledMonotonicMs;
                            if (reading.value.profileError.empty())
                                reading.value.profileError = "Profile read failed";
                        }
                        std::string mouseError = reading.value.error;
                        if (mouseError.empty())
                            mouseError = reading.value.profileError;
                        state->batteryCache[device.path] = std::move(reading);
                        notePrivateError(device.path, mouseError, "mouse");
                    }
                }
                for (auto it = state->batteryCache.begin(); it != state->batteryCache.end();) {
                    if (!present.contains(it->first))
                        it = state->batteryCache.erase(it);
                    else
                        ++it;
                }
                result.batteries = state->batteryCache;
                for (auto it = state->headsetCache.begin(); it != state->headsetCache.end();)
                    if (!presentHeadsets.contains(it->first))
                        it = state->headsetCache.erase(it);
                    else
                        ++it;
                result.headsets = state->headsetCache;
                // 移除设备的错误记忆，重新接入时从干净状态开始记录。
                std::set<std::wstring> stillPresent = present;
                stillPresent.insert(presentHeadsets.begin(), presentHeadsets.end());
                state->privateErrors.retain(stillPresent);
            }
            if (deferForMonitorWrite())
                return;
            if (!self)
                return;
            // 启动后第一次完整扫描的耗时：用于对比"启动到可交互"是否退化。
            if (!state->initialScanLogged.exchange(true)) {
                qInfo() << "Initial scan completed in" << monotonicMs() - monoNow << "ms"
                        << "usb" << result.hidEnumerationSucceeded << "audio"
                        << result.audioEnumerationSucceeded << "monitor"
                        << result.monitorEnumerationSucceeded << "errors" << result.errors.size();
            }
            QMetaObject::invokeMethod(
                self,
                [self, ticket, result = std::move(result)]() mutable {
                    if (!self || self->stopping_)
                        return;
                    self->snapshot_ = std::move(result);
                    self->busy_ = false;
                    emit self->refreshed();
                    if (ticket != 0 && ticket == self->pendingRefreshTicket_) {
                        self->pendingRefreshTicket_ = 0;
                        const auto& current = self->snapshot_;
                        const auto sampled =
                            std::max({current.sampledMs, current.audioSampledMs, current.monitorSampledMs});
                        const auto time = QDateTime::fromMSecsSinceEpoch(sampled).toString("HH:mm:ss");
                        const bool success = current.errors.isEmpty();
                        const auto message = success
                                                 ? QString("状态已刷新 · 采样时间 %1").arg(time)
                                                 : QString("刷新完成，但有读取失败 · 采样时间 %1\n%2")
                                                       .arg(time, current.errors.join(QStringLiteral("；")));
                        emit self->refreshFinished(ticket, success, message);
                    }
                    self->resumeRefresh();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void DeviceService::command(std::function<void(const platform::WindowsBackend&)> action,
                            QString description) {
    if (stopping_)
        return;
    QPointer<DeviceService> self(this);
    auto state = state_;
    QMetaObject::invokeMethod(
        worker_,
        [self, state, action = std::move(action), description] {
            bool success = true;
            QString message = description + "请求已发送，正在读取状态";
            try {
                platform::WindowsBackend backend;
                action(backend);
            } catch (const std::exception& e) {
                success = false;
                message = description + "失败：" + QString::fromUtf8(e.what());
            }
            state->monitorsInvalidated = true;
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self,
                [self, message, success] {
                    if (!self || self->stopping_)
                        return;
                    qInfo().noquote() << message;
                    emit self->operationFinished(message, success);
                    self->refresh(true);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
bool DeviceService::findVgnDevice(const QString& deviceId, platform::HidDeviceInfo& device) const {
    for (const auto& candidate : snapshot_.hid) {
        if (!candidate.vgnBatteryCandidate)
            continue;
        if (QString::fromStdWString(candidate.path) == deviceId) {
            device = candidate;
            return true;
        }
    }
    return false;
}

bool DeviceService::allowExperimentalWrite() {
    if (stopping_)
        return false;
    if (passive_ || !experimentalWrites_) {
        emit operationFinished(QStringLiteral("实验性设备写入未开启，未发送任何写入"), false);
        return false;
    }
    return true;
}

void DeviceService::applyVgnWrite(QString description, platform::HidDeviceInfo device,
                                  std::function<QString(const platform::WindowsBackend&)> perform) {
    if (stopping_)
        return;
    if (passive_ || !experimentalWrites_) {
        emit operationFinished(QStringLiteral("实验性设备写入未开启，未发送任何写入"), false);
        return;
    }
    auto state = state_;
    QPointer<DeviceService> self(this);
    QMetaObject::invokeMethod(
        worker_,
        [self, state, device, description, perform = std::move(perform)] {
            QString message;
            bool success = false;
            try {
                platform::WindowsBackend backend;
                message = perform(backend);
                success = true;
            } catch (const std::exception& error) {
                message = description + "失败：" + QString::fromUtf8(error.what());
            }
            // 写入后丢弃该接口的读数缓存，下一次扫描必须重新向设备读取，
            // 不能用写入前的采样冒充写入结果。
            state->batteryCache.erase(device.path);
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self,
                [self, message, success] {
                    if (!self || self->stopping_)
                        return;
                    qInfo().noquote() << message;
                    emit self->operationFinished(message, success);
                    self->refresh(true);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void DeviceService::writeVgnDpiStage(const QString& deviceId, int stage, int x, int y) {
    if (!allowExperimentalWrite())
        return;
    platform::HidDeviceInfo device;
    if (!findVgnDevice(deviceId, device)) {
        emit operationFinished(QStringLiteral("目标鼠标当前不可用，未发送写入"), false);
        return;
    }
    const auto label = QStringLiteral("写入 DPI 档位 %1").arg(stage + 1);
    applyVgnWrite(label, device, [device, stage, x, y](const platform::WindowsBackend& backend) {
        const auto result = backend.writeVgnDpiStage(device, stage, x, y);
        if (result.unchanged)
            return QStringLiteral("DPI 档位 %1 已是 %2×%3，未发送写入")
                .arg(stage + 1)
                .arg(x)
                .arg(y);
        if (!result.verified)
            throw std::runtime_error(result.error.empty() ? "回读与请求不一致" : result.error);
        return QStringLiteral("DPI 档位 %1 已写入并回读一致（%2×%3）").arg(stage + 1).arg(x).arg(y);
    });
}

void DeviceService::writeVgnPollingRate(const QString& deviceId, int hertz) {
    if (!allowExperimentalWrite())
        return;
    platform::HidDeviceInfo device;
    if (!findVgnDevice(deviceId, device)) {
        emit operationFinished(QStringLiteral("目标鼠标当前不可用，未发送写入"), false);
        return;
    }
    applyVgnWrite(QStringLiteral("写入回报率"), device,
                  [device, hertz](const platform::WindowsBackend& backend) {
                      const auto result = backend.writeVgnPollingRate(device, hertz);
                      if (result.unchanged)
                          return QStringLiteral("回报率已是 %1 Hz，未发送写入").arg(hertz);
                      if (!result.verified)
                          throw std::runtime_error(result.error.empty() ? "回读与请求不一致" : result.error);
                      return QStringLiteral("回报率已写入并回读一致（%1 Hz）").arg(hertz);
                  });
}

void DeviceService::writeVgnActiveStage(const QString& deviceId, int stage) {
    if (!allowExperimentalWrite())
        return;
    platform::HidDeviceInfo device;
    if (!findVgnDevice(deviceId, device)) {
        emit operationFinished(QStringLiteral("目标鼠标当前不可用，未发送写入"), false);
        return;
    }
    applyVgnWrite(QStringLiteral("切换当前档位"), device,
                  [device, stage](const platform::WindowsBackend& backend) {
                      const auto result = backend.writeVgnActiveStage(device, stage);
                      if (result.unchanged)
                          return QStringLiteral("当前已是第 %1 档，未发送写入").arg(stage + 1);
                      if (!result.verified)
                          throw std::runtime_error(result.error.empty() ? "回读与请求不一致" : result.error);
                      return QStringLiteral("当前档位已切换为第 %1 档并回读一致").arg(stage + 1);
                  });
}
DeviceService::LiveControl& DeviceService::control(ControlKind kind) {
    if (kind == ControlKind::Brightness)
        return *brightnessControl_;
    return kind == ControlKind::InputVolume ? *inputVolumeControl_ : *volumeControl_;
}
const DeviceService::LiveControl& DeviceService::control(ControlKind kind) const {
    if (kind == ControlKind::Brightness)
        return *brightnessControl_;
    return kind == ControlKind::InputVolume ? *inputVolumeControl_ : *volumeControl_;
}
bool DeviceService::liveControlsPending() const {
    for (const auto kind : {ControlKind::Brightness, ControlKind::Volume, ControlKind::InputVolume}) {
        const auto& channel = control(kind);
        if (channel.writes.active() || channel.awaitingReadback)
            return true;
    }
    return false;
}
void DeviceService::selectTarget(ControlKind kind, const QString& id) {
    auto& channel = control(kind);
    if (!channel.writes.selectTarget(id.toStdWString()))
        return;
    channel.dispatchTimer.stop();
    channel.readbackTimer.stop();
    channel.awaitingReadback = false;
    ++channel.revision;
    state_->targetGenerations[static_cast<std::size_t>(kind)].store(channel.writes.generation());
    resumeRefresh();
}
void DeviceService::selectBrightnessTarget(const QString& id) {
    selectTarget(ControlKind::Brightness, id);
}
void DeviceService::selectVolumeTarget(const QString& id, bool capture) {
    // 两个音频方向分别保留目标、节流和回读状态，切换麦克风不会取消播放音量。
    selectTarget(capture ? ControlKind::InputVolume : ControlKind::Volume, id);
}
bool DeviceService::brightnessUpdatePending(const QString& id) const {
    const auto& channel = control(ControlKind::Brightness);
    return channel.writes.target() == id.toStdWString() &&
           (channel.writes.active() || channel.awaitingReadback);
}
bool DeviceService::volumeUpdatePending(const QString& id) const {
    if (id.isEmpty())
        return false;
    for (const auto kind : {ControlKind::Volume, ControlKind::InputVolume}) {
        const auto& channel = control(kind);
        if (channel.writes.target() == id.toStdWString() &&
            (channel.writes.active() || channel.awaitingReadback))
            return true;
    }
    return false;
}
void DeviceService::queueValue(ControlKind kind, const QString& id, int percent) {
    if (stopping_)
        return;
    auto& channel = control(kind);
    if (!channel.writes.request(id.toStdWString(), percent))
        return;
    ++channel.revision;
    channel.awaitingReadback = true;
    channel.readbackTimer.stop();
    dispatchControl(kind);
}
void DeviceService::setFeature(const QString& id, DeviceFeature feature, int value) {
    if (stopping_)
        return;
    const auto vcp = monitorVcpCode(feature);
    const bool percent = feature == DeviceFeature::Brightness || feature == DeviceFeature::Volume;
    const bool mute = feature == DeviceFeature::Mute;
    if (!percent && !mute && !vcp) {
        const bool readOnly = feature == DeviceFeature::Battery || feature == DeviceFeature::Charging;
        emit operationFinished(readOnly ? "该功能仅供读取，不能设置。" : "该功能尚未适配，无法设置。", false);
        return;
    }
    const int maximum = percent ? 100 : (mute ? 1 : 65535);
    if (id.isEmpty() || value < 0 || value > maximum) {
        emit operationFinished("功能设置被拒绝：目标为空或数值超出允许范围。", false);
        return;
    }
    // 保留原有亮度、音量合并队列；不在统一入口直接调用设备或绕过验证。
    if (feature == DeviceFeature::Brightness)
        setBrightness(id, value);
    else if (feature == DeviceFeature::Volume)
        setVolume(id, value);
    else if (mute)
        setMute(id, value != 0);
    else
        setMonitorFeature(id, *vcp, value);
}

void DeviceService::setBrightness(const QString& id, int percent) {
    queueValue(ControlKind::Brightness, id, percent);
}
void DeviceService::setVolume(const QString& id, int percent) {
    const auto kind = !id.isEmpty() && inputVolumeControl_->writes.target() == id.toStdWString()
                          ? ControlKind::InputVolume
                          : ControlKind::Volume;
    queueValue(kind, id, percent);
}
void DeviceService::finishBrightness() {
    brightnessControl_->writes.flush();
    dispatchControl(ControlKind::Brightness);
}
void DeviceService::finishVolume(const QString& id) {
    const auto kind = !id.isEmpty() && inputVolumeControl_->writes.target() == id.toStdWString()
                          ? ControlKind::InputVolume
                          : ControlKind::Volume;
    auto& channel = control(kind);
    if (!id.isEmpty() && channel.writes.target() != id.toStdWString())
        return;
    channel.writes.flush();
    dispatchControl(kind);
}

void DeviceService::dispatchControl(ControlKind kind) {
    if (stopping_)
        return;
    auto& channel = control(kind);
    const auto request = channel.writes.take(controlClock_.elapsed());
    if (!request) {
        if (const auto delay = channel.writes.delay(controlClock_.elapsed()))
            channel.dispatchTimer.start(static_cast<int>(*delay));
        return;
    }
    channel.dispatchTimer.stop();
    const auto state = state_;
    QPointer<DeviceService> self(this);
    QMetaObject::invokeMethod(
        worker_,
        [self, state, kind, request = *request] {
            const bool cancelled =
                state->targetGenerations[static_cast<std::size_t>(kind)].load() != request.generation;
            bool success = !cancelled;
            QString error;
            if (!cancelled) {
                try {
                    platform::WindowsBackend backend;
                    qCDebug(controlLog)
                        << "write" << (kind == ControlKind::Brightness ? "brightness" : "volume")
                        << request.percent << "at" << QDateTime::currentMSecsSinceEpoch();
                    if (kind == ControlKind::Brightness)
                        backend.setMonitorBrightness(request.target, request.percent);
                    else
                        backend.setAudioVolume(request.target, static_cast<float>(request.percent));
                } catch (const std::exception& e) {
                    success = false;
                    error = QString::fromUtf8(e.what());
                }
            }
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self,
                [self, kind, request, success, cancelled, error] {
                    if (!self || self->stopping_)
                        return;
                    auto& completed = self->control(kind);
                    if (!completed.writes.complete(request.token))
                        return;
                    const bool current = request.generation == completed.writes.generation();
                    if (current && !cancelled && !success) {
                        const auto message =
                            (kind == ControlKind::Brightness ? QString("调整显示器亮度失败：")
                                                             : QString("调整音量失败：")) +
                            error;
                        qWarning().noquote() << message;
                        emit self->operationFinished(message, false);
                    }
                    if (completed.writes.hasPending())
                        self->dispatchControl(kind);
                    else if (completed.awaitingReadback)
                        completed.readbackTimer.start(kind == ControlKind::Brightness ? 250 : 120);
                    self->resumeRefresh();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void DeviceService::readBackControl(ControlKind kind) {
    auto& channel = control(kind);
    if (stopping_ || channel.readbackInFlight || channel.writes.active() || !channel.awaitingReadback)
        return;
    channel.readbackInFlight = true;
    const auto revision = channel.revision;
    const auto target = channel.writes.target();
    const auto state = state_;
    QPointer<DeviceService> self(this);
    // 平稳后只读回对应类别，避免每个滑动采样都触发完整扫描。
    // 读回期间若收到新数值，旧版本结果不回填界面。
    QMetaObject::invokeMethod(
        worker_,
        [self, state, kind, revision, target] {
            std::vector<platform::AudioEndpointInfo> audio;
            std::vector<platform::MonitorInfo> monitors;
            QString error;
            try {
                platform::WindowsBackend backend;
                if (kind == ControlKind::Brightness) {
                    monitors = backend.enumerateMonitors();
                    state->monitors = monitors;
                    state->monitorsMs = QDateTime::currentMSecsSinceEpoch();
                    state->monitorsMonotonicMs = monotonicMs();
                    state->monitorsInvalidated = false;
                } else
                    audio = backend.enumerateAudioEndpoints();
            } catch (const std::exception& e) {
                error = QString::fromUtf8(e.what());
            }
            if (!self)
                return;
            const auto sampledMs = QDateTime::currentMSecsSinceEpoch();
            const auto sampledMonotonicMs = monotonicMs();
            QMetaObject::invokeMethod(
                self,
                [self, kind, revision, target, error, sampledMs, sampledMonotonicMs,
                 audio = std::move(audio), monitors = std::move(monitors)]() mutable {
                    if (!self || self->stopping_)
                        return;
                    auto& finished = self->control(kind);
                    finished.readbackInFlight = false;
                    if (revision == finished.revision && target == finished.writes.target() &&
                        !finished.writes.active()) {
                        finished.awaitingReadback = false;
                        if (error.isEmpty()) {
                            if (kind == ControlKind::Brightness) {
                                self->snapshot_.monitors = std::move(monitors);
                                self->snapshot_.monitorSampledMs = sampledMs;
                                self->snapshot_.monitorSampledMonotonicMs = sampledMonotonicMs;
                                self->snapshot_.monitorEnumerationSucceeded = true;
                            } else {
                                self->snapshot_.audio = std::move(audio);
                                self->snapshot_.audioSampledMs = sampledMs;
                                self->snapshot_.audioSampledMonotonicMs = sampledMonotonicMs;
                                self->snapshot_.audioEnumerationSucceeded = true;
                            }
                            emit self->refreshed();
                        } else {
                            const auto message = QString("调整后读取状态失败：") + error;
                            qWarning().noquote() << message;
                            emit self->operationFinished(message, false);
                        }
                    } else if (finished.awaitingReadback && !finished.writes.active())
                        finished.readbackTimer.start(0);
                    self->resumeRefresh();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void DeviceService::resumeRefresh() {
    if (busy_ || (!refreshRequested_ && !refreshAgain_))
        return;
    if (liveControlsPending() || monitorFeaturePending_)
        return;
    const bool force = refreshAgain_;
    refreshAgain_ = false;
    refreshRequested_ = false;
    refresh(force);
}
void DeviceService::setMute(const QString& id, bool mute) {
    if (!id.isEmpty())
        command([id, mute](const auto& b) { b.setAudioMute(id.toStdWString(), mute); },
                mute ? "设为静音" : "取消静音");
}

void DeviceService::invalidateMonitorCapabilities() {
    monitorCapabilities_ = {};
    monitorCapabilitiesAt_ = 0;
    state_->monitorCapabilitiesRevision.store(++monitorCapabilitiesRevision_);
}

void DeviceService::selectMonitorControlsTarget(const QString& id) {
    if (monitorControlsTarget_ == id)
        return;
    monitorControlsTarget_ = id;
    invalidateMonitorCapabilities();
    state_->monitorControlsGeneration.store(++monitorControlsGeneration_);
    monitorControls_ = {};
    monitorControls_.id = id.toStdWString();
    monitorControlsBusy_ = false;
    monitorControlsReadAttempted_ = false;
    monitorFeaturePending_ = false;
    state_->monitorFeaturePending.store(false);
    emit monitorControlsChanged();
    resumeRefresh();
}

void DeviceService::restoreSettings(const QString& profileId, const QString& targetId, bool monitor,
                                    const QMap<QString, int>& values) {
    if (stopping_ || targetId.isEmpty() || values.isEmpty() || restoringProfiles_.contains(profileId))
        return;
    restoringProfiles_.insert(profileId);
    std::map<std::string, int> settings;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        settings.emplace(it.key().toStdString(), it.value());
    const auto state = state_;
    QPointer<DeviceService> self(this);
    QMetaObject::invokeMethod(
        worker_,
        [self, state, profileId, targetId, monitor, values = std::move(settings)] {
            RestoreResult result;
            try {
                result = restoreDeviceSettings(platform::WindowsBackend{}, targetId.toStdWString(), monitor,
                                               values);
            } catch (const std::exception& error) {
                result.success = false;
                result.message = error.what();
            }
            if (monitor)
                state->monitorsInvalidated = true;
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self,
                [self, profileId, result = std::move(result)] {
                    if (!self || self->stopping_)
                        return;
                    self->restoringProfiles_.remove(profileId);
                    const auto message = QString::fromStdString(result.message);
                    qInfo().noquote() << "设备设置恢复：" << message;
                    emit self->settingsRestored(profileId, result.success, message);
                    self->refresh(true);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void DeviceService::requestMonitorControls(const QString& id) {
    if (stopping_ || id.isEmpty() || id != monitorControlsTarget_ || monitorControlsBusy_)
        return;
    // 新完整查询失败时不能继续沿用旧声明；排队中的旧查询也不能重新激活缓存。
    invalidateMonitorCapabilities();
    monitorControlsBusy_ = true;
    monitorControlsReadAttempted_ = true;
    emit monitorControlsChanged();
    const auto generation = monitorControlsGeneration_;
    const auto capabilitiesRevision = monitorCapabilitiesRevision_;
    const auto state = state_;
    QPointer<DeviceService> self(this);
    QMetaObject::invokeMethod(
        worker_,
        [self, state, generation, capabilitiesRevision, id] {
            if (state->monitorControlsGeneration.load() != generation)
                return;
            platform::MonitorControlsInfo result;
            result.id = id.toStdWString();
            try {
                result = platform::WindowsBackend{}.queryMonitorControls(result.id);
                result.sampledMs = QDateTime::currentMSecsSinceEpoch();
                result.sampledMonotonicMs = monotonicMs();
            } catch (const std::exception& e) {
                result.capabilitiesError = e.what();
            }
            if (!self)
                return;
            const auto sampledAt = monotonicMs();
            QMetaObject::invokeMethod(
                self,
                [self, generation, capabilitiesRevision, sampledAt, result = std::move(result)]() mutable {
                    if (!self || self->stopping_ || generation != self->monitorControlsGeneration_)
                        return;
                    if (capabilitiesRevision == self->monitorCapabilitiesRevision_ &&
                        result.capabilitiesError.empty() && !result.capabilities.empty()) {
                        self->monitorCapabilities_ = result;
                        self->monitorCapabilitiesAt_ = sampledAt;
                    }
                    self->monitorControls_ = std::move(result);
                    self->monitorControlsBusy_ = false;
                    emit self->monitorControlsChanged();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void DeviceService::setMonitorFeature(const QString& id, int code, int value) {
    if (stopping_ || id.isEmpty() || id != monitorControlsTarget_ || monitorControlsBusy_ || code < 0 ||
        code > 255 || value < 0 || value > 65535)
        return;
    monitorControlsBusy_ = true;
    monitorFeaturePending_ = true;
    state_->monitorFeaturePending.store(true);
    emit monitorControlsChanged();
    const auto generation = monitorControlsGeneration_;
    const auto state = state_;
    const auto requestedAt = monotonicMs();
    const auto declarationAt = monitorCapabilitiesAt_;
    const auto age = requestedAt - monitorCapabilitiesAt_;
    const bool freshDeclaration = monitorCapabilities_.id == id.toStdWString() &&
                                  monitorCapabilitiesAt_ > 0 && age >= 0 && age <= 5 * 60 * 1000;
    const auto declaration = freshDeclaration ? monitorCapabilities_ : platform::MonitorControlsInfo{};
    if (code == 0x14 || code == 0x60 || code == 0xDC)
        invalidateMonitorCapabilities(); // 本次命令可用已有声明，后续命令须等待模式变化后的新声明。
    const auto capabilitiesRevision = monitorCapabilitiesRevision_;
    QPointer<DeviceService> self(this);
    QMetaObject::invokeMethod(
        worker_,
        [self, state, generation, capabilitiesRevision, declaration, declarationAt, id, code, value,
         requestedAt] {
            if (state->monitorControlsGeneration.load() != generation)
                return;
            const auto startedAt = monotonicMs();
            bool sent = false;
            QString error;
            try {
                platform::WindowsBackend backend;
                const auto declarationAge = startedAt - declarationAt;
                const bool cached = !declaration.capabilities.empty() && declarationAt > 0 &&
                                    declarationAge >= 0 && declarationAge <= 5 * 60 * 1000 &&
                                    state->monitorCapabilitiesRevision.load() == capabilitiesRevision;
                const auto timings =
                    cached
                        ? backend.setMonitorFeatureFromCapabilities(
                              id.toStdWString(), static_cast<std::uint8_t>(code),
                              static_cast<std::uint16_t>(value), declaration)
                        : backend.setMonitorFeatureTimed(id.toStdWString(), static_cast<std::uint8_t>(code),
                                                         static_cast<std::uint16_t>(value));
                sent = true;
                state->monitorsInvalidated = true;
                qCInfo(controlLog) << "monitor feature sent" << id << "code" << code << "value" << value
                                    << "queue_ms" << startedAt - requestedAt << "validation_ms"
                                    << timings.validationMs << "write_ms" << timings.writeMs
                                    << "capabilities_cached" << cached;
            } catch (const std::exception& e) {
                error = QString::fromUtf8(e.what());
                qCInfo(controlLog) << "monitor feature failed" << id << "code" << code << "queue_ms"
                                    << startedAt - requestedAt << "validation_ms" << -1 << "write_ms" << -1
                                    << "attempt_ms" << monotonicMs() - startedAt;
            }
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self,
                [self, generation, id, code, value, requestedAt, sent, error] {
                    if (!self || self->stopping_ || generation != self->monitorControlsGeneration_)
                        return;
                    if (!sent) {
                        self->finishMonitorFeature(id, code, generation, false, false, error);
                        return;
                    }
                    emit self->monitorFeatureProgress("请求已发送，正在确认显示器实际状态…");
                    // 等待显示器应用预设时让出工作线程；普通轮询仍暂停，避免长扫描抢占回读。
                    QTimer::singleShot(
                        code == 0x12 ? 0 : 250, self, [self, id, code, value, generation, requestedAt] {
                            if (self && !self->stopping_ && generation == self->monitorControlsGeneration_)
                                self->readBackMonitorFeature(id, code, value, generation, requestedAt);
                        });
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void DeviceService::readBackMonitorFeature(const QString& id, int code, int value, std::uint64_t generation,
                                           qint64 requestedAt, bool finalAttempt) {
    const auto queuedAt = monotonicMs();
    const auto state = state_;
    QPointer<DeviceService> self(this);
    QMetaObject::invokeMethod(
        worker_,
        [self, state, id, code, value, generation, requestedAt, queuedAt, finalAttempt] {
            if (state->monitorControlsGeneration.load() != generation)
                return;
            bool verified = false;
            QString error;
            platform::MonitorFeatureInfo actual;
            const auto startedAt = monotonicMs();
            try {
                // 只确认已通过本次写前校验的原码；不把单项数据冒充成全量能力快照。
                actual = platform::WindowsBackend{}.queryMonitorFeature(id.toStdWString(),
                                                                        static_cast<std::uint8_t>(code));
                verified = actual.code == static_cast<std::uint8_t>(code) && actual.readable &&
                           actual.current == static_cast<std::uint32_t>(value);
                if (!verified)
                    error = QString::fromStdString(actual.error);
            } catch (const std::exception& e) {
                error = QString::fromUtf8(e.what());
            }
            qCInfo(controlLog) << "monitor feature readback" << id << "code" << code << "queue_ms"
                                << startedAt - queuedAt << "readback_ms" << monotonicMs() - startedAt
                                << "total_ms" << monotonicMs() - requestedAt << "verified" << verified
                                << "actual_code" << actual.code << "readable" << actual.readable << "current"
                                << actual.current << "error" << error << "final_attempt" << finalAttempt;
            if (self)
                QMetaObject::invokeMethod(
                    self,
                    [self, id, code, value, generation, requestedAt, finalAttempt, verified, error] {
                        if (!self || self->stopping_ || generation != self->monitorControlsGeneration_)
                            return;
                        if (!verified && !finalAttempt) {
                            // 固件可能尚未更新读数；只补一次延后确认，绝不重发设置。
                            emit self->monitorFeatureProgress("尚未读到确认值，稍后再次确认…");
                            QTimer::singleShot(250, self, [self, id, code, value, generation, requestedAt] {
                                if (self && !self->stopping_ &&
                                    generation == self->monitorControlsGeneration_)
                                    self->readBackMonitorFeature(id, code, value, generation, requestedAt,
                                                                 true);
                            });
                            return;
                        }
                        self->finishMonitorFeature(id, code, generation, true, verified, error);
                    },
                    Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void DeviceService::finishMonitorFeature(const QString& id, int code, std::uint64_t generation, bool sent,
                                         bool verified, const QString& error) {
    if (generation != monitorControlsGeneration_)
        return;
    monitorFeaturePending_ = false;
    state_->monitorFeaturePending.store(false);
    monitorControlsBusy_ = false;
    if (!verified)
        invalidateMonitorCapabilities();
    if (sent) {
        // 目标确认之后才补齐其他状态；不强制执行 USB、电量、音频和亮度的整轮扫描。
        requestMonitorControls(id);
    } else {
        emit monitorControlsChanged();
    }
    QString message;
    if (!sent)
        message = "显示器功能设置失败：" + error;
    else if (verified)
        message = (code == 0xDC || code == 0x14) ? "显示器设置已回读确认；具体预设名称仍以显示器菜单为准。"
                                                 : "显示器设置已回读确认。";
    else if (code == 0x60)
        message = "输入源切换请求已发送，实际切换未确认；链路可能已中断，必要时用显示器按键切回。";
    else
        message = "显示器设置请求已发送，但回读未确认生效，请核对显示器菜单。";
    qInfo().noquote() << message;
    emit monitorFeatureFinished(message);
    emit operationFinished(message, sent);
    resumeRefresh();
}
} // namespace peripheral::app
