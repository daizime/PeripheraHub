#pragma once

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <optional>

namespace peripheral::app {

struct SavedDeviceProfile {
    QString id;
    QString name;
    QString category;
    // 音频方向仅来自系统 capture 标志；旧档案留空，不能用名称猜测。
    QString direction;
    QDateTime lastSeen;
    QMap<QString, int> settings;
    QSet<QString> confirmedSettings;
    // 连接状态来自本次扫描，不写入档案，避免重启后把历史设备当成在线设备。
    bool connected = false;
};

// 只保存已识别设备的设置，不负责发现设备、写硬件或自动恢复设置。
class DeviceProfiles final {
  public:
    explicit DeviceProfiles(QString filePath);
    bool load();
    bool save() const;
    // 仅删除离线档案；原子落盘失败时恢复内存记录，避免退出时误写删除结果。
    bool remove(const QString& id);
    void beginScan();
    // 枚举失败时仅维持上一轮会话，不伪造新的发现时间或新建设备。
    void keepConnected(const QString& id);
    void observe(const QString& id, const QString& name, const QString& category,
                 const QMap<QString, int>& initialValues = {}, const QString& direction = {});
    void finishScan();
    void remember(const QString& id, const QString& key, int value, bool confirmed = false);
    // 回读不匹配时保留保存的目标值，并撤销确认，不能悄悄用设备默认值覆盖目标。
    void confirm(const QString& id, const QString& key, int actual);
    QList<SavedDeviceProfile> profiles() const;
    std::optional<SavedDeviceProfile> find(const QString& id) const;
    QString errorString() const;

  private:
    static bool validSetting(const QString& key, int value);
    static bool validMetadata(const QString& id, const QString& name, const QString& category);
    static bool validDirection(const QString& direction);
    QString filePath_;
    QMap<QString, SavedDeviceProfile> profiles_;
    mutable QString error_;
    bool loadRejected_ = false;
};

} // namespace peripheral::app
