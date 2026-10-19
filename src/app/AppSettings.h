#pragma once
#include <QString>

namespace peripheral::app {

// 程序偏好与设备参数分开；默认值保持旧版行为。
struct AppPreferences {
    bool closeToTray = true;
    bool restoreOnReconnect = true;
    int refreshIntervalSeconds = 3;
    // 外设电量低于 10% 时通过系统通知提醒；默认开启。
    bool lowBatteryNotification = true;
    // 实验性设备写入：默认关闭。只有用户显式开启后，界面才允许对已验证型号
    // 发送私有写入；关闭时任何写入口都不出现。
    bool experimentalDeviceWrites = false;
    bool operator==(const AppPreferences&) const = default;
};

class AppSettings final {
  public:
    explicit AppSettings(QString filePath);
    AppPreferences load(QString* warning = nullptr) const;
    bool save(const AppPreferences& preferences, QString& error) const;
    static bool validRefreshInterval(int seconds);

  private:
    QString filePath_;
};

} // namespace peripheral::app
