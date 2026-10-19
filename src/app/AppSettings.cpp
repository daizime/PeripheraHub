#include "AppSettings.h"
#include <QSettings>
#include <QVariant>
#include <array>
#include <utility>

namespace peripheral::app {
namespace {
constexpr std::array keys{"application/closeToTray", "application/restoreOnReconnect",
                          "application/refreshIntervalSeconds", "application/lowBatteryNotification",
                          "application/experimentalDeviceWrites"};
bool readBool(QSettings& settings, const char* key, bool fallback, bool& invalid) {
    if (!settings.contains(key))
        return fallback;
    const auto text = settings.value(key).toString();
    if (text == QStringLiteral("true"))
        return true;
    if (text == QStringLiteral("false"))
        return false;
    invalid = true;
    return fallback;
}
} // namespace

AppSettings::AppSettings(QString filePath) : filePath_(std::move(filePath)) {}

bool AppSettings::validRefreshInterval(int seconds) {
    return seconds == 3 || seconds == 5 || seconds == 10 || seconds == 30;
}

AppPreferences AppSettings::load(QString* warning) const {
    if (warning)
        warning->clear();
    QSettings settings(filePath_, QSettings::IniFormat);
    AppPreferences result;
    bool invalid = false;
    result.closeToTray = readBool(settings, keys[0], result.closeToTray, invalid);
    result.restoreOnReconnect = readBool(settings, keys[1], result.restoreOnReconnect, invalid);
    if (settings.contains(keys[2])) {
        bool integer = false;
        const auto seconds = settings.value(keys[2]).toString().toInt(&integer);
        if (integer && validRefreshInterval(seconds))
            result.refreshIntervalSeconds = seconds;
        else
            invalid = true;
    }
    result.lowBatteryNotification = readBool(settings, keys[3], result.lowBatteryNotification, invalid);
    result.experimentalDeviceWrites =
        readBool(settings, keys[4], result.experimentalDeviceWrites, invalid);
    if (settings.status() != QSettings::NoError) {
        result = {};
        if (warning)
            *warning = QStringLiteral("程序设置读取失败，已使用默认程序选项；原文件保留。");
    } else if (invalid && warning) {
        *warning = QStringLiteral("部分程序选项无效，已使用对应默认值；设备档案不受影响。");
    }
    return result;
}

bool AppSettings::save(const AppPreferences& preferences, QString& error) const {
    error.clear();
    if (!validRefreshInterval(preferences.refreshIntervalSeconds)) {
        error = QStringLiteral("刷新间隔必须为 3、5、10 或 30 秒。");
        return false;
    }
    QSettings settings(filePath_, QSettings::IniFormat);
    // 只改 application 下的四个键，保留设备选择、分组展开与鼠标关联。
    std::array<QVariant, keys.size()> previous;
    std::array<bool, keys.size()> present;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        present[i] = settings.contains(keys[i]);
        previous[i] = settings.value(keys[i]);
    }
    if (settings.status() != QSettings::NoError) {
        error = QStringLiteral("程序设置文件无法读取，未覆盖原文件。");
        return false;
    }
    settings.setValue(keys[0], preferences.closeToTray);
    settings.setValue(keys[1], preferences.restoreOnReconnect);
    settings.setValue(keys[2], preferences.refreshIntervalSeconds);
    settings.setValue(keys[3], preferences.lowBatteryNotification);
    settings.setValue(keys[4], preferences.experimentalDeviceWrites);
    settings.sync();
    if (settings.status() == QSettings::NoError)
        return true;
    // QSettings 同进程共享缓存；失败后撤销新值，防止其他保存把它们带回文件。
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (present[i])
            settings.setValue(keys[i], previous[i]);
        else
            settings.remove(keys[i]);
    }
    settings.sync();
    error = QStringLiteral("程序设置保存失败，请检查数据目录是否可写；当前选项保持原值。");
    return false;
}

} // namespace peripheral::app
