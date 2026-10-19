#include "app/AppSettings.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <iostream>

using namespace peripheral::app;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}
QByteArray read(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "fixture file can be read");
    return file.readAll();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory(QDir::current().filePath("app-settings-test-XXXXXX"));
    check(directory.isValid(), "preferences use a disposable workspace directory");
    const auto path = directory.filePath("settings.ini");
    AppSettings settings(path);
    QString error;
    check(settings.load(&error) == AppPreferences{} && error.isEmpty(), "missing file retains old defaults");
    check(!QFile::exists(path), "loading defaults does not create a file");
    {
        QSettings legacy(path, QSettings::IniFormat);
        legacy.setValue("mouse/boundEndpoints", QStringList{"receiver-a", "wired-a"});
        legacy.setValue("monitor/selected", "monitor-a");
        legacy.setValue("saved/groups/audio/input", false);
        legacy.sync();
        check(legacy.status() == QSettings::NoError, "legacy settings seeded");
    }
    check(settings.load(&error) == AppPreferences{} && error.isEmpty(), "legacy file needs no migration");
    const auto profilePath = directory.filePath("devices.json");
    QFile profiles(profilePath);
    check(profiles.open(QIODevice::WriteOnly), "device archive fixture created");
    profiles.write("separate-device-archive");
    profiles.close();
    const AppPreferences changed{false, false, 10, false};
    check(settings.save(changed, error) && error.isEmpty(), "save explicit program preferences");
    check(AppSettings(path).load(&error) == changed, "preferences survive an independent reload");
    {
        QSettings retained(path, QSettings::IniFormat);
        check(retained.value("mouse/boundEndpoints").toStringList() == QStringList{"receiver-a", "wired-a"},
              "saving preferences preserves mouse connection association");
        check(retained.value("monitor/selected").toString() == "monitor-a", "device selection remains intact");
        check(!retained.value("saved/groups/audio/input").toBool(), "tree expansion remains intact");
    }
    check(read(profilePath) == "separate-device-archive", "saving preferences never modifies device archive");
    const auto before = read(path);
    for (const int invalid : {-1, 0, 1, 4, 60, 1000000}) {
        auto candidate = changed;
        candidate.refreshIntervalSeconds = invalid;
        check(!settings.save(candidate, error) && !error.isEmpty(), "unsupported poll interval is rejected");
        check(read(path) == before, "rejected preference leaves file unchanged");
    }
    check(settings.save({}, error), "restore default program preferences");
    check(settings.load() == AppPreferences{}, "program defaults reloaded");
    check(read(profilePath) == "separate-device-archive", "restoring defaults preserves device archive");
    {
        QSettings invalid(path, QSettings::IniFormat);
        invalid.setValue("application/closeToTray", "maybe");
        invalid.setValue("application/restoreOnReconnect", "false");
        invalid.setValue("application/refreshIntervalSeconds", 0);
        invalid.setValue("application/lowBatteryNotification", "sometimes");
        invalid.sync();
    }
    const auto repaired = settings.load(&error);
    check(repaired.closeToTray && !repaired.restoreOnReconnect && repaired.refreshIntervalSeconds == 3 &&
              repaired.lowBatteryNotification,
          "invalid options fall back independently without losing valid choices");
    check(!error.isEmpty(), "invalid persisted options produce a visible warning");
    check(settings.save(changed, error), "explicit valid choice repairs invalid preferences");

    // 实验性写入默认关闭，且必须显式开启后才被记住；非法值按关闭处理。
    check(!AppPreferences{}.experimentalDeviceWrites, "experimental device writes default to off");
    {
        auto enabled = changed;
        enabled.experimentalDeviceWrites = true;
        check(settings.save(enabled, error) && AppSettings(path).load(&error).experimentalDeviceWrites,
              "explicitly enabled experimental writes survive a reload");
        QSettings raw(path, QSettings::IniFormat);
        raw.setValue("application/experimentalDeviceWrites", "yes");
        raw.sync();
        const auto repairedWrites = settings.load(&error);
        check(!repairedWrites.experimentalDeviceWrites && !error.isEmpty(),
              "unknown experimental-write value falls back to off with a warning");
        check(settings.save(changed, error) && !AppSettings(path).load().experimentalDeviceWrites,
              "restoring defaults switches experimental writes off again");
    }

    // 让 settings.ini 的父路径变成普通文件，真实触发写盘失败。
    const auto parent = directory.filePath("blocked");
    QFile blocker(parent);
    check(blocker.open(QIODevice::WriteOnly), "create non-directory parent for failed save");
    blocker.write("keep-original");
    blocker.close();
    AppSettings blocked(parent + "/settings.ini");
    check(!blocked.save(changed, error) && !error.isEmpty(), "unwritable settings path reports failure");
    check(read(parent) == "keep-original", "failed save does not overwrite blocking file");
    check(QFile::remove(parent) && QDir().mkdir(parent), "restore writable fixture location");
    check(blocked.load() == AppPreferences{}, "failed save does not leak pending values through Qt cache");
    check(blocked.save({}, error), "save can succeed after storage recovers");
    std::cout << "CHECKS=" << checks << " FAILURES=" << failures << '\n';
    return failures ? 1 : 0;
}
