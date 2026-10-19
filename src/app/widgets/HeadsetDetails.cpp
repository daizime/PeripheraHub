#include "HeadsetDetails.h"
#include <QLabel>
#include <QStringList>
#include <QVBoxLayout>

namespace peripheral::app {
HeadsetDetails::HeadsetDetails(QWidget* parent) : QWidget(parent) {
    setObjectName("headsetHardwareDetails");
    setMinimumWidth(0);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(QStringLiteral("N9 V2 ULTRA · 硬件参数"));
    title->setProperty("role", "status");
    layout->addWidget(title);
    state_ = new QLabel;
    peq_ = new QLabel;
    peq_->setObjectName("headsetPeqReadings");
    for (auto* label : {state_, peq_}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setProperty("role", "muted");
        layout->addWidget(label);
    }
    setInfo(nullptr);
}
void HeadsetDetails::setInfo(const platform::N9ControlsInfo* info) {
    setVisible(info != nullptr);
    if (!info)
        return;
    setToolTip(QString::fromStdString(info->error));
    if (!info->status) {
        state_->setText(QStringLiteral("当前未读到有效耳机状态。"));
        peq_->clear();
        return;
    }
    const auto& state = *info->status;
    const auto version = [](const auto& bytes) {
        QStringList pieces;
        for (const auto byte : bytes)
            pieces << QString::number(byte);
        return pieces.join('.');
    };
    state_->setText(QStringLiteral("耳机固件 %1 · 接收器 %2\nEQ 模式代码 %3 · 耳返标志 %4 · 耳返音量原值 %5")
        .arg(version(state.headsetVersion), version(state.dongleVersion))
        .arg(state.eqMode).arg(state.sidetone ? 1 : 0).arg(state.sidetoneVolume));
    QStringList rows{QStringLiteral("10 段 PEQ 读取 · 配置银行代码 1")};
    for (std::size_t index = 0; index < info->peq.size(); ++index) {
        const auto& point = info->peq[index];
        if (!point) {
            rows << QStringLiteral("%1 · 未读取成功").arg(index + 1);
            continue;
        }
        rows << QStringLiteral("%1 Hz · %2 dB · Q %3 · 类型 %4 · 启用标志 %5")
            .arg(point->frequencyHz).arg(point->gainTenthsDb / 10.0, 0, 'f', 1)
            .arg(point->qTenths / 10.0, 0, 'f', 1).arg(point->filterType).arg(point->enabled ? 1 : 0);
    }
    rows << QStringLiteral("以上为设备返回值；当前只读，EQ 写入与实际生效仍待验证。");
    peq_->setText(rows.join('\n'));
}
} // namespace peripheral::app
