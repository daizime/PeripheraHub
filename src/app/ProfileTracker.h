#pragma once

#include "core/device_model.h"
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <vector>

namespace peripheral::app {
class DeviceProfiles;
class DeviceService;
struct SavedDeviceProfile;

// 管理档案生命周期、实际读数与安全恢复；界面只提交用户意图并展示结果。
class ProfileTracker final : public QObject {
    Q_OBJECT
  public:
    ProfileTracker(DeviceService* service, DeviceProfiles* profiles, QObject* parent = nullptr);
    void update(const std::vector<DeviceSnapshot>& devices);
    void updateMonitorControls();
    void remember(const QString& profileId, const QString& key, int value);
    void flush();
    // 只影响后续连接会话，不在切换开关时向当前设备写入。
    void setRestoreOnReconnect(bool enabled) { restoreOnReconnect_ = enabled; }
    QString messageFor(const QString& id) const;
    // 空字符串表示可删除；调用方在提交前需完成一次带票号的新扫描。
    QString removalBlockReason(const QString& id) const;
    bool removeProfile(const QString& id, QString& error);
    static QString hidProfileId(const DeviceSnapshot& device);

  signals:
    void changed();
    void statusMessage(const QString& message);

  private:
    void acceptReadings(const QString& id, const QMap<QString, int>& values, qint64 sampledMonotonicMs);
    void restore(const SavedDeviceProfile& profile, const QString& targetId, bool monitor);
    void markDirty();
    void save();
    DeviceService* service_;
    DeviceProfiles* profiles_;
    QTimer saveTimer_;
    QTimer checkpointTimer_;
    QSet<QString> connectedIds_;
    QSet<QString> restoringIds_;
    QMap<QString, qint64> readAfter_;
    QMap<QString, QString> messages_;
    QMap<QString, qint64> advancedReadAfter_;
    QString lastSaveError_;
    bool restoreOnReconnect_ = true;
};

} // namespace peripheral::app
