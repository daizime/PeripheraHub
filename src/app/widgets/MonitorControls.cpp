#include "MonitorControls.h"
#include "UiTheme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>

namespace peripheral::app {
namespace {

QString codeText(unsigned int value) {
    return QStringLiteral("0x%1").arg(value, 2, 16, QLatin1Char('0')).toUpper().replace("0X", "0x");
}

QLabel* description(const QString& text) {
    auto* result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setProperty("role", "muted");
    result->setMinimumWidth(0);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return result;
}

QString optionText(int code, std::uint16_t value) {
    const QString suffix = QStringLiteral(" (%1)").arg(codeText(value));
    if (code == 0x60) {
        switch (value) {
        case 0x0F:
            return QStringLiteral("DisplayPort 1") + suffix;
        case 0x10:
            return QStringLiteral("DisplayPort 2") + suffix;
        case 0x11:
            return QStringLiteral("HDMI 1") + suffix;
        case 0x12:
            return QStringLiteral("HDMI 2") + suffix;
        default:
            return QStringLiteral("输入代码 %1").arg(codeText(value));
        }
    }
    return (code == 0xDC ? QStringLiteral("设备模式 %1") : QStringLiteral("色彩预设 %1"))
        .arg(codeText(value));
}

} // namespace

MonitorControls::MonitorControls(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    setMinimumWidth(0);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("显示器内置功能"));
    title->setProperty("role", "status");
    layout->addWidget(title);

    const std::array<int, 4> codes{0x12, 0x60, 0xDC, 0x14};
    const std::array<QString, 4> titles{QStringLiteral("对比度"), QStringLiteral("输入信号源"),
                                        QStringLiteral("情景 / 显示模式"), QStringLiteral("色彩预设")};
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        auto& row = rows_[index];
        row.code = codes[index];
        auto* rowTitle = new QLabel(titles[index]);
        rowTitle->setObjectName("monitorRowLabel");
        rowTitle->setProperty("role", "controlLabel");
        rowTitle->setWordWrap(true);
        rowTitle->setFixedWidth(86);
        auto* editorLayout = new QHBoxLayout;
        editorLayout->setSpacing(8);
        editorLayout->addWidget(rowTitle);
        if (row.code == 0x12) {
            row.number = createThemedSpinBox();
            row.number->setKeyboardTracking(false);
            row.number->setAccessibleName(QStringLiteral("显示器对比度原始值"));
            row.number->setObjectName(QStringLiteral("monitorContrastEditor"));
            row.number->setMinimumWidth(0);
            row.number->setProperty("externalSteps", true);
            editorLayout->addWidget(row.number, 1);
            row.decrease = new QPushButton(QString::fromUtf8("−"));
            row.increase = new QPushButton(QStringLiteral("+"));
            row.decrease->setObjectName("monitorContrastDecrease");
            row.increase->setObjectName("monitorContrastIncrease");
            for (auto* step : {row.decrease, row.increase}) {
                step->setProperty("separateNumberStep", true);
                step->setFixedWidth(30);
                step->setMinimumHeight(34);
                step->setAutoRepeat(true);
                step->setFocusPolicy(Qt::NoFocus);
                editorLayout->addWidget(step);
            }
            row.decrease->setAccessibleName(QStringLiteral("对比度减少"));
            row.increase->setAccessibleName(QStringLiteral("对比度增加"));
            connect(row.decrease, &QPushButton::clicked, row.number, &QSpinBox::stepDown);
            connect(row.increase, &QPushButton::clicked, row.number, &QSpinBox::stepUp);
            connect(row.number, &QSpinBox::valueChanged, this, [this] { updateEnabledState(); });
        } else {
            row.options = createThemedComboBox();
            // 横向参数行按可用宽度省略选项文字，完整文字继续在弹层和提示中显示。
            row.options->setMinimumWidth(96);
            row.options->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            row.options->setAccessibleName(titles[index]);
            row.options->setObjectName(QStringLiteral("monitorFeature%1Options").arg(row.code));
            editorLayout->addWidget(row.options, 1);
        }
        row.apply = new QPushButton(row.code == 0x60 ? QStringLiteral("切换") : QStringLiteral("应用"));
        row.apply->setObjectName(QStringLiteral("monitorFeature%1Apply").arg(row.code));
        row.apply->setAccessibleName(titles[index] + row.apply->text());
        row.apply->setProperty("compact", true);
        editorLayout->addWidget(row.apply);
        layout->addLayout(editorLayout);
        row.state = description({});
        row.state->setContentsMargins(86, 0, 0, 0);
        layout->addWidget(row.state);
        connect(row.apply, &QPushButton::clicked, this, [this, index] { applyRow(index); });
        if (row.options) {
            connect(row.options, &QComboBox::currentIndexChanged, this, [this] { updateEnabledState(); });
        }
    }

    layout->addWidget(description(QStringLiteral("情景与色彩预设按能力表显示设备代码。")));

    status_ = description({});
    status_->setObjectName(QStringLiteral("monitorControlsStatus"));
    layout->addWidget(status_);
    refresh_ = new QPushButton(QStringLiteral("读取显示器功能"));
    refresh_->setIcon(fluentIcon(UiIcon::Refresh));
    refresh_->setIconSize(QSize(16, 16));
    refresh_->setObjectName(QStringLiteral("monitorControlsRefresh"));
    refresh_->setProperty("compact", true);
    layout->addWidget(refresh_, 0, Qt::AlignLeft);
    connect(refresh_, &QPushButton::clicked, this, [this] {
        clearOperationStatus();
        emit refreshRequested();
    });
    clear();
}

