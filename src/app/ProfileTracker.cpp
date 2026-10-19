#include "ProfileTracker.h"

#include "DeviceProfiles.h"
#include "DeviceService.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QStringList>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace peripheral::app {
namespace {
QString text(const std::wstring& value) {
    return QString::fromStdWString(value);
}
QString text(const std::string& value) {
    return QString::fromStdString(value);
}

// Sampling order and freshness use a monotonic clock so a system time
// correction cannot freeze accepted readings or make an old sample look new.
qint64 monotonicNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct PresentDevice {
    QString id;
    QString target;
    QString name;
    QString category;
    QMap<QString, int> readings;
    QString direction;
};

bool validPercent(int value) {
    return value >= 0 && value <= 100;
}

} // namespace

ProfileTracker::ProfileTracker(DeviceService* service, DeviceProfiles* profiles, QObject* parent)
    : QObject(parent), service_(service), profiles_(profiles) {
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(500);
    connect(&saveTimer_, &QTimer::timeout, this, &ProfileTracker::save);
    checkpointTimer_.setInterval(60000);
    connect(&checkpointTimer_, &QTimer::timeout, this, &ProfileTracker::save);
    checkpointTimer_.start();
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &ProfileTracker::save);
    connect(service_, &DeviceService::settingsRestored, this,
            [this](const QString& id, bool success, const QString& message) {
                if (!restoringIds_.remove(id)) {
                    return;
                }
                // 迟到的扫描不能把恢复前的默认值记成“最后设置”。
                readAfter_[id] = monotonicNowMs();
                messages_[id] = message.isEmpty() ? (success ? QStringLiteral("保存的设置已恢复。")
                                                             : QStringLiteral("设置恢复未完成。"))
                                                  : message;
                const auto selected = text(service_->monitorControls().id);
                if (!selected.isEmpty() && id == QStringLiteral("monitor:") + selected) {
                    // 使恢复前已经排队的高级读取失效，再读取当前选中的显示器。
                    advancedReadAfter_[id] = readAfter_.value(id);
                    service_->selectMonitorControlsTarget({});
                    service_->selectMonitorControlsTarget(selected);
                    service_->requestMonitorControls(selected);
                }
                markDirty();
                emit changed();
                emit statusMessage(messages_.value(id));
            });
}

QString ProfileTracker::hidProfileId(const DeviceSnapshot& device) {
    QString id = QStringLiteral("hid:") + text(device.logicalId);
    QStringList serials;
    for (const auto& endpoint : device.endpoints) {
        if (!endpoint.serial.empty()) {
            // 同一 Container 在后续进程被复用时，仍须用 VID 和序列号分开档案。
            serials.append(QString::number(endpoint.vid, 16) + QLatin1Char(':') + text(endpoint.serial));
        }
    }
    serials.removeDuplicates();
    serials.sort();
    if (!serials.isEmpty()) {
        QByteArray identity;
        for (const auto& serial : serials) {
            const auto bytes = serial.toUtf8();
            identity += QByteArray::number(bytes.size()) + ':' + bytes;
        }
        id += QStringLiteral(":serial:") +
              QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    }
    return id;
}

