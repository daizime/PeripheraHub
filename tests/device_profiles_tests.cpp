#include "app/DeviceProfiles.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>

using peripheral::app::DeviceProfiles;

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
QByteArray read(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "read fixture archive");
    return file.readAll();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary(QDir::current().filePath("profile-deletion-test-XXXXXX"));
    check(temporary.isValid(), "all deletion checks use disposable profiles");
    const auto path = temporary.filePath("devices.json");
    DeviceProfiles store(path);
    check(store.load(), "new store loads");
    store.observe("monitor:a", "Same name", "monitor", {{"brightness", 37}, {"vcp_dc", 3}});
    store.observe("monitor:b", "Same name", "monitor", {{"brightness", 63}});
    store.observe("audio:a", "Headset", "audio", {{"volume", 20}}, "output");
    check(store.save(), "seed three independent profiles");
    const auto before = read(path);
    check(!store.remove("monitor:a"), "connected profile cannot be removed");
    check(store.find("monitor:a").has_value(), "blocked deletion preserves memory");
    check(read(path) == before, "blocked deletion preserves disk");
    check(!store.remove("unknown"), "missing target reports failure");
    check(read(path) == before, "missing target does not rewrite file");

    store.beginScan();
    store.observe("monitor:b", "Same name", "monitor");
    store.observe("audio:a", "Headset", "audio", {}, "output");
    store.finishScan();
    check(store.remove("monitor:a"), "disconnected target is removed atomically");
    check(!store.find("monitor:a"), "target disappears from memory");
    check(store.profiles().size() == 2, "only one profile is removed");
    check(store.find("monitor:b")->settings.value("brightness") == 63,
          "same-name different device settings remain intact");
    check(store.find("audio:a")->settings.value("volume") == 20, "another category remains intact");
    check(store.save(), "later checkpoint succeeds after removal");
    DeviceProfiles restarted(path);
    check(restarted.load(), "restart loads remaining profiles");
    check(!restarted.find("monitor:a"), "checkpoint and restart do not resurrect deletion");
    restarted.observe("monitor:a", "Newly read name", "monitor", {{"brightness", 71}});
    check(restarted.find("monitor:a")->name == "Newly read name", "reconnect reads current name");
    check(restarted.find("monitor:a")->settings.value("brightness") == 71,
          "reconnect seeds current setting instead of deleted value");
    check(!restarted.find("monitor:a")->settings.contains("vcp_dc"),
          "deleted advanced settings are not inherited");

    // 制造真实提交失败：目标路径变成目录，不能用新档案原子替换。
    restarted.beginScan();
    check(restarted.save(), "persist reconnected fixture before simulated storage failure");
    const auto saved = read(path);
    const auto backup = path + ".backup";
    check(QFile::rename(path, backup), "retain original bytes for failure fixture");
    check(QDir().mkdir(path), "block atomic replacement with a directory");
    check(!restarted.remove("monitor:a"), "failed atomic commit reports deletion failure");
    check(!restarted.errorString().isEmpty(), "commit failure has an actionable error");
    check(restarted.find("monitor:a").has_value(), "failed commit rolls back memory deletion");
    check(read(backup) == saved, "failed deletion preserves previous archive bytes");
    check(QDir().rmdir(path) && QFile::rename(backup, path), "restore fixture storage");
    check(restarted.save(), "checkpoint after failure keeps restored target");
    DeviceProfiles afterFailure(path);
    check(afterFailure.load() && afterFailure.find("monitor:a").has_value(),
          "failed deletion does not become successful on exit or restart");

    // 解析失败时，不允许删档操作覆盖原始损坏文件。
    QFile corrupt(path);
    check(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate), "create invalid fixture");
    corrupt.write("invalid-json");
    corrupt.close();
    check(!afterFailure.load(), "invalid archive is rejected");
    check(!afterFailure.remove("monitor:a"), "deletion respects rejected archive protection");
    check(afterFailure.find("monitor:a").has_value(), "rejected archive retains in-memory target");
    check(read(path) == "invalid-json", "deletion never overwrites rejected original");

    DeviceProfiles single(temporary.filePath("single.json"));
    single.observe("monitor:last", "Last display", "monitor");
    single.beginScan();
    check(single.remove("monitor:last"), "last offline profile can be removed");
    DeviceProfiles empty(temporary.filePath("single.json"));
    check(empty.load() && empty.profiles().isEmpty(), "empty archive remains valid on restart");
    std::cout << "CHECKS=" << checks << " FAILURES=" << failures << '\n';
    return failures ? 1 : 0;
}
