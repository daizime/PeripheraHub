#pragma once

#include "platform/WindowsBackend.h"

#include <QWidget>
#include <array>
#include <cstddef>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace peripheral::app {

// 显示设备实际回报的控制项；目标选择、协议校验和硬件访问由调用方负责。
class MonitorControls final : public QWidget {
    Q_OBJECT

  public:
    explicit MonitorControls(QWidget* parent = nullptr);

    void setInfo(const platform::MonitorControlsInfo& info);
    // 同一个亮度控件在常用页和内置功能页之间移动，保留拖动及目标状态。
    void setBrightnessControl(QWidget* control);
    void setBusy(bool busy);
    void setStatus(const QString& status);
    // 操作反馈保留到下次明确操作或目标变化，不被同一目标的后台补读覆盖。
    void setOperationStatus(const QString& status, bool finished = false);
    void clear();

  signals:
    void refreshRequested();
    void valueRequested(int code, int value);

  private:
    struct Row {
        int code = 0;
        QLabel* state = nullptr;
        QSpinBox* number = nullptr;
        QComboBox* options = nullptr;
        QPushButton* apply = nullptr;
        QPushButton* decrease = nullptr;
        QPushButton* increase = nullptr;
        platform::MonitorFeatureInfo feature;
        bool available = false;
        bool mappingConfirmed = false;
    };

    void updateRow(Row& row, const platform::MonitorFeatureInfo* feature);
    void updateEnabledState();
    void applyRow(std::size_t index);
    void clearOperationStatus();
    void showStatus();

    std::array<Row, 4> rows_;
    QLabel* status_ = nullptr;
    QPushButton* refresh_ = nullptr;
    platform::MonitorControlsInfo info_;
    bool busy_ = false;
    bool operationFinished_ = false;
    QString statusText_;
    QString operationStatus_;
};

} // namespace peripheral::app