void MonitorControls::setBrightnessControl(QWidget* control) {
    static_cast<QVBoxLayout*>(layout())->insertWidget(1, control);
}

void MonitorControls::setInfo(const platform::MonitorControlsInfo& info) {
    const bool targetChanged = info_.id != info.id;
    if (targetChanged)
        clearOperationStatus();
    info_ = info;
    for (auto& row : rows_) {
        if (targetChanged)
            row.mappingConfirmed = false;
        const auto found = std::find_if(info.features.begin(), info.features.end(),
                                        [&row](const auto& feature) { return feature.code == row.code; });
        updateRow(row, found == info.features.end() ? nullptr : &*found);
    }
    const QString capabilities = QString::fromStdString(info.capabilities);
    setToolTip(plainTextToolTip(capabilities));
    if (!info.capabilitiesError.empty())
        setStatus(QString::fromStdString(info.capabilitiesError));
    else
        setStatus(QStringLiteral("选项来自设备能力表；实际支持与写入结果以设备回读为准。"));
    updateEnabledState();
}

void MonitorControls::setBusy(bool busy) {
    // 禁用当前应用按钮前先移除焦点，避免 Qt 跳到后面的亮度输入并滚动详情。
    if (busy) {
        if (auto* focused = focusWidget(); focused && isAncestorOf(focused))
            focused->clearFocus();
    }
    busy_ = busy;
    updateEnabledState();
    showStatus();
}

void MonitorControls::setStatus(const QString& status) {
    statusText_ = status;
    showStatus();
}

void MonitorControls::setOperationStatus(const QString& status, bool finished) {
    operationStatus_ = status;
    operationFinished_ = finished;
    showStatus();
}

void MonitorControls::clearOperationStatus() {
    operationStatus_.clear();
    operationFinished_ = false;
    showStatus();
}

void MonitorControls::showStatus() {
    QString text = operationStatus_.isEmpty() ? statusText_ : operationStatus_;
    if (!operationStatus_.isEmpty() && operationFinished_ && busy_)
        text += QStringLiteral("\n正在后台补读其他功能…");
    status_->setText(text);
    status_->setVisible(!text.isEmpty());
}

void MonitorControls::clear() {
    busy_ = false;
    clearOperationStatus();
    info_ = {};
    setToolTip({});
    for (auto& row : rows_) {
        row.mappingConfirmed = false;
        updateRow(row, nullptr);
    }
    setStatus(QStringLiteral("尚未读取显示器内置功能。"));
    updateEnabledState();
}

