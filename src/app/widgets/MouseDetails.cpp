#include "MouseDetails.h"
#include "UiTheme.h"
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

namespace peripheral::app {
namespace {
QString actionText(const vgn::ButtonMapping& action) {
    if (action.actionClass == 1) {
        switch (action.parameter) {
        case 0x0100: return QStringLiteral("左键");
        case 0x0200: return QStringLiteral("右键");
        case 0x0400: return QStringLiteral("中键");
        case 0x0800: return QStringLiteral("后退");
        case 0x1000: return QStringLiteral("前进");
        }
    }
    if (action.actionClass == 2 && action.parameter == 0x0100)
        return QStringLiteral("DPI 循环");
    return QStringLiteral("类型 %1 · 参数 0x%2（名称未确认）")
        .arg(action.actionClass).arg(action.parameter, 4, 16, QLatin1Char('0'));
}
QWidget* writeRow(const QString& label, QWidget* input, QPushButton* apply, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto* text = new QLabel(label, row);
    text->setProperty("role", "muted");
    layout->addWidget(text);
    layout->addWidget(input, 1);
    layout->addWidget(apply);
    return row;
}
} // namespace

MouseDetails::MouseDetails(QWidget* parent) : QWidget(parent) {
    setObjectName("mouseHardwareDetails");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    state_ = new QLabel;
    stages_ = new QLabel;
    buttons_ = new QLabel;
    performance_ = new QLabel;
    stages_->setObjectName("mouseDpiStages");
    buttons_->setObjectName("mouseButtonMappings");
    for (auto* item : {state_, stages_, buttons_, performance_}) {
        item->setTextFormat(Qt::PlainText);
        item->setWordWrap(true);
        item->setMinimumWidth(0);
        item->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        item->setProperty("role", "muted");
        layout->addWidget(item);
    }

    // 写入区：只在"实验性设备写入"开启且当前型号已通过写入验证时出现。
    editor_ = new QWidget(this);
    editor_->setObjectName("mouseWriteEditor");
    auto* editorLayout = new QVBoxLayout(editor_);
    editorLayout->setContentsMargins(0, 6, 0, 0);
    editorLayout->setSpacing(6);
    auto* hint = new QLabel(QStringLiteral("写入当前档位 DPI、回报率或切换档位。每次都立即回读；失败不回滚。"), editor_);
    hint->setTextFormat(Qt::PlainText);
    hint->setWordWrap(true);
    hint->setProperty("role", "muted");
    editorLayout->addWidget(hint);

    dpiInput_ = createThemedSpinBox();
    dpiInput_->setRange(50, 42000);
    dpiInput_->setSingleStep(50);
    dpiInput_->setSuffix(QStringLiteral(" DPI"));
    dpiApply_ = new QPushButton(QStringLiteral("应用"), editor_);
    dpiApply_->setObjectName("mouseDpiApply");
    editorLayout->addWidget(writeRow(QStringLiteral("档位 DPI"), dpiInput_, dpiApply_, editor_));

    rateInput_ = createThemedComboBox();
    rateInput_->setObjectName("mouseRateInput");
    for (const int hertz : {125, 250, 500, 1000, 2000, 4000, 8000})
        rateInput_->addItem(QStringLiteral("%1 Hz").arg(hertz), hertz);
    rateApply_ = new QPushButton(QStringLiteral("应用"), editor_);
    rateApply_->setObjectName("mouseRateApply");
    editorLayout->addWidget(writeRow(QStringLiteral("回报率"), rateInput_, rateApply_, editor_));

    stageInput_ = createThemedComboBox();
    stageInput_->setObjectName("mouseStageInput");
    stageApply_ = new QPushButton(QStringLiteral("应用"), editor_);
    stageApply_->setObjectName("mouseStageApply");
    editorLayout->addWidget(writeRow(QStringLiteral("切换档位"), stageInput_, stageApply_, editor_));

    layout->addWidget(editor_);
    editor_->hide();

    connect(dpiApply_, &QPushButton::clicked, this, [this] { emit dpiWriteRequested(dpiInput_->value()); });
    connect(rateApply_, &QPushButton::clicked, this, [this] { emit rateWriteRequested(rateInput_->currentData().toInt()); });
    connect(stageApply_, &QPushButton::clicked, this, [this] { emit stageWriteRequested(stageInput_->currentData().toInt()); });
    setInfo(nullptr);
}

QString MouseDetails::dpiText(const vgn::DpiStage& stage) {
    return stage.x == stage.y ? QStringLiteral("%1 DPI").arg(stage.x)
        : QStringLiteral("X %1 / Y %2 DPI").arg(stage.x).arg(stage.y);
}

void MouseDetails::setWritable(bool writable) {
    writable_ = writable;
    refreshStateText();
    updateEditor();
}

// 状态行必须与写入区一致：开关关闭时不能说可写，开启后也不能仍写"只读"。
void MouseDetails::refreshStateText() {
    if (!profile_) {
        state_->clear();
        return;
    }
    state_->setText(QStringLiteral("F2 Master+ · %1 · %2\n%3 Hz · %4")
        .arg(stale_ ? QStringLiteral("上次读数") : QStringLiteral("设备配置读数"))
        .arg(QDateTime::fromMSecsSinceEpoch(sampledMs_).toString("HH:mm:ss"))
        .arg(profile_->pollingRateHz)
        .arg(writable_ ? QStringLiteral("写入已开启（实验性）") : QStringLiteral("配置目前只读")));
}

void MouseDetails::updateEditor() {
    editor_->setVisible(writable_ && profile_ != nullptr && !profile_->stages.empty());
}

void MouseDetails::setInfo(const vgn::Profile* profile, qint64 sampledMs, bool stale,
                           const QString& error) {
    // 保存副本而不是指针：来源是服务快照里的数据，快照被替换后原对象即销毁，
    // setWritable 等入口仍可能在两次刷新之间访问这份配置。
    if (profile) {
        profileCopy_ = *profile;
        profile_ = &*profileCopy_;
    } else {
        profileCopy_.reset();
        profile_ = nullptr;
    }
    sampledMs_ = sampledMs;
    stale_ = stale;
    setVisible(profile != nullptr);
    if (!profile) {
        for (auto* item : {state_, stages_, buttons_, performance_})
            item->clear();
        deviceDpi_ = -1; // 换设备后重新从设备值开始，不沿用上一台鼠标的输入。
        updateEditor();
        return;
    }
    setToolTip(error);
    refreshStateText();
    QStringList stages{QStringLiteral("DPI 档位")};
    for (std::size_t index = 0; index < profile->stages.size(); ++index)
        stages << QStringLiteral("%1 · %2%3").arg(index + 1).arg(dpiText(profile->stages[index]))
            .arg(static_cast<int>(index) == profile->activeStage ? QStringLiteral(" · 当前档位") : QString{});
    stages_->setText(stages.join('\n'));
    const QStringList names{QStringLiteral("左键"), QStringLiteral("右键"), QStringLiteral("中键"),
        QStringLiteral("侧键 1"), QStringLiteral("侧键 2"), QStringLiteral("DPI 键")};
    QStringList buttons{QStringLiteral("按键配置（读取）")};
    for (std::size_t index = 0; index < profile->buttons.size(); ++index)
        buttons << names[static_cast<qsizetype>(index)] + QStringLiteral(" → ") + actionText(profile->buttons[index]);
    buttons_->setText(buttons.join('\n'));
    performance_->setText(QStringLiteral("LOD 代码 %1 · 按键去抖 %2 ms · 休眠参数 %3 s\n"
        "Motion Sync %4 · 直线修正 %5 · 波纹修正 %6 · 性能代码 %7\n"
        "DPI 档位、回报率与当前档位写入已在实机验证；按键与性能写入仍待验证。")
        .arg(profile->lodCode).arg(profile->debounceMs).arg(profile->sleepSeconds)
        .arg(profile->motionSync ? 1 : 0).arg(profile->angleSnapping ? 1 : 0)
        .arg(profile->rippleControl ? 1 : 0).arg(profile->performanceCode));

    // 写入控件按当前读数预填，不写入任何内容；用户点"应用"才发请求。
    // 正在编辑的数值框不被后台刷新覆盖，否则每 3 秒一次的扫描会把用户输入抹掉。
    const bool validStage = profile->activeStage >= 0 &&
        static_cast<std::size_t>(profile->activeStage) < profile->stages.size();
    if (validStage) {
        const auto& current = profile->stages[profile->activeStage];
        // 用户没有改动过输入框（值仍等于上次显示的设备值）时才跟随设备；
        // 否则后台每 3 秒一次的扫描会把正在输入的数值抹掉。首次显示总是跟随。
        if (deviceDpi_ < 0 || dpiInput_->value() == deviceDpi_)
            dpiInput_->setValue(current.x);
        deviceDpi_ = current.x;
        stageInput_->clear();
        for (std::size_t index = 0; index < profile->stages.size(); ++index)
            stageInput_->addItem(QStringLiteral("%1 · %2").arg(index + 1).arg(dpiText(profile->stages[index])),
                                 static_cast<int>(index));
        stageInput_->setCurrentIndex(profile->activeStage);
    }
    const int rateIndex = rateInput_->findData(profile->pollingRateHz);
    if (rateIndex >= 0)
        rateInput_->setCurrentIndex(rateIndex);
    updateEditor();
}
} // namespace peripheral::app
