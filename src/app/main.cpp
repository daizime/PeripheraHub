#include "DeviceService.h"
#include "MainWindow.h"
#include "RuntimeSession.h"
#include "widgets/UiTheme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QMessageBox>
#include <QTimer>

namespace {
QString logPath;
QMutex logMutex;
void logMessage(QtMsgType type, const QMessageLogContext&, const QString& message) {
    QMutexLocker lock(&logMutex);
    QFile file(logPath);
    if (file.size() > 2 * 1024 * 1024) {
        QFile::remove(logPath + ".previous");
        file.rename(logPath + ".previous");
        file.setFileName(logPath);
    }
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + " [" +
                    QString::number(type) + "] " + message + "\n")
                       .toUtf8());
    }
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName("PeripheralHub");
    application.setApplicationVersion(QStringLiteral(PERIPHERAL_APP_VERSION));
    application.setOrganizationName("PeripheralHub");
    application.setWindowIcon(peripheral::app::applicationIcon());
    application.setFont(QFont("Microsoft YaHei UI", 10));
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription("Windows 外设中心 · 0.0 初始预览版");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(
        {"data-dir", "本地数据目录", "directory", QCoreApplication::applicationDirPath() + "/data"});
    parser.addOption({"passive", "仅枚举设备，不发送厂商协议查询"});
    parser.addOption({"quit-after", "测试用：若干毫秒后退出", "milliseconds"});
    parser.addOption({"snapshot", "测试用：退出前保存窗口图像", "path"});
    parser.addOption({"taskbar-snapshot", "测试用：退出前保存任务栏简略概览", "path"});
    parser.addOption({"state-output", "退出前保存版本化状态快照", "path"});
    parser.process(application);
    peripheral::app::RuntimeSession session;
    const auto sessionResult = session.acquire(parser.value("data-dir"));
    if (sessionResult != peripheral::app::RuntimeSession::Result::Ready) {
        // Automated smoke runs return an error without leaving a modal window.
        // No second settings/log/device writer has been constructed yet.
        if (!parser.isSet("quit-after"))
            QMessageBox::information(nullptr, QStringLiteral("外设中心"), session.error());
        return sessionResult == peripheral::app::RuntimeSession::Result::AlreadyRunning ? 4 : 2;
    }
    const auto dataDirectory = session.directory();
    logPath = dataDirectory + "/peripheral-hub.log";
    qInstallMessageHandler(logMessage);
    peripheral::app::DeviceService service(parser.isSet("passive"));
    peripheral::app::MainWindow window(&service, dataDirectory);
    window.show();
    service.start();
    if (parser.isSet("quit-after")) {
        bool ok = false;
        const int timeout = parser.value("quit-after").toInt(&ok);
        if (!ok || timeout < 100)
            return 2;
        QTimer::singleShot(timeout, &application, [&] {
            bool success = true;
            if (parser.isSet("snapshot"))
                success = window.grab().save(parser.value("snapshot")) && success;
            if (parser.isSet("taskbar-snapshot"))
                success = window.taskbarPreviewImage(QSize(360, 220)).save(parser.value("taskbar-snapshot")) && success;
            if (parser.isSet("state-output"))
                success = window.saveState(parser.value("state-output")) && success;
            application.exit(success ? 0 : 3);
        });
    }
    const int result = application.exec();
    qInstallMessageHandler(nullptr);
    return result;
}