void MonitorControls::updateRow(Row& row, const platform::MonitorFeatureInfo* feature) {
    row.feature = feature ? *feature : platform::MonitorFeatureInfo{};
    row.available = feature && feature->readable && feature->writable;
    QString state;
    if (!feature || !feature->advertised)
        state = QStringLiteral("能力表未声明该功能。");
    else if (!feature->readable)
        state = QStringLiteral("当前值未读取成功；暂不可调整。");
    else
        state = row.code == 0x12 ? QStringLiteral("当前值 %1").arg(feature->current)
                                 : QStringLiteral("当前设备值 %1").arg(codeText(feature->current));

    if (row.number) {
        const QSignalBlocker blocker(row.number);
        const bool validRange = feature && feature->readable && feature->maximum > 0 &&
                                feature->maximum <= std::numeric_limits<std::uint16_t>::max() &&
                                feature->current <= feature->maximum;
        row.available = row.available && validRange;
        row.number->setRange(0, validRange ? static_cast<int>(feature->maximum) : 0);
        row.number->setSpecialValueText(validRange ? QString{} : QStringLiteral("—"));
        row.number->setValue(validRange ? static_cast<int>(feature->current) : 0);
        if (validRange)
            state += QStringLiteral("；原始范围 0–%1。").arg(feature->maximum);
        else if (feature && feature->readable)
            state += QStringLiteral("；回报范围无效，暂不可调整。");
    } else {
        // 非连续控制只能列出能力表枚举，绝不把 maximum 展开成候选值。
        const QSignalBlocker blocker(row.options);
        row.options->clear();
        if (feature) {
            for (const auto value : feature->allowedValues) {
                if (row.options->findData(value) < 0)
                    row.options->addItem(optionText(row.code, value), value);
            }
        }
        const int currentIndex = feature && feature->readable ? row.options->findData(feature->current) : -1;
        row.options->setCurrentIndex(currentIndex);
        if (feature && feature->readable && currentIndex < 0) {
            row.options->setPlaceholderText(
                QStringLiteral("当前代码 %1 未列入能力表").arg(codeText(feature->current)));
            state += QStringLiteral("；设备回报不一致；选项来自能力表，名称/写入待验证。");
        } else {
            row.options->setPlaceholderText(QStringLiteral("无可用选项"));
            if (feature && feature->readable && (row.code == 0xDC || row.code == 0x14))
                state += QStringLiteral("；代码名称尚未核定。");
        }
        row.available = row.available && row.options->count() > 0;
    }
    // 设备原始错误供诊断查看，常用说明保持简洁，避免重复显示英文协议细节。
    row.state->setToolTip(plainTextToolTip(feature ? QString::fromStdString(feature->error) : QString{}));
    if (feature && feature->readable && !feature->writable)
        state += QStringLiteral(" 当前只读。");
    row.state->setText(state);
}

void MonitorControls::updateEnabledState() {
    for (auto& row : rows_) {
        const bool available = !busy_ && !info_.id.empty() && row.available;
        if (row.number)
            row.number->setEnabled(available);
        if (row.decrease) {
            row.decrease->setEnabled(available && row.number->value() > row.number->minimum());
            row.increase->setEnabled(available && row.number->value() < row.number->maximum());
        }
        if (row.options)
            row.options->setEnabled(available);
        row.apply->setEnabled(available && (!row.options || row.options->currentIndex() >= 0));
    }
    refresh_->setEnabled(!busy_);
}

void MonitorControls::applyRow(std::size_t index) {
    auto& row = rows_.at(index);
    if (busy_ || info_.id.empty() || !row.available)
        return;
    if (row.number)
        row.number->interpretText();
    const int value = row.number ? row.number->value() : row.options->currentData().toInt();
    if (row.options && row.options->currentIndex() < 0)
        return;
    const auto requestedTarget = info_.id;
    bool mappingConfirmedNow = false;

    // 更改输入可能让控制端立即失去画面，必须由用户明确确认。
    if (row.code == 0x60) {
        if (!confirmThemedAction(this, QStringLiteral("切换显示器输入信号源"),
                                  QStringLiteral("将切换为 %1。可能无信号，必要时用显示器按键切回。")
                                      .arg(row.options->currentText())))
            return;
    } else if (row.options && !row.mappingConfirmed) {
        if (!confirmThemedAction(this, QStringLiteral("确认设备预设代码"),
                                  QStringLiteral("%1 的菜单名称尚未验证。应用后请对照显示器菜单确认效果。")
                                      .arg(row.options->currentText())))
            return;
        mappingConfirmedNow = true;
    }
    // 确认框会处理其他事件；期间若切换了显示器或重新探测失败，则取消旧请求。
    if (info_.id != requestedTarget || busy_ || !row.available ||
        (row.options && row.options->findData(value) < 0) ||
        (row.number && (value < row.number->minimum() || value > row.number->maximum())))
        return;
    if (mappingConfirmedNow)
        row.mappingConfirmed = true;
    clearOperationStatus();
    emit valueRequested(row.code, value);
}

} // namespace peripheral::app
