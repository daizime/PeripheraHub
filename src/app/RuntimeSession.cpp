#include "RuntimeSession.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>

namespace peripheral::app {

RuntimeSession::Result RuntimeSession::acquire(const QString& directory) {
    if (lock_ && lock_->isLocked()) {
        error_ = QStringLiteral("当前会话已持有数据目录，不能重复初始化。");
        return Result::Unavailable;
    }
    directory_.clear();
    error_.clear();
    lock_.reset();
    const auto path = QDir(directory).absolutePath();
    if (directory.trimmed().isEmpty() || !QDir().mkpath(path)) {
        error_ = QStringLiteral("无法创建数据目录：%1").arg(path);
        return Result::Unavailable;
    }
    // Resolve aliases before locking, so /data and /data/../data share a lock.
    directory_ = QFileInfo(path).canonicalFilePath();
    if (directory_.isEmpty()) {
        error_ = QStringLiteral("无法解析数据目录：%1").arg(path);
        return Result::Unavailable;
    }
    lock_ = std::make_unique<QLockFile>(directory_ + QStringLiteral("/session.lock"));
    // This lock is held for the entire run. Age alone must never invalidate it.
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) {
        if (lock_->error() == QLockFile::LockFailedError) {
            error_ = QStringLiteral("外设中心已在使用该数据目录，请从任务栏或系统托盘打开。\n%1")
                         .arg(directory_);
            return Result::AlreadyRunning;
        }
        error_ = QStringLiteral("数据目录无法加锁，可能没有写入权限或磁盘空间不足：%1").arg(directory_);
        return Result::Unavailable;
    }
    // Test actual file creation, rather than relying on Windows permission bits.
    QTemporaryFile probe(directory_ + QStringLiteral("/.write-check-XXXXXX"));
    if (!probe.open() || probe.write("ok", 2) != 2 || !probe.flush()) {
        error_ = QStringLiteral("数据目录不可写：%1\n%2").arg(directory_, probe.errorString());
        lock_->unlock();
        return Result::Unavailable;
    }
    return Result::Ready;
}

} // namespace peripheral::app
