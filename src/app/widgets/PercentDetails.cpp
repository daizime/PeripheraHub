#include "PercentDetails.h"
#include "UiTheme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace peripheral::app {

PercentDetails::PercentDetails(const QString& title, const QList<int>& presets, QWidget* parent)
    : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* heading = new QHBoxLayout;
    auto* titleLabel = new QLabel(title);
    titleLabel->setProperty("role", "status");
    heading->addWidget(titleLabel, 1);
    valueLabel_ = new QLabel(QStringLiteral("—"));
    valueLabel_->setProperty("role", "status");
    heading->addWidget(valueLabel_);
    layout->addLayout(heading);

    auto* editorRow = new QHBoxLayout;
    editorRow->setSpacing(8);
    auto* editorLabel = new QLabel(QStringLiteral("精确设置"));
    valueEditor_ = createThemedSpinBox();
    valueEditor_->setRange(0, 100);
    valueEditor_->setSuffix(QStringLiteral("%"));
    // 输入 50 时仅在确认后发出 50，避免途中把设备写成 5。
    valueEditor_->setKeyboardTracking(false);
    valueEditor_->setAccessibleName(title + QStringLiteral("百分比"));
    valueEditor_->setMinimumWidth(128);
    editorLabel->setBuddy(valueEditor_);
    editorRow->addWidget(editorLabel);
    editorRow->addWidget(valueEditor_);
    editorRow->addStretch(1);
    auto* refresh = new QPushButton(QStringLiteral("更新状态"));
    refresh->setIcon(fluentIcon(UiIcon::Refresh));
    refresh->setIconSize(QSize(16, 16));
    refresh->setProperty("compact", true);
    editorRow->addWidget(refresh);
    layout->addLayout(editorRow);

    auto* presetsRow = new QHBoxLayout;
    presetsRow->setSpacing(6);
    for (const int preset : presets) {
        if (preset < 0 || preset > 100)
            continue;
        auto* button = new QPushButton(QString::number(preset) + QStringLiteral("%"));
        button->setProperty("compact", true);
        button->setAccessibleName(title + QStringLiteral("设为 ") + button->text());
        presetButtons_.append(button);
        presetsRow->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, preset] {
            // 先同步自身显示，再发送一次请求；同步本身不触发设备写入。
            setValue(preset);
            emit valueRequested(preset);
        });
    }
    presetsRow->addStretch(1);
    layout->addLayout(presetsRow);

    descriptionLabel_ = new QLabel;
    descriptionLabel_->setTextFormat(Qt::PlainText);
    descriptionLabel_->setProperty("role", "muted");
    descriptionLabel_->setWordWrap(true);
    layout->addWidget(descriptionLabel_);

    connect(valueEditor_, &QSpinBox::valueChanged, this, [this](int value) {
        valueKnown_ = true;
        valueLabel_->setText(QString::number(value) + QStringLiteral("%"));
        emit valueRequested(value);
    });
    connect(refresh, &QPushButton::clicked, this, &PercentDetails::refreshRequested);
    setValue(std::nullopt);
}

void PercentDetails::setValue(std::optional<int> value) {
    // 对外同步入口始终静默，避免轮询读数引起反向写入循环。
    const QSignalBlocker blocker(valueEditor_);
    valueKnown_ = value.has_value() && *value >= 0 && *value <= 100;
    valueEditor_->setSpecialValueText(valueKnown_ ? QString{} : QStringLiteral("—"));
    valueEditor_->setValue(valueKnown_ ? *value : 0);
    valueLabel_->setText(valueKnown_ ? QString::number(*value) + QStringLiteral("%") : QStringLiteral("—"));
    updateEnabledState();
}

void PercentDetails::setAvailable(bool available) {
    available_ = available;
    if (!available)
        setValue(std::nullopt);
    else
        updateEnabledState();
}

void PercentDetails::setDescription(const QString& description) {
    descriptionLabel_->setText(description);
    descriptionLabel_->setVisible(!description.isEmpty());
}

bool PercentDetails::isEditing() const {
    const auto* editor = valueEditor_->findChild<QLineEdit*>();
    return valueEditor_->hasFocus() || (editor && editor->hasFocus());
}

void PercentDetails::updateEnabledState() {
    // 未知不是 0%；读取成功前不允许通过本面板修改设备。
    const bool enabled = available_ && valueKnown_;
    valueEditor_->setEnabled(enabled);
    for (auto* button : presetButtons_)
        button->setEnabled(enabled);
}

} // namespace peripheral::app
