#include "app/RuntimeSession.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>

using peripheral::app::RuntimeSession;
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    // Keep the fixture beside this disposable test executable. The system temp
    // directory may have a different ACL or be outside a restricted build sandbox.
    QTemporaryDir fixture(QCoreApplication::applicationDirPath() +
                          QStringLiteral("/runtime-session-XXXXXX"));
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) {
            ++failures;
            std::cerr << message << '\n';
        }
    };
    check(fixture.isValid(), "isolated directory exists");
    const auto directory = fixture.filePath("data");
    {
        RuntimeSession owner, contender, separate;
        const auto firstResult = owner.acquire(directory);
        check(firstResult == RuntimeSession::Result::Ready, "first owner acquires lock");
        if (firstResult != RuntimeSession::Result::Ready)
            std::cerr << owner.error().toStdString() << '\n';
        QFile data(directory + "/devices.json");
        check(data.open(QIODevice::WriteOnly), "owner can write a profile fixture");
        data.write("existing-device-settings");
        data.close();
        check(contender.acquire(directory + "/../data/.") == RuntimeSession::Result::AlreadyRunning,
              "path aliases cannot start a second writer");
        check(!contender.error().isEmpty(), "duplicate startup explains the failure");
        check(separate.acquire(fixture.filePath("other")) == RuntimeSession::Result::Ready,
              "isolated data directories have independent sessions");
        check(owner.acquire(directory) == RuntimeSession::Result::Unavailable,
              "reinitialization cannot release an active lock");
        check(data.open(QIODevice::ReadOnly) && data.readAll() == "existing-device-settings",
              "rejected startup does not modify saved profiles");
        data.close();
        const auto files = QDir(directory).entryList(QDir::Files | QDir::Hidden);
        check(files.size() == 2, "write probe leaves no files behind");
    }
    RuntimeSession next, invalid;
    check(next.acquire(directory) == RuntimeSession::Result::Ready, "normal exit releases lock");
    check(invalid.acquire(directory + "/devices.json/child") == RuntimeSession::Result::Unavailable,
          "a file cannot be used as a data directory");
    check(!invalid.error().isEmpty(), "unavailable directory produces an actionable error");
    check(invalid.acquire({}) == RuntimeSession::Result::Unavailable, "empty path is rejected");
    return failures == 0 ? 0 : 1;
}
