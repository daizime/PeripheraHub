#pragma once

#include <QLockFile>
#include <QString>
#include <memory>

namespace peripheral::app {

// One owner per canonical data directory. Acquire before constructing settings,
// log files or device services; keep alive until they have been destroyed.
class RuntimeSession final {
  public:
    enum class Result { Ready, AlreadyRunning, Unavailable };
    Result acquire(const QString& directory);
    const QString& directory() const { return directory_; }
    const QString& error() const { return error_; }

  private:
    QString directory_, error_;
    std::unique_ptr<QLockFile> lock_;
};

} // namespace peripheral::app
