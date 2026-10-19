#include "DeviceCard.h"
#include "UiTheme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextEdit>
#include <QTextDocument>
#include <QVBoxLayout>

namespace peripheral::app {
namespace {

// QStackedWidget 默认取所有页面的最大建议尺寸，会被内容较多的详情撑大。
// 页面尺寸交给卡片分配；详情的最小内容尺寸由其内部滚动区域处理。
class CardPages final : public QStackedWidget {
  public:
    using QStackedWidget::QStackedWidget;
    QSize sizeHint() const override { return {0, 0}; }
    QSize minimumSizeHint() const override { return {0, 0}; }
};

QLabel* headerLabel(const QString& text, const char* role) {
    auto* result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    result->setProperty("role", role);
    result->setWordWrap(true);
    return result;
}

} // namespace

DeviceCard::DeviceCard(const QString& category, const QString& title, QWidget* parent)
    : QFrame(parent), title_(title) {
    setObjectName("deviceCard");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(14);
    layout->setSizeConstraint(QLayout::SetNoConstraint);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setObjectName("deviceCardHeader");
    header->setSpacing(18);
    categoryIcon_ = new QLabel;
    categoryIcon_->setObjectName("deviceCategoryIcon");
    categoryIcon_->setFixedSize(64, 64);
    categoryIcon_->setAlignment(Qt::AlignCenter);
    categoryIcon_->setMargin(15);
    categoryIcon_->setScaledContents(true);
    categoryIcon_->hide();
    header->addWidget(categoryIcon_, 0, Qt::AlignTop);
    auto* titles = new QVBoxLayout;
    titles->setContentsMargins(0, 0, 0, 0);
    titles->setSpacing(7);
    titles->addWidget(headerLabel(category, "category"));
    titleLabel_ = headerLabel(title, "cardTitle");
    titleLabel_->setTextFormat(Qt::PlainText);
    titleLabel_->setWordWrap(false);
    // 设备名称可能很长；单行省略并忽略横向建议尺寸，不挤动相邻卡片。
    titleLabel_->setMinimumWidth(0);
    titleLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    titles->addWidget(titleLabel_);
    header->addLayout(titles, 1);

    toggleButton_ = new QPushButton("更多");
    toggleButton_->setObjectName("cardToggle");
    toggleButton_->setCheckable(true);
    toggleButton_->setIcon(fluentIcon(UiIcon::More));
    toggleButton_->setIconSize(QSize(20, 20));
    toggleButton_->setAccessibleName(title + "：展开更多功能");
    toggleButton_->setToolTip("点击卡片空白处，也可以展开更多功能");
    setTitle(title);
    header->addWidget(toggleButton_, 0, Qt::AlignTop);
    layout->addLayout(header);

    pages_ = new CardPages;
    pages_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    const auto addPage = [this](int rightMargin) {
        auto* scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet("QScrollArea { background:transparent; border:0; }"
                              "QScrollArea > QWidget > QWidget { background:transparent; }");
        auto* page = new QWidget;
        auto* body = new QVBoxLayout(page);
        body->setContentsMargins(0, 0, rightMargin, 0);
        body->setSpacing(10);
        // 两页均保留控件的正常最小高度；小窗口通过内部滚动访问，不裁切文字。
        body->setSizeConstraint(QLayout::SetMinimumSize);
        scroll->setWidget(page);
        pages_->addWidget(scroll);
        return body;
    };
    quickLayout_ = addPage(0);
    detailLayout_ = addPage(0);
    layout->addWidget(pages_, 1);

    connect(toggleButton_, &QPushButton::clicked, this, &DeviceCard::setExpanded);

    // 应用级过滤器能识别后续加入布局的控件，无须修改每个子控件的事件处理。
    // 仅观察本卡片内的鼠标事件；交互控件的事件始终原样传递。
    qApp->installEventFilter(this);
}

void DeviceCard::setCategoryIcon(const QIcon& icon) {
    // 保留 4x 源图，由 QLabel 按所在屏幕缩放，跨 DPI 屏幕移动时不沿用低分辨率缓存。
    categoryIcon_->setPixmap(icon.pixmap(QSize(96, 96)));
    categoryIcon_->setVisible(!icon.isNull());
}

void DeviceCard::setTitle(const QString& title) {
    title_ = title;
    titleLabel_->setToolTip(plainTextToolTip(title_));
    titleLabel_->setAccessibleName(title_);
    updateTitleLabel();
    toggleButton_->setAccessibleName(title_ + (expanded_ ? "：收起更多功能" : "：展开更多功能"));
}

void DeviceCard::updateTitleLabel() {
    titleLabel_->setText(
        titleLabel_->fontMetrics().elidedText(title_, Qt::ElideRight, titleLabel_->contentsRect().width()));
}

QSize DeviceCard::sizeHint() const {
    return {420, 310};
}

QSize DeviceCard::minimumSizeHint() const {
    return {300, 275};
}

bool DeviceCard::hasHeightForWidth() const {
    // QWidget 默认转发内部布局的换行高度；切页会使父网格在小窗口中重新分配行高。
    // 外框由网格分配，页面在内部滚动，不能把当前页的高度需求传给其他卡片。
    return false;
}

void DeviceCard::setExpanded(bool expanded) {
    if (expanded_ == expanded)
        return;
    expanded_ = expanded;
    clickArmed_ = false;
    pages_->setCurrentIndex(expanded ? 1 : 0);
    toggleButton_->setChecked(expanded);
    toggleButton_->setText(expanded ? "收起" : "更多");
    toggleButton_->setIcon(fluentIcon(expanded ? UiIcon::ChevronUp : UiIcon::More));
    toggleButton_->setAccessibleName(title_ + (expanded ? "：收起更多功能" : "：展开更多功能"));
    toggleButton_->setAccessibleDescription(expanded ? "返回常用控制" : "在卡片内显示更多功能");
    emit expandedChanged(expanded_);
    emit expansionChanged(cardId_, expanded_);
}

bool DeviceCard::isBlankClickTarget(const QWidget* widget) const {
    if (!widget || (widget != this && !isAncestorOf(widget)))
        return false;

    for (auto* current = widget; current && current != this; current = current->parentWidget()) {
        // 自定义控件可声明此属性，明确排除空白处切换行为。
        if (current->property("deviceCardInteractive").toBool())
            return false;
        if (qobject_cast<const QAbstractButton*>(current) || qobject_cast<const QAbstractSlider*>(current) ||
            qobject_cast<const QAbstractSpinBox*>(current) || qobject_cast<const QComboBox*>(current) ||
            qobject_cast<const QLineEdit*>(current) || qobject_cast<const QTextEdit*>(current) ||
            qobject_cast<const QPlainTextEdit*>(current) || qobject_cast<const QAbstractItemView*>(current) ||
            qobject_cast<const QTabBar*>(current))
            return false;
        if (const auto* group = qobject_cast<const QGroupBox*>(current); group && group->isCheckable())
            return false;
        if (const auto* text = qobject_cast<const QLabel*>(current)) {
            if (text->textInteractionFlags() & Qt::TextSelectableByMouse)
                return false;
            // QLabel 默认开启 LinksAccessibleByMouse；普通文字仍属于可点击的空白区域。
            const bool richText = text->textFormat() != Qt::PlainText &&
                                  (text->textFormat() != Qt::AutoText || Qt::mightBeRichText(text->text()));
            if (richText && (text->textInteractionFlags() & Qt::LinksAccessibleByMouse))
                return false;
        }
        // 保守保留未知交互控件的操作；滚动区域容器本身允许空白处点击。
        if (current->focusPolicy() != Qt::NoFocus && !qobject_cast<const QAbstractScrollArea*>(current))
            return false;
    }
    return true;
}

bool DeviceCard::eventFilter(QObject* watched, QEvent* event) {
    if (watched == titleLabel_ && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange ||
                                   event->type() == QEvent::StyleChange))
        updateTitleLabel();
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease &&
        event->type() != QEvent::MouseButtonDblClick && event->type() != QEvent::MouseMove)
        return QFrame::eventFilter(watched, event);

    const auto* mouse = static_cast<QMouseEvent*>(event);
    if (event->type() == QEvent::MouseMove) {
        if (clickArmed_ && (mouse->globalPosition().toPoint() - pressPosition_).manhattanLength() >=
                               QApplication::startDragDistance())
            clickArmed_ = false;
        return QFrame::eventFilter(watched, event);
    }
    if (mouse->button() != Qt::LeftButton)
        return QFrame::eventFilter(watched, event);
    const auto* target = qobject_cast<QWidget*>(watched);
    const QPoint localPosition = mapFromGlobal(mouse->globalPosition().toPoint());
    const bool inside = rect().contains(localPosition);
    if (!target || (target != this && !isAncestorOf(target))) {
        // QWidget 会把未处理的事件继续传给父窗口；不能因此取消刚记录的卡片点击。
        if (!inside)
            clickArmed_ = false;
        return QFrame::eventFilter(watched, event);
    }
    const QWidget* hit = childAt(localPosition);
    if (!hit)
        hit = this;
    // 同时检查实际位置，防止禁用控件将鼠标事件向父级透传后触发切换。
    const bool blankTarget =
        inside && isVisible() && isEnabled() && isBlankClickTarget(target) && isBlankClickTarget(hit);

    if (event->type() == QEvent::MouseButtonPress) {
        clickArmed_ = blankTarget && mouse->buttons() == Qt::LeftButton;
        if (clickArmed_)
            pressPosition_ = mouse->globalPosition().toPoint();
    } else if (event->type() == QEvent::MouseButtonDblClick) {
        clickArmed_ = false;
    } else {
        const bool clicked = clickArmed_ && blankTarget && mouse->buttons() == Qt::NoButton &&
                             (mouse->globalPosition().toPoint() - pressPosition_).manhattanLength() <
                                 QApplication::startDragDistance();
        clickArmed_ = false;
        if (clicked) {
            setExpanded(!expanded_);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace peripheral::app