void ProfileTracker::update(const std::vector<DeviceSnapshot>& devices) {
    const auto& snapshot = service_->snapshot();
    QList<PresentDevice> present;
    if (snapshot.hidEnumerationSucceeded) {
        for (const auto& device : devices) {
            if (device.endpoints.empty() ||
                (device.kind != DeviceKind::Mouse && device.kind != DeviceKind::Keyboard)) {
                continue;
            }
            const auto category =
                device.kind == DeviceKind::Mouse ? QStringLiteral("mouse") : QStringLiteral("keyboard");
            QMap<QString, int> readings;
            if (!device.stale) {
                if (device.battery.reportedPercent && validPercent(*device.battery.reportedPercent)) {
                    readings.insert(QStringLiteral("battery"), *device.battery.reportedPercent);
                }
                if (device.battery.charging) {
                    readings.insert(QStringLiteral("charging"), *device.battery.charging ? 1 : 0);
                }
            }
            if (const auto mouse = snapshot.mouseForDevice(device);
                mouse && mouse->value.profileError.empty() && mouse->profileSampledMonotonicMs > 0 &&
                snapshot.sampledMonotonicMs - mouse->profileSampledMonotonicMs <= 45000) {
                const auto& profile = *mouse->value.profile;
                readings.insert("vgn_rate_hz", profile.pollingRateHz);
                readings.insert("vgn_stage_count", static_cast<int>(profile.stages.size()));
                readings.insert("vgn_active_stage", profile.activeStage);
                readings.insert("vgn_lod_code", profile.lodCode);
                readings.insert("vgn_debounce_ms", profile.debounceMs);
                readings.insert("vgn_sleep_seconds", profile.sleepSeconds);
                readings.insert("vgn_motion_sync", profile.motionSync ? 1 : 0);
                readings.insert("vgn_angle_snap", profile.angleSnapping ? 1 : 0);
                readings.insert("vgn_ripple", profile.rippleControl ? 1 : 0);
                readings.insert("vgn_performance_code", profile.performanceCode);
                for (std::size_t index = 0; index < profile.stages.size(); ++index) {
                    const auto prefix = QString("vgn_dpi_%1_").arg(index);
                    readings.insert(prefix + "x", profile.stages[index].x);
                    readings.insert(prefix + "y", profile.stages[index].y);
                }
                for (std::size_t index = 0; index < profile.buttons.size(); ++index) {
                    const auto prefix = QString("vgn_key_%1_").arg(index);
                    readings.insert(prefix + "class", profile.buttons[index].actionClass);
                    readings.insert(prefix + "param", profile.buttons[index].parameter);
                }
            }
            present.append({hidProfileId(device),
                            {},
                            device.name.empty() ? category : text(device.name),
                            category,
                            readings});
        }
    }
    if (snapshot.audioEnumerationSucceeded) {
        for (const auto& audio : snapshot.audio) {
            if (audio.id.empty()) {
                continue;
            }
            QMap<QString, int> readings;
            if (audio.volumeReadable && std::isfinite(audio.volumePercent) && audio.volumePercent >= 0 &&
                audio.volumePercent <= 100) {
                readings.insert(QStringLiteral("volume"), static_cast<int>(std::lround(audio.volumePercent)));
                readings.insert(QStringLiteral("mute"), audio.muted ? 1 : 0);
            }
            if (const auto headset = snapshot.headsetForContainer(audio.containerId);
                headset && headset->value.status) {
                readings.insert(QStringLiteral("battery"), headset->value.status->batteryPercent);
                readings.insert(QStringLiteral("charging"),
                    headset->value.status->batteryKind == n9::BatteryKind::Charging ? 1 : 0);
                if (!audio.capture) {
                    const auto& state = *headset->value.status;
                    readings.insert("n9_eq_mode", state.eqMode);
                    readings.insert("n9_sidetone", state.sidetone ? 1 : 0);
                    readings.insert("n9_sidetone_volume", state.sidetoneVolume);
                    readings.insert("n9_surround", state.surround ? 1 : 0);
                    readings.insert("n9_spatial", state.spatialSound ? 1 : 0);
                    readings.insert("n9_rgb", state.rgb ? 1 : 0);
                    readings.insert("n9_autooff", state.autoPowerOffCode);
                    for (std::size_t index = 0; index < headset->value.peq.size(); ++index) {
                        if (const auto& point = headset->value.peq[index]) {
                            const auto prefix = QString("n9_peq_%1_").arg(index);
                            readings.insert(prefix + "hz", point->frequencyHz);
                            readings.insert(prefix + "gain10", point->gainTenthsDb);
                            readings.insert(prefix + "q10", point->qTenths);
                            readings.insert(prefix + "type", point->filterType);
                            readings.insert(prefix + "enabled", point->enabled ? 1 : 0);
                        }
                    }
                }
            }
            present.append({QStringLiteral("audio:") + text(audio.id), text(audio.id),
                            audio.name.empty() ? QStringLiteral("音频设备") : text(audio.name),
                            QStringLiteral("audio"), readings,
                            audio.capture ? QStringLiteral("input") : QStringLiteral("output")});
        }
    }
    if (snapshot.monitorEnumerationSucceeded) {
        for (const auto& monitor : snapshot.monitors) {
            if (monitor.id.empty()) {
                continue;
            }
            QMap<QString, int> readings;
            if (monitor.brightnessReadable && validPercent(monitor.brightnessPercent)) {
                readings.insert(QStringLiteral("brightness"), monitor.brightnessPercent);
            }
            present.append({QStringLiteral("monitor:") + text(monitor.id), text(monitor.id),
                            monitor.name.empty() ? QStringLiteral("显示器") : text(monitor.name),
                            QStringLiteral("monitor"), readings});
        }
    }

    const auto previous = profiles_->profiles();
    profiles_->beginScan();
    QSet<QString> currentIds;
    // 一次枚举失败不是拔除证据；保留该类别上一轮的连接会话，恢复后不重复自动写入。
    for (const auto& profile : previous) {
        const bool failed =
            (profile.category == QStringLiteral("audio") && !snapshot.audioEnumerationSucceeded) ||
            (profile.category == QStringLiteral("monitor") && !snapshot.monitorEnumerationSucceeded) ||
            ((profile.category == QStringLiteral("mouse") ||
              profile.category == QStringLiteral("keyboard")) &&
             !snapshot.hidEnumerationSucceeded);
        if (failed && connectedIds_.contains(profile.id)) {
            profiles_->keepConnected(profile.id);
            currentIds.insert(profile.id);
        }
    }
    for (const auto& device : present) {
        if (currentIds.contains(device.id)) {
            continue;
        }
        // 先取得旧目标；observe 只补缺失字段，不能先把重连默认值覆盖到旧档案。
        const auto saved = profiles_->find(device.id);
        profiles_->observe(device.id, device.name, device.category, device.readings, device.direction);
        const auto observed = profiles_->find(device.id);
        if (!observed || !observed->connected) {
            continue;
        }
        currentIds.insert(device.id);
        if (!saved || saved->name != device.name || saved->category != device.category ||
            saved->direction != observed->direction || saved->settings != observed->settings ||
            saved->confirmedSettings != observed->confirmedSettings) {
            markDirty();
        }
        const bool newSession = !connectedIds_.contains(device.id);
        if (newSession) {
            if (device.category == QStringLiteral("monitor")) {
                advancedReadAfter_[device.id] = snapshot.monitorSampledMonotonicMs > 0
                                                    ? snapshot.monitorSampledMonotonicMs
                                                    : snapshot.sampledMonotonicMs;
            }
            if (restoreOnReconnect_ && saved && (device.category == QStringLiteral("audio") ||
                          device.category == QStringLiteral("monitor"))) {
                restore(*saved, device.target, device.category == QStringLiteral("monitor"));
            }
        }
        const auto sampledMonotonicMs =
            device.category == QStringLiteral("audio")     ? snapshot.audioSampledMonotonicMs
            : device.category == QStringLiteral("monitor") ? snapshot.monitorSampledMonotonicMs
                                                           : snapshot.sampledMonotonicMs;
        acceptReadings(device.id, device.readings,
                       sampledMonotonicMs > 0 ? sampledMonotonicMs : snapshot.sampledMonotonicMs);
    }
    profiles_->finishScan();
    const bool connectionChanged = connectedIds_ != currentIds;
    connectedIds_ = std::move(currentIds);
    if (connectionChanged) {
        flush();
    }
    // 存在性或最后发现时间变化也需要让已保存设备栏目刷新；时间戳每分钟落盘。
    emit changed();
}

