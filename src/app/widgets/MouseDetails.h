#pragma once
#include "core/vgn_config.h"
#include <QWidget>
#include <optional>
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace peripheral::app {
// Pure presentation: no enumeration, persistence or HID transactions. The write
// rows only emit what the user asked for; the window decides whether the
// experimental write switch allows the call.
class MouseDetails final : public QWidget {
    Q_OBJECT
  public:
    explicit MouseDetails(QWidget* parent = nullptr);
    // sampledMs is the wall-clock sample time for display; stale is decided by
    // the caller from monotonic freshness so clock corrections cannot refresh it.
    void setInfo(const vgn::Profile* profile, qint64 sampledMs = 0, bool stale = false,
                 const QString& error = {});
    // 实验性写入开关关闭时，写入区完全不出现。
    void setWritable(bool writable);
    static QString dpiText(const vgn::DpiStage& stage);

  signals:
    void dpiWriteRequested(int dpi);
    void rateWriteRequested(int hertz);
    void stageWriteRequested(int stage);

  private:
    void updateEditor();
    void refreshStateText();
    QString sampleTime_;
    qint64 sampledMs_ = 0;
    bool stale_ = false;
    // 上一次从设备读到的 DPI；用于判断输入框是否被用户改过。
    int deviceDpi_ = 0;
    QLabel *state_, *stages_, *buttons_, *performance_;
    QWidget* editor_ = nullptr;
    QSpinBox* dpiInput_ = nullptr;
    QComboBox* rateInput_ = nullptr;
    QComboBox* stageInput_ = nullptr;
    QPushButton *dpiApply_ = nullptr, *rateApply_ = nullptr, *stageApply_ = nullptr;
    // 保存配置副本，避免指向服务快照的裸指针在刷新后失效。
    std::optional<vgn::Profile> profileCopy_;
    const vgn::Profile* profile_ = nullptr;
    bool writable_ = false;
};
} // namespace peripheral::app
