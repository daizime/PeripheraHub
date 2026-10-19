#include "DeviceGrid.h"
#include "DeviceCard.h"

#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>

namespace peripheral::app {

DeviceGrid::DeviceGrid(QWidget* parent) : QWidget(parent) {
    setObjectName("deviceGrid");
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
    // 外层窗口可继续决定可用高度；卡片内容过多时由自身滚动区域处理。
    layout_->setSizeConstraint(QLayout::SetNoConstraint);

    empty_ = new QLabel;
    empty_->setObjectName("deviceGridEmpty");
    empty_->setProperty("role", "muted");
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setMinimumWidth(0);
    empty_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout_->addWidget(empty_, 1);

    grid_ = new QGridLayout;
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setHorizontalSpacing(18);
    grid_->setVerticalSpacing(18);
    grid_->setColumnStretch(0, 49);
    grid_->setColumnStretch(1, 51);
    layout_->addLayout(grid_);
    layout_->addStretch(0);
    updateEmptyText();
}

DeviceGrid::~DeviceGrid() {
    // 子控件由 QWidget 的析构稍后销毁；此时成员已析构，不能再执行重排回调。
    for (const auto& entry : entries_)
        if (entry.card)
            disconnect(entry.card, nullptr, this, nullptr);
}

void DeviceGrid::addCard(DeviceCard* card) {
    if (!card || std::any_of(entries_.begin(), entries_.end(),
                             [card](const Entry& entry) { return entry.card == card; }))
        return;
    card->setParent(this);
    card->hide();
    entries_.push_back({card, false});
    // QWidget 与 QPointer 的销毁时序不同，先移除该登记项再重排。
    connect(card, &QObject::destroyed, this, [this, card] {
        std::erase_if(entries_, [card](const Entry& entry) {
            return entry.card.isNull() || entry.card.data() == card;
        });
        reflow();
    });
}

void DeviceGrid::setPresent(DeviceCard* card, bool present) {
    const auto entry = std::find_if(entries_.begin(), entries_.end(),
                                    [card](const Entry& entry) { return entry.card == card; });
    if (entry == entries_.end() || entry->present == present)
        return;
    entry->present = present;
    reflow();
}

void DeviceGrid::setScanning(bool scanning) {
    if (scanning_ == scanning)
        return;
    scanning_ = scanning;
    updateEmptyText();
}

int DeviceGrid::visibleCardCount() const {
    return static_cast<int>(std::count_if(entries_.begin(), entries_.end(),
                                          [](const Entry& entry) { return entry.card && entry.present; }));
}

void DeviceGrid::updateEmptyText() {
    empty_->setText(scanning_ ? QStringLiteral("正在识别设备…")
                              : QStringLiteral("尚未检测到可管理设备，接入后会自动显示"));
}

void DeviceGrid::reflow() {
    // 只删除布局项，保留卡片及其选择、输入值与展开状态。
    while (auto* item = grid_->takeAt(0))
        delete item;
    for (int row = 0; row < grid_->rowCount(); ++row)
        grid_->setRowStretch(row, 0);
    std::erase_if(entries_, [](const Entry& entry) { return entry.card.isNull(); });

    int index = 0;
    for (const auto& entry : entries_) {
        entry.card->setVisible(entry.present);
        if (entry.present) {
            grid_->addWidget(entry.card, index / 2, index % 2);
            grid_->setRowStretch(index / 2, 1);
            ++index;
        }
    }
    empty_->setVisible(index == 0);
    if (index > 2) {
        // 参考图上排略高，右列略宽，给显示器控制与音频选择留出空间。
        grid_->setRowStretch(0, 52);
        grid_->setRowStretch(1, 48);
    }
    layout_->setStretch(0, index == 0 ? 1 : 0);
    // 单行按卡片建议高度显示；尾部伸缩区吸收空余空间，避免卡片过高。
    // 两行共用剩余高度，展开不会触发重新登记或重排。
    layout_->setStretch(1, index > 2 ? 1 : 0);
    layout_->setStretch(2, index > 0 && index <= 2 ? 1 : 0);
}

} // namespace peripheral::app