void ProfileTracker::restore(const SavedDeviceProfile& profile, const QString& targetId, bool monitor) {
    if (restoringIds_.contains(profile.id) || targetId.isEmpty() ||
        (monitor && targetId.startsWith(QStringLiteral("unidentified:")))) {
        return;
    }
    QMap<QString, int> values;
    const QStringList keys = monitor ? QStringList{QStringLiteral("brightness"), QStringLiteral("vcp_dc")}
                                     : QStringList{QStringLiteral("volume")};
    for (const auto& key : keys) {
        if (profile.confirmedSettings.contains(key) && profile.settings.contains(key)) {
            values.insert(key, profile.settings.value(key));
        }
    }
    if (values.isEmpty()) {
        return;
    }
    if (monitor && profile.confirmedSettings.contains(QStringLiteral("vcp_60")) &&
        profile.settings.contains(QStringLiteral("vcp_60"))) {
        // 只给恢复逻辑核对当前信号源，绝不是要切换输入源的写入目标。
        values.insert(QStringLiteral("last_input"), profile.settings.value(QStringLiteral("vcp_60")));
    }
    // 输入源、静音、色彩与对比度仅记录，不在连接时自动写入。
    restoringIds_.insert(profile.id);
    messages_[profile.id] = QStringLiteral("正在恢复上次确认的设置…");
    service_->restoreSettings(profile.id, targetId, monitor, values);
}

void ProfileTracker::acceptReadings(const QString& id, const QMap<QString, int>& values,
                                    qint64 sampledMonotonicMs) {
    if (restoringIds_.contains(id) || sampledMonotonicMs < readAfter_.value(id)) {
        return;
    }
    const auto profile = profiles_->find(id);
    if (!profile || !profile->connected) {
        return;
    }
    const QString target = id.mid(id.indexOf(QLatin1Char(':')) + 1);
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if ((it.key() == QStringLiteral("brightness") && service_->brightnessUpdatePending(target)) ||
            (it.key() == QStringLiteral("volume") && service_->volumeUpdatePending(target))) {
            continue;
        }
        if (!profile->settings.contains(it.key()) || profile->settings.value(it.key()) != it.value() ||
            !profile->confirmedSettings.contains(it.key())) {
            // 采用实际新读数，也记录其他程序/显示器按键造成的变化，不把发送成功当成确认。
            profiles_->remember(id, it.key(), it.value(), true);
            markDirty();
        }
    }
}

