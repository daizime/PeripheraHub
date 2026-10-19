#include "app/DeviceProfiles.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <iostream>

using peripheral::app::DeviceProfiles;

namespace {
int checks = 0;
int failures = 0;

void check(bool passed, const char* description) {
    ++checks;
    if (!passed) {
        ++failures;
        std::cerr << "FAILED: " << description << '\n';
    }
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "test file opens");
    check(file.write(bytes) == bytes.size(), "test file writes all bytes");
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "test file reads");
    return file.readAll();
}

QJsonObject entry(const QString& id = QStringLiteral("id")) {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), QStringLiteral("Same model")},
            {QStringLiteral("category"), QStringLiteral("monitor")},
            {QStringLiteral("lastSeen"), QStringLiteral("2026-10-18T05:20:30.123Z")},
            {QStringLiteral("settings"), QJsonObject{}},
            {QStringLiteral("confirmedSettings"), QJsonArray{}}};
}

QByteArray json(const QJsonArray& entries, int schema = 1) {
    return QJsonDocument(
               QJsonObject{{QStringLiteral("schema"), schema}, {QStringLiteral("profiles"), entries}})
        .toJson();
}

void directionChecks(const QString& path) {
    auto legacy = entry(QStringLiteral("audio-direction"));
    legacy.insert(QStringLiteral("category"), QStringLiteral("audio"));
    writeFile(path, json(QJsonArray{legacy}));
    DeviceProfiles store(path);
    check(store.load(), "schema 1 without optional direction remains compatible");
    check(store.find(QStringLiteral("audio-direction"))->direction.isEmpty(),
          "legacy audio direction remains unknown instead of guessed");
    store.observe(QStringLiteral("audio-direction"), QStringLiteral("Audio"), QStringLiteral("audio"), {},
                  QStringLiteral("input"));
    check(store.find(QStringLiteral("audio-direction"))->direction == QStringLiteral("input"),
          "re-observation upgrades legacy direction from reliable metadata");
    store.observe(QStringLiteral("audio-direction"), QStringLiteral("Audio"), QStringLiteral("audio"));
    check(store.find(QStringLiteral("audio-direction"))->direction == QStringLiteral("input"),
          "empty observation does not erase known audio direction");
    store.observe(QStringLiteral("audio-direction"), QStringLiteral("Audio"), QStringLiteral("audio"), {},
                  QStringLiteral("INPUT"));
    check(store.find(QStringLiteral("audio-direction"))->direction == QStringLiteral("input"),
          "unsupported direction string is ignored during observation");
    store.observe(QStringLiteral("audio-output"), QStringLiteral("Audio"), QStringLiteral("audio"), {},
                  QStringLiteral("output"));
    check(store.save(), "audio direction saves with schema 1");
    DeviceProfiles loaded(path);
    check(loaded.load() &&
              loaded.find(QStringLiteral("audio-direction"))->direction == QStringLiteral("input") &&
              loaded.find(QStringLiteral("audio-output"))->direction == QStringLiteral("output"),
          "input and output directions round-trip independently");
    auto empty = legacy;
    empty.insert(QStringLiteral("direction"), QString());
    writeFile(path, json(QJsonArray{empty}));
    check(loaded.load() && loaded.find(QStringLiteral("audio-direction"))->direction.isEmpty(),
          "explicit empty JSON direction is accepted as unknown");
    for (const auto& invalid : {QJsonValue(QStringLiteral("capture")), QJsonValue(QStringLiteral("Input")),
                                QJsonValue(1), QJsonValue(QJsonValue::Null), QJsonValue(QJsonArray{})}) {
        auto bad = legacy;
        bad.insert(QStringLiteral("direction"), invalid);
        const auto bytes = json(QJsonArray{bad});
        writeFile(path, bytes);
        DeviceProfiles malformed(path);
        check(!malformed.load() && !malformed.save(), "invalid audio direction rejects and blocks overwrite");
        check(readFile(path) == bytes, "invalid audio direction preserves original file");
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary(QCoreApplication::applicationDirPath() +
                            QStringLiteral("/profile-checks-XXXXXX"));
    check(temporary.isValid(), "temporary directory available");
    directionChecks(temporary.filePath(QStringLiteral("direction.json")));
    const auto path = temporary.filePath(QStringLiteral("nested/devices.json"));
    DeviceProfiles store(path);
    check(store.load(), "missing file is an empty valid store");
    check(store.profiles().isEmpty(), "empty on first start");
    check(!store.find(QStringLiteral("unknown")), "unknown device has no profile");
    store.remember(QStringLiteral("unknown"), QStringLiteral("volume"), 0);
    check(store.profiles().isEmpty(), "remember cannot create unknown device");
    store.beginScan();
    store.observe(QStringLiteral("display/a/b"), QStringLiteral("Same model"), QStringLiteral("monitor"),
                  {{QStringLiteral("brightness"), 47}, {QStringLiteral("unsupported"), 22}});
    store.observe(QStringLiteral("display/a/c"), QStringLiteral("Same model"), QStringLiteral("monitor"));
    store.finishScan();
    check(store.profiles().size() == 2, "same-name devices remain separate by stable ID");
    auto first = store.find(QStringLiteral("display/a/b"));
    check(first && first->connected && first->lastSeen.isValid(),
          "observed device is connected with timestamp");
    check(first->settings.value(QStringLiteral("brightness")) == 47, "initial readable setting is seeded");
    check(first->confirmedSettings.contains(QStringLiteral("brightness")), "initial read is confirmed");
    check(!first->settings.contains(QStringLiteral("unsupported")), "unknown setting key is ignored");
    check(!store.find(QStringLiteral("display/a/c"))->settings.contains(QStringLiteral("brightness")),
          "missing reading remains absent instead of zero");
    store.remember(first->id, QStringLiteral("brightness"), 62);
    first = store.find(first->id);
    check(first->settings.value(QStringLiteral("brightness")) == 62, "user target is saved");
    check(!first->confirmedSettings.contains(QStringLiteral("brightness")), "pending target is unconfirmed");
    store.confirm(first->id, QStringLiteral("brightness"), 60);
    first = store.find(first->id);
    check(first->settings.value(QStringLiteral("brightness")) == 62,
          "mismatched read does not overwrite target");
    check(!first->confirmedSettings.contains(QStringLiteral("brightness")),
          "mismatched read stays unconfirmed");
    store.confirm(first->id, QStringLiteral("brightness"), 62);
    check(store.find(first->id)->confirmedSettings.contains(QStringLiteral("brightness")),
          "matching read confirms target");
    store.confirm(first->id, QStringLiteral("brightness"), 61);
    check(!store.find(first->id)->confirmedSettings.contains(QStringLiteral("brightness")),
          "later mismatch revokes confirmation");
    store.remember(first->id, QStringLiteral("volume"), 0, true);
    store.remember(first->id, QStringLiteral("mute"), 1, true);
    store.remember(first->id, QStringLiteral("vcp_12"), 65535, true);
    store.remember(first->id, QStringLiteral("vcp_60"), 15);
    store.remember(first->id, QStringLiteral("vcp_dc"), 3);
    store.remember(first->id, QStringLiteral("vcp_14"), 5);
    store.remember(first->id, QStringLiteral("volume"), 101);
    store.remember(first->id, QStringLiteral("mute"), 2);
    store.remember(first->id, QStringLiteral("vcp_12"), 65536);
    store.remember(first->id, QStringLiteral("custom"), 1);
    first = store.find(first->id);
    check(first->settings.size() == 7, "only seven allowed keys are stored");
    check(first->settings.value(QStringLiteral("volume")) == 0, "known zero volume remains valid");
    check(first->settings.value(QStringLiteral("mute")) == 1, "invalid mute is rejected");
    check(first->settings.value(QStringLiteral("vcp_12")) == 65535, "16-bit range is enforced");
    check(store.save(), "atomic save succeeds and creates parent directory");
    check(!readFile(path).contains("connected"), "connection state is never persisted");
    DeviceProfiles reloaded(path);
    check(reloaded.load(), "saved store round-trips");
    check(reloaded.profiles().size() == 2, "all profiles reload");
    auto loaded = reloaded.find(first->id);
    check(loaded && !loaded->connected, "saved device is disconnected until observed this session");
    check(loaded->settings == first->settings && loaded->confirmedSettings == first->confirmedSettings,
          "targets and confirmation flags round-trip");
    reloaded.remember(first->id, QStringLiteral("brightness"), 99);
    check(reloaded.find(first->id)->settings.value(QStringLiteral("brightness")) == 62,
          "cannot change a disconnected historical device");
    reloaded.observe(first->id, QStringLiteral("Updated model name"), QStringLiteral("monitor"),
                     {{QStringLiteral("brightness"), 25}});
    loaded = reloaded.find(first->id);
    check(loaded->settings.value(QStringLiteral("brightness")) == 62,
          "reconnect never overwrites saved target");
    check(loaded->name == QStringLiteral("Updated model name"), "observations update device metadata");
    reloaded.beginScan();
    reloaded.observe(QStringLiteral("display/a/c"), QStringLiteral("Same model"), QStringLiteral("monitor"));
    reloaded.finishScan();
    check(!reloaded.find(first->id)->connected, "missing device becomes disconnected");
    check(reloaded.find(QStringLiteral("display/a/c"))->connected, "present device remains connected");
    reloaded.observe(QString(), QStringLiteral("Bad"), QStringLiteral("monitor"));
    reloaded.observe(QStringLiteral("long"), QString(1025, QLatin1Char('a')), QStringLiteral("monitor"));
    check(reloaded.profiles().size() == 2, "invalid metadata cannot create profiles");

    const auto badPath = temporary.filePath(QStringLiteral("invalid.json"));
    DeviceProfiles bad(badPath);
    bad.observe(QStringLiteral("memory"), QStringLiteral("Memory"), QStringLiteral("monitor"));
    const auto malformed = QByteArray("{incomplete");
    writeFile(badPath, malformed);
    check(!bad.load() && !bad.errorString().isEmpty(), "malformed JSON rejects with diagnostic");
    check(bad.find(QStringLiteral("memory")).has_value(), "bad load preserves existing memory");
    check(!bad.save(), "bad load prevents overwriting original file");
    check(readFile(badPath) == malformed, "failed load and save preserve exact original bytes");
    writeFile(badPath, json(QJsonArray{entry()}));
    check(bad.load() && bad.save(), "valid repaired file can be loaded and saved again");

    const auto rejects = [&](const QByteArray& bytes, const char* name) {
        writeFile(badPath, bytes);
        DeviceProfiles invalid(badPath);
        check(!invalid.load(), name);
        check(!invalid.save(), "invalid data cannot be silently replaced");
        check(readFile(badPath) == bytes, "invalid file is preserved");
    };
    rejects(json(QJsonArray{}, 2), "unknown schema rejects");
    rejects(json(QJsonArray{entry(), entry()}), "duplicate ID rejects");
    auto mutated = entry();
    mutated.insert(QStringLiteral("lastSeen"), QStringLiteral("not-a-date"));
    rejects(json(QJsonArray{mutated}), "invalid timestamp rejects");
    mutated = entry();
    mutated.insert(QStringLiteral("settings"), QJsonObject{{QStringLiteral("volume"), 2.5}});
    rejects(json(QJsonArray{mutated}), "fractional setting rejects");
    mutated.insert(QStringLiteral("settings"), QJsonObject{{QStringLiteral("volume"), -1}});
    rejects(json(QJsonArray{mutated}), "negative setting rejects");
    mutated.insert(QStringLiteral("settings"), QJsonObject{{QStringLiteral("volume"), 101}});
    rejects(json(QJsonArray{mutated}), "percentage above 100 rejects");
    mutated.insert(QStringLiteral("settings"), QJsonObject{{QStringLiteral("mute"), 2}});
    rejects(json(QJsonArray{mutated}), "non-boolean mute rejects");
    mutated.insert(QStringLiteral("settings"), QJsonObject{{QStringLiteral("vcp_12"), 65536}});
    rejects(json(QJsonArray{mutated}), "VCP above 16-bit rejects");
    mutated.insert(QStringLiteral("settings"), QJsonObject{{QStringLiteral("unsupported"), 12}});
    rejects(json(QJsonArray{mutated}), "unknown setting key rejects");
    mutated = entry();
    mutated.insert(QStringLiteral("confirmedSettings"), QJsonArray{QStringLiteral("brightness")});
    rejects(json(QJsonArray{mutated}), "confirmation without target rejects");
    mutated.insert(QStringLiteral("settings"), QJsonObject{{QStringLiteral("brightness"), 50}});
    mutated.insert(QStringLiteral("confirmedSettings"),
                   QJsonArray{QStringLiteral("brightness"), QStringLiteral("brightness")});
    rejects(json(QJsonArray{mutated}), "duplicate confirmation rejects");
    rejects(QByteArray(1024 * 1024 + 1, ' '), "file above 1 MiB rejects");
    QJsonArray tooMany;
    for (int i = 0; i < 513; ++i) {
        tooMany.append(entry(QString::number(i)));
    }
    rejects(json(tooMany), "more than 512 profiles rejects");
    DeviceProfiles limited(temporary.filePath(QStringLiteral("limited.json")));
    for (int i = 0; i < 513; ++i) {
        limited.observe(QString::number(i), QStringLiteral("Same model"), QStringLiteral("monitor"));
    }
    check(limited.profiles().size() == 512, "observation count is capped at 512");
    check(!limited.errorString().isEmpty(), "observation limit produces diagnostic");
    DeviceProfiles large(temporary.filePath(QStringLiteral("large.json")));
    for (int i = 0; i < 512; ++i) {
        large.observe(QString(3000, QLatin1Char('x')) + QString::number(i), QStringLiteral("Name"),
                      QStringLiteral("monitor"));
    }
    check(!large.save(), "oversized serialized output is never written");
    check(!QFile::exists(temporary.filePath(QStringLiteral("large.json"))),
          "oversized save leaves no target");
    DeviceProfiles cannotWrite(temporary.filePath(QStringLiteral("missing-parent/file")));
    writeFile(temporary.filePath(QStringLiteral("missing-parent")), QByteArray("existing file"));
    check(!cannotWrite.save() && !cannotWrite.errorString().isEmpty(), "directory failure reports error");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
