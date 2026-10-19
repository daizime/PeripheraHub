#pragma once

#include <QList>
#include <QWidget>
#include <optional>

class QLabel;
class QPushButton;
class QSpinBox;

namespace peripheral::app {

// 只提供百分比编辑交互；设备选择、排队及实际写入均由调用方负责。
class PercentDetails final : public QWidget {
    Q_OBJECT

  public:
    explicit PercentDetails(const QString& title, const QList<int>& presets, QWidget* parent = nullptr);

    void setValue(int value) { setValue(std::optional<int>{value}); }
    void setValue(std::optional<int> value);
    void setAvailable(bool available);
    void setDescription(const QString& description);
    bool isEditing() const;

  signals:
    void valueRequested(int value);
    void refreshRequested();

  private:
    void updateEnabledState();

    QLabel* valueLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QSpinBox* valueEditor_ = nullptr;
    QList<QPushButton*> presetButtons_;
    bool available_ = false;
    bool valueKnown_ = false;
};

} // namespace peripheral::app