void ProfileTracker::updateMonitorControls() {
    if (service_->monitorControlsBusy() || !service_->monitorControlsReadAttempted()) {
        return;
    }
    const auto& controls = service_->monitorControls();
    const QString id = QStringLiteral("monitor:") + text(controls.id);
    const auto sampledMonotonicMs = controls.sampledMonotonicMs > 0 ? controls.sampledMonotonicMs
                                                                    : service_->snapshot().sampledMonotonicMs;
    if (controls.id.empty() || !connectedIds_.contains(id) || restoringIds_.contains(id) ||
        sampledMonotonicMs < advancedReadAfter_.value(id) || sampledMonotonicMs < readAfter_.value(id)) {
        return;
    }
    bool updated = false;
    for (const auto& feature : controls.features) {
        QString key;
        if (feature.code == 0x12) {
            key = QStringLiteral("vcp_12");
            if (!feature.readable || feature.current > 65535 || feature.current > feature.maximum) {
                continue;
            }
        } else if (feature.code == 0x60 || feature.code == 0xDC || feature.code == 0x14) {
            key = QStringLiteral("vcp_%1").arg(feature.code, 2, 16, QLatin1Char('0'));
            if (!feature.readable || feature.current > 65535 ||
                std::find(feature.allowedValues.begin(), feature.allowedValues.end(), feature.current) ==
                    feature.allowedValues.end()) {
                continue;
            }
        } else {
            continue;
        }
        const auto profile = profiles_->find(id);
        const int actual = static_cast<int>(feature.current);
        if (!profile || !profile->connected) {
            return;
        }
        if (!profile->settings.contains(key) || profile->settings.value(key) != actual ||
            !profile->confirmedSettings.contains(key)) {
            profiles_->remember(id, key, actual, true);
            markDirty();
            updated = true;
        }
    }
    if (updated) {
        emit changed();
    }
}

void ProfileTracker::remember(const QString& profileId, const QString& key, int value) {
    const auto previous = profiles_->find(profileId);
    if (!previous || !previous->connected) {
        return;
    }
    profiles_->remember(profileId, key, value);
    const auto saved = profiles_->find(profileId);
    if (!saved || !saved->settings.contains(key) || saved->settings.value(key) != value ||
        saved->confirmedSettings.contains(key)) {
        return;
    }
    readAfter_[profileId] = std::max(monotonicNowMs(), service_->snapshot().sampledMonotonicMs + 1);
    markDirty();
    emit changed();
}

QString ProfileTracker::messageFor(const QString& id) const {
    return messages_.value(id);
}

QString ProfileTracker::removalBlockReason(const QString& id) const {
    const auto profile = profiles_->find(id);
    if (!profile) {
        return QStringLiteral("请选择需要删除的设备档案。");
    }
    if (profile->connected || connectedIds_.contains(id)) {
        return QStringLiteral("请先结束使用并断开设备连接，再删除档案；2.4G 设备需拔出对应接收器。");
    }
    if (restoringIds_.contains(id)) {
        return QStringLiteral("正在结束该设备的设置恢复，请稍后刷新再删除。");
    }
    const auto& snapshot = service_->snapshot();
    const bool enumerated =
        profile->category == QStringLiteral("monitor") ? snapshot.monitorEnumerationSucceeded
        : profile->category == QStringLiteral("audio") ? snapshot.audioEnumerationSucceeded
        : (profile->category == QStringLiteral("mouse") || profile->category == QStringLiteral("keyboard"))
            ? snapshot.hidEnumerationSucceeded
            : snapshot.hidEnumerationSucceeded && snapshot.audioEnumerationSucceeded &&
                  snapshot.monitorEnumerationSucceeded;
    if (!enumerated) {
        return QStringLiteral("尚未确认设备已断开，请刷新状态成功后再删除。");
    }
    return {};
}

bool ProfileTracker::removeProfile(const QString& id, QString& error) {
    error = removalBlockReason(id);
    if (!error.isEmpty()) {
        return false;
    }
    if (!profiles_->remove(id)) {
        error = profiles_->errorString();
        return false;
    }
    // 清理会话信息；迟到的回读找不到档案，不会再次创建已删除记录。
    connectedIds_.remove(id);
    restoringIds_.remove(id);
    readAfter_.remove(id);
    advancedReadAfter_.remove(id);
    messages_.remove(id);
    emit changed();
    return true;
}

void ProfileTracker::flush() {
    saveTimer_.stop();
    save();
}

void ProfileTracker::markDirty() {
    saveTimer_.start();
}

void ProfileTracker::save() {
    if (!profiles_->save()) {
        const auto error = profiles_->errorString();
        if (lastSaveError_ != error) {
            lastSaveError_ = error;
            emit statusMessage(error);
        }
    } else {
        lastSaveError_.clear();
    }
}

} // namespace peripheral::app
