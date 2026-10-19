#include "UiTheme.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QFile>
#include <QHelpEvent>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QImage>
#include <QPainter>
#include <QPushButton>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QWindow>
#include <QResizeEvent>
#include <QHash>
#include <QDebug>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPointer>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QToolTip>
#include <QTextDocument>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <QShowEvent>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace peripheral::app {
namespace {

void scrollOutsideInput(QWidget* input, QWheelEvent* event) {
    // 显式转交视口兼容触控板与合成事件；消费原事件，避免 Qt 再次冒泡而重复滚动。
    for (auto* parent = input->parentWidget(); parent; parent = parent->parentWidget()) {
        auto* area = qobject_cast<QAbstractScrollArea*>(parent);
        if (!area)
            continue;
        auto* viewport = area->viewport();
        QWheelEvent forwarded(viewport->mapFromGlobal(event->globalPosition()), event->globalPosition(),
                              event->pixelDelta(), event->angleDelta(), event->buttons(), event->modifiers(),
                              event->phase(), event->inverted(), event->source(), event->pointingDevice());
        forwarded.setTimestamp(event->timestamp());
        QCoreApplication::sendEvent(viewport, &forwarded);
        if (forwarded.isAccepted())
            break;
    }
    event->accept();
}

class ScrollingSpinBox final : public QSpinBox {
  public:
    explicit ScrollingSpinBox(QWidget* parent) : QSpinBox(parent) {
        setFocusPolicy(Qt::StrongFocus);
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        lineEdit()->installEventFilter(this);
        decrease_ = new QPushButton(QString::fromUtf8("−"), this);
        increase_ = new QPushButton(QStringLiteral("+"), this);
        decrease_->setObjectName("numberDecrease");
        increase_->setObjectName("numberIncrease");
        for (auto* button : {decrease_, increase_}) {
            button->setProperty("numberStep", true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setAutoRepeat(true);
        }
        connect(decrease_, &QPushButton::clicked, this, &QSpinBox::stepDown);
        connect(increase_, &QPushButton::clicked, this, &QSpinBox::stepUp);
        connect(this, &QSpinBox::valueChanged, this, [this] { updateSteps(); });
    }

    QSize sizeHint() const override {
        return QSpinBox::sizeHint().expandedTo(QSize(property("externalSteps").toBool() ? 56 : 128, 36));
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QSpinBox::resizeEvent(event);
        const int height = std::min(26, std::max(1, this->height() - 8));
        decrease_->setGeometry(width() - 62, (this->height() - height) / 2, 26, height);
        increase_->setGeometry(width() - 32, (this->height() - height) / 2, 26, height);
    }

    bool event(QEvent* event) override {
        if (decrease_ && (event->type() == QEvent::Paint || event->type() == QEvent::EnabledChange))
            updateSteps();
        return QSpinBox::event(event);
    }

    void wheelEvent(QWheelEvent* event) override { scrollOutsideInput(this, event); }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == lineEdit() && event->type() == QEvent::Wheel) {
            scrollOutsideInput(this, static_cast<QWheelEvent*>(event));
            return true;
        }
        return QSpinBox::eventFilter(watched, event);
    }

  private:
    void updateSteps() {
        const bool external = property("externalSteps").toBool();
        decrease_->setVisible(!external);
        increase_->setVisible(!external);
        decrease_->setEnabled(isEnabled() && !isReadOnly() && value() > minimum());
        increase_->setEnabled(isEnabled() && !isReadOnly() && value() < maximum());
        decrease_->setAccessibleName(accessibleName() + QStringLiteral("减少"));
        increase_->setAccessibleName(accessibleName() + QStringLiteral("增加"));
    }
    QPushButton* decrease_ = nullptr;
    QPushButton* increase_ = nullptr;
};

class GlassSurface final : public QWidget {
  public:
    explicit GlassSurface(QWidget* parent) : QWidget(parent) {
        setObjectName("glassSurface");
        setAutoFillBackground(false);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        const bool nativeGlass = window()->property("systemGlassActive").toBool();
        painter.fillRect(rect(), QColor(208, 228, 255, nativeGlass ? 126 : 255));
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        // 只绘制弥散背景和细纹理，不对文字或整棵控件树执行模糊。
        const auto glow = [&](QPointF center, qreal radius, QColor color) {
            if (nativeGlass)
                color.setAlpha(color.alpha() / 2);
            QRadialGradient gradient(center, radius);
            gradient.setColorAt(0, color);
            color.setAlpha(0);
            gradient.setColorAt(1, color);
            painter.fillRect(rect(), gradient);
        };
        glow({width() * .08, height() * .18}, width() * .8, QColor(123, 181, 254, 100));
        glow({width() * .88, height() * .15}, width() * .7, QColor(206, 200, 255, 115));
        glow({width() * .74, height() * .85}, width() * .65, QColor(151, 189, 252, 105));
        glow({width() * .38, height() * .45}, width() * .55, QColor(255, 255, 255, 135));
        static const QImage grain = [] {
            QImage image(96, 96, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            quint32 seed = 0x759ac13;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    seed ^= seed << 13;
                    seed ^= seed >> 17;
                    seed ^= seed << 5;
                    image.setPixelColor(x, y, seed & 1 ? QColor(255, 255, 255, 3)
                                                       : QColor(76, 111, 172, 1));
                }
            return image;
        }();
        painter.fillRect(rect(), QBrush(grain));
    }
};

class CaptionButton final : public QPushButton {
  public:
    enum Kind { Minimize, Maximize, Close };
    CaptionButton(Kind kind, QWidget* target, QWidget* parent)
        : QPushButton(parent), kind_(kind), target_(target) {
        setFixedSize(46, 32);
        setAutoDefault(false);
        setDefault(false);
        setFocusPolicy(Qt::NoFocus);
        setProperty("captionButton", true);
        setObjectName(kind == Minimize ? "windowMinimize" : kind == Maximize ? "windowMaximize" : "windowClose");
        setAccessibleName(kind == Minimize ? QStringLiteral("最小化") :
                          kind == Maximize ? QStringLiteral("最大化或还原") : QStringLiteral("关闭窗口"));
        setToolTip(accessibleName());
        connect(this, &QPushButton::clicked, this, [this] {
            if (kind_ == Minimize)
                target_->showMinimized();
            else if (kind_ == Maximize)
                target_->isMaximized() ? target_->showNormal() : target_->showMaximized();
            else
                target_->close();
        });
        target->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == target_ && event->type() == QEvent::WindowStateChange) {
            setToolTip(kind_ == Maximize && target_->isMaximized() ? QStringLiteral("还原") : accessibleName());
            update();
        }
        return QPushButton::eventFilter(watched, event);
    }
    void paintEvent(QPaintEvent* event) override {
        QPushButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(kind_ == Close && underMouse() ? Qt::white : QColor("#28476f"), 1.15));
        const QRectF glyph((width() - 10) / 2., (height() - 10) / 2., 10, 10);
        if (kind_ == Minimize)
            painter.drawLine(glyph.bottomLeft() - QPointF(0, 3), glyph.bottomRight() - QPointF(0, 3));
        else if (kind_ == Maximize && target_->isMaximized()) {
            painter.drawRect(glyph.adjusted(2, 0, 0, -2));
            painter.fillRect(glyph.adjusted(0, 2, -2, 0), QColor(235, 243, 255, 220));
            painter.drawRect(glyph.adjusted(0, 2, -2, 0));
        } else if (kind_ == Maximize)
            painter.drawRect(glyph);
        else {
            painter.drawLine(glyph.topLeft(), glyph.bottomRight());
            painter.drawLine(glyph.topRight(), glyph.bottomLeft());
        }
    }

  private:
    Kind kind_;
    QWidget* target_;
};

class WindowChrome final : public QWidget {
  public:
    explicit WindowChrome(QWidget* target, bool dialog) : QWidget(target), target_(target), dialog_(dialog) {
        setObjectName("windowChrome");
        setAccessibleName(QStringLiteral("窗口标题栏"));
        setFixedHeight(34);
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(8, 0, 6, 0);
        row->setSpacing(0);
        if (dialog) {
            auto* title = new QLabel(target->windowTitle());
            title->setTextFormat(Qt::PlainText);
            title->setProperty("role", "dialogTitle");
            connect(target, &QWidget::windowTitleChanged, title, &QLabel::setText);
            row->addWidget(title);
        }
        row->addStretch();
        if (!dialog)
            for (auto kind : {CaptionButton::Minimize, CaptionButton::Maximize})
                row->addWidget(new CaptionButton(kind, target, this));
        row->addWidget(new CaptionButton(CaptionButton::Close, target, this));
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && target_->windowHandle()) {
            target_->windowHandle()->startSystemMove();
            event->accept();
        }
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (!dialog_ && event->button() == Qt::LeftButton) {
            target_->isMaximized() ? target_->showNormal() : target_->showMaximized();
            event->accept();
        }
    }

  private:
    QWidget* target_;
    bool dialog_;
};

class ScrollingSlider final : public QSlider {
  public:
    ScrollingSlider(Qt::Orientation orientation, QWidget* parent) : QSlider(orientation, parent) {
        setFocusPolicy(Qt::StrongFocus);
        // 原生 sizeHint 可只有 15px；给圆形手柄及抗锯齿边缘留出空间。
        if (orientation == Qt::Horizontal)
            setMinimumHeight(32);
    }

  protected:
    void wheelEvent(QWheelEvent* event) override { scrollOutsideInput(this, event); }
};

void prepareFloatingSurface(QWidget* surface) {
    // 圆角必须应用到真正的顶层窗口；只装饰列表会留下默认外框和不透明角点。
    if (!surface->property("floatingSurfaceStyled").toBool()) {
        surface->setProperty("floatingSurfaceStyled", true);
        surface->setWindowFlag(Qt::FramelessWindowHint, true);
        surface->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        surface->setAttribute(Qt::WA_TranslucentBackground);
        surface->setAutoFillBackground(false);
    }
    // Qt Windows 11 样式在 polish 中另加右下偏移的绘制阴影，原生窗口标志无法关闭它。
    surface->setGraphicsEffect(nullptr);
}

class FloatingSurfaceFilter final : public QObject {
  public:
    explicit FloatingSurfaceFilter(QObject* parent) : QObject(parent) {}
  protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            if (auto* widget = qobject_cast<QWidget*>(object);
                widget && (widget->windowType() == Qt::ToolTip || qobject_cast<QMenu*>(widget)))
                prepareFloatingSurface(widget);
        }
        return false;
    }
};

void ensureFloatingSurfaceFilter() {
    static QPointer<FloatingSurfaceFilter> filter;
    if (qApp && !filter) {
        filter = new FloatingSurfaceFilter(qApp);
        qApp->installEventFilter(filter);
    }
}

class ThemedComboBox final : public QComboBox {
  public:
    explicit ThemedComboBox(QWidget* parent) : QComboBox(parent) {
        setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        setMinimumContentsLength(12);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::StrongFocus);
    }

    void showPopup() override {
        // 弹层可比闭合框更宽，不将长设备名反向传给卡片的最小尺寸。
        // 样式接管后再创建标准列表；Qt 默认专用列表会强制裁切超屏名称。
        if (!popup_ || view() != popup_) {
            popup_ = new QListView;
            popup_->setFrameShape(QFrame::NoFrame);
            popup_->setResizeMode(QListView::Adjust);
            popup_->viewport()->setAutoFillBackground(false);
            popup_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
            setView(popup_);
            popup_->viewport()->installEventFilter(this);
        }
        auto* container = popup_->window();
        container->ensurePolished();
        prepareFloatingSurface(container);
        if (popupSurface_ != container) {
            popupSurface_ = container;
            container->installEventFilter(this);
        }
        container->setObjectName("comboPopupSurface");
        container->setAttribute(Qt::WA_StyledBackground, false);
        if (auto* frame = qobject_cast<QFrame*>(container))
            frame->setFrameShape(QFrame::NoFrame);
        container->setContentsMargins(6, 6, 6, 6);
        popup_->setTextElideMode(Qt::ElideNone);
        popup_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        int contentWidth = 0;
        for (int row = 0; row < count(); ++row)
            contentWidth = std::max(contentWidth, fontMetrics().horizontalAdvance(itemText(row)) + 48);
        const int desired = std::max(width(), contentWidth);
        const int available = screen() ? std::max(1, screen()->availableGeometry().width() - 16) : desired;
        popup_->setMinimumWidth(std::min(desired, available));
        // Qt 菜单委托可能把行宽压回视口，超屏名称显式保留内容宽度才能横向浏览。
        popup_->setGridSize(contentWidth > available && count() > 0
                                ? QSize(contentWidth, popup_->sizeHintForRow(0))
                                : QSize());
        QToolTip::hideText();
        QComboBox::showPopup();
        // 使用最终视口尺寸重排，避免沿用默认 640px 宽度而出现多余横向滚动条。
        popup_->doItemsLayout();
    }

  protected:
    void wheelEvent(QWheelEvent* event) override { scrollOutsideInput(this, event); }

    void paintEvent(QPaintEvent*) override {
        QStyleOptionComboBox option;
        initStyleOption(&option);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool enabled = isEnabled();
        const bool hovered = option.state & QStyle::State_MouseOver;
        const QColor fill = !enabled ? QColor(228, 237, 251, 95)
                            : hasFocus() ? QColor(255, 255, 255, 135)
                            : hovered ? QColor(255, 255, 255, 115) : QColor(255, 255, 255, 80);
        const QColor rim = !enabled ? QColor(177, 197, 226, 120)
                           : hasFocus() ? QColor("#6b91ec") : QColor(176, 198, 233, 180);
        painter.setBrush(fill);
        painter.setPen(QPen(rim, 1));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 8, 8);
        const QRect arrow = style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                    QStyle::SC_ComboBoxArrow, this);
        fluentIcon(UiIcon::ChevronDown, QColor("#365781")).paint(
            &painter, QRect(arrow.center() - QPoint(8, 8), QSize(16, 16)), Qt::AlignCenter,
            enabled ? QIcon::Normal : QIcon::Disabled);
        if (currentIndex() < 0 && !placeholderText().isEmpty()) {
            option.palette.setBrush(QPalette::ButtonText, option.palette.placeholderText());
            option.currentText = placeholderText();
        }
        if (!isEditable()) {
            auto edit =
                style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
            if (!option.currentIcon.isNull()) {
                const QRect logicalIcon(edit.topLeft(), QSize(option.iconSize.width(), edit.height()));
                option.currentIcon.paint(&painter, QStyle::visualRect(option.direction, edit, logicalIcon),
                                         Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);
                if (option.direction == Qt::RightToLeft)
                    edit.adjust(0, 0, -option.iconSize.width() - 4, 0);
                else
                    edit.adjust(option.iconSize.width() + 4, 0, 0, 0);
            }
            option.currentText = fontMetrics().elidedText(option.currentText, Qt::ElideRight,
                                                          std::max(0, edit.width() - 4));
            painter.setFont(font());
            painter.setPen(!enabled ? QColor("#798ba5")
                           : currentIndex() < 0 ? QColor("#617695") : QColor("#1d345c"));
            painter.drawText(edit, QStyle::visualAlignment(option.direction, Qt::AlignLeft) | Qt::AlignVCenter,
                             option.currentText);
        }
    }

    bool event(QEvent* event) override {
        if (event->type() == QEvent::ToolTip) {
            const auto* help = static_cast<QHelpEvent*>(event);
            QToolTip::showText(help->globalPos(), plainTextToolTip(currentIndex() >= 0 ? currentText() : placeholderText()),
                               this, rect());
            return true;
        }
        return QComboBox::event(event);
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (popupSurface_ && watched == popupSurface_ && event->type() == QEvent::Paint) {
            QPainter painter(popupSurface_);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(popupSurface_->rect(), Qt::transparent);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor("#c0d1eb"), 1));
            painter.setBrush(QColor("#f1f6ff"));
            painter.drawRoundedRect(QRectF(popupSurface_->rect()).adjusted(.5, .5, -.5, -.5), 10, 10);
            return true;
        }
        if (popup_ && watched == popup_->viewport() && event->type() == QEvent::ToolTip) {
            const auto* help = static_cast<QHelpEvent*>(event);
            const auto index = popup_->indexAt(help->pos());
            if (index.isValid()) {
                QToolTip::showText(help->globalPos(), plainTextToolTip(index.data(Qt::DisplayRole).toString()),
                                   popup_->viewport(), popup_->visualRect(index));
                return true;
            }
        }
        return QComboBox::eventFilter(watched, event);
    }

  private:
    QPointer<QListView> popup_;
    QPointer<QWidget> popupSurface_;
};

class ThemedTreeWidget final : public QTreeWidget {
  public:
    using QTreeWidget::QTreeWidget;
  protected:
    void drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const override {
        // 保留 Qt 的展开命中区与键盘行为，仅替换 Windows 的虚线连接线和箭头绘制。
        if (!model()->hasChildren(index))
            return;
        const bool rtl = layoutDirection() == Qt::RightToLeft;
        const int centerX = rtl ? rect.left() + indentation() / 2 : rect.right() - indentation() / 2;
        painter->save();
        painter->translate(centerX, rect.center().y());
        if (!isExpanded(index))
            painter->rotate(rtl ? 90 : -90);
        fluentIcon(UiIcon::ChevronDown, QColor("#48618a")).paint(painter, QRect(-8, -8, 16, 16));
        painter->restore();
    }
};

} // namespace

// 保留原生窗口样式和 ARGB backing store，客户区覆盖标题栏；不使用 layered window。
bool applySystemGlass(QWidget* window) {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != "windows")
        return false;
    const HMODULE module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return false;
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    using ExtendFrame = HRESULT(WINAPI*)(HWND, const MARGINS*);
    using EnableBlur = HRESULT(WINAPI*)(HWND, const DWM_BLURBEHIND*);
    const auto setAttribute = reinterpret_cast<SetAttribute>(GetProcAddress(module, "DwmSetWindowAttribute"));
    const auto extendFrame = reinterpret_cast<ExtendFrame>(GetProcAddress(module, "DwmExtendFrameIntoClientArea"));
    const auto enableBlur = reinterpret_cast<EnableBlur>(GetProcAddress(module, "DwmEnableBlurBehindWindow"));
    bool applied = false;
    if (setAttribute && extendFrame) {
        const HWND handle = reinterpret_cast<HWND>(window->winId());
        const BOOL dark = FALSE;
        setAttribute(handle, 20, &dark, sizeof(dark));
        const int rounded = 2;
        setAttribute(handle, 33, &rounded, sizeof(rounded));
        const MARGINS margins{-1, -1, -1, -1};
        extendFrame(handle, &margins);
        // 可调透明度的窗口模糊：接口动态探测，兼容边界见 UI_GLASS.md。
        struct AccentPolicy { int state; DWORD flags; DWORD color; DWORD animation; };
        struct CompositionData { int attribute; void* data; SIZE_T size; };
        using SetComposition = BOOL(WINAPI*)(HWND, CompositionData*);
        const auto user32 = GetModuleHandleW(L"user32.dll");
        const auto setComposition = user32 ? reinterpret_cast<SetComposition>(
            GetProcAddress(user32, "SetWindowCompositionAttribute")) : nullptr;
        if (setComposition && enableBlur) {
            const int noBackdrop = 1;
            setAttribute(handle, 38, &noBackdrop, sizeof(noBackdrop));
            // 配合整窗 frame 扩展启用 GDI backing store 的 alpha 合成。
            const HRGN wholeWindow = CreateRectRgn(0, 0, -1, -1);
            DWM_BLURBEHIND blur{};
            blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
            blur.fEnable = TRUE;
            blur.hRgnBlur = wholeWindow;
            const HRESULT blurResult = enableBlur(handle, &blur);
            if (wholeWindow)
                DeleteObject(wholeWindow);
            AccentPolicy accent{3, 0, 0, 0};
            CompositionData data{19, &accent, sizeof(accent)};
            applied = SUCCEEDED(blurResult) && setComposition(handle, &data);
        }
        if (applied) {
            window->setProperty("glassCompositionMode", "accent-blur");
        } else {
            const int backdrop = 3; // DWMSBT_TRANSIENTWINDOW, Windows 11 22621+.
            applied = SUCCEEDED(setAttribute(handle, 38, &backdrop, sizeof(backdrop)));
            window->setProperty("glassCompositionMode", applied ? "system-acrylic" : "frosted-fallback");
        }
    }
    FreeLibrary(module);
    return applied;
#else
    Q_UNUSED(window);
    return false;
#endif
}

void initializeGlassFrame(QWidget* window) {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != "windows")
        return;
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    const LONG_PTR style = GetWindowLongPtrW(handle, GWL_STYLE);
    SetWindowLongPtrW(handle, GWL_STYLE, (style & ~WS_CAPTION) | WS_THICKFRAME);
    SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#else
    Q_UNUSED(window);
#endif
}

bool handleGlassFrame(QWidget* window, const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG")
        return false;
    const auto* native = static_cast<const MSG*>(message);
    if (!native)
        return false;
    if (native->message == WM_NCCALCSIZE && native->wParam) {
        if (IsZoomed(native->hwnd)) {
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(native->hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                reinterpret_cast<NCCALCSIZE_PARAMS*>(native->lParam)->rgrc[0] = monitor.rcWork;
        }
        *result = 0;
        return true;
    }
    if (native->message == WM_NCHITTEST) {
        RECT bounds{};
        GetWindowRect(native->hwnd, &bounds);
        const POINT global{GET_X_LPARAM(native->lParam), GET_Y_LPARAM(native->lParam)};
        const int border = (std::max)(5, static_cast<int>(std::lround(6 * window->devicePixelRatioF())));
        if (!IsZoomed(native->hwnd)) {
            const bool left = global.x < bounds.left + border;
            const bool right = global.x >= bounds.right - border;
            const bool top = global.y < bounds.top + border;
            const bool bottom = global.y >= bounds.bottom - border;
            if (top || bottom || left || right) {
                *result = top ? (left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP)
                             : bottom ? (left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM)
                             : left ? HTLEFT : HTRIGHT;
                return true;
            }
        }
        POINT client = global;
        ScreenToClient(native->hwnd, &client);
        const QPoint point(qRound(client.x / window->devicePixelRatioF()),
                           qRound(client.y / window->devicePixelRatioF()));
        if (auto* chrome = window->findChild<QWidget*>("windowChrome")) {
            const auto inChrome = chrome->mapFrom(window, point);
            if (chrome->rect().contains(inChrome)) {
                const auto* hit = chrome->childAt(inChrome);
                *result = qobject_cast<const QAbstractButton*>(hit) ? HTCLIENT : HTCAPTION;
                return true;
            }
        }
    }
#else
    Q_UNUSED(window);
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}

FrostedDialog::FrostedDialog(QWidget* parent) : QDialog(parent) {
    setProperty("glassDialog", true);
    if (QGuiApplication::platformName() == "windows")
        setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(glassStyleSheet() + comboBoxStyleSheet());
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    surface_ = createGlassSurface(this);
    auto* surfaceLayout = new QVBoxLayout(surface_);
    surfaceLayout->setContentsMargins(0, 0, 0, 0);
    surfaceLayout->setSpacing(0);
    surfaceLayout->addWidget(createWindowChrome(this, true));
    auto* content = new QWidget;
    contentLayout_ = new QVBoxLayout(content);
    contentLayout_->setContentsMargins(24, 16, 24, 24);
    contentLayout_->setSpacing(14);
    surfaceLayout->addWidget(content, 1);
    outer->addWidget(surface_);
}

void FrostedDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!frameReady_) {
        frameReady_ = true;
        initializeGlassFrame(this);
    }
    setProperty("systemGlassActive", applySystemGlass(this));
    surface_->update();
}

bool FrostedDialog::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (handleGlassFrame(this, eventType, message, result))
        return true;
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        const auto* native = static_cast<const MSG*>(message);
        if (native && (native->message == WM_DWMCOMPOSITIONCHANGED || native->message == WM_THEMECHANGED ||
                       native->message == WM_SETTINGCHANGE))
            QTimer::singleShot(0, this, [this] {
                setProperty("systemGlassActive", applySystemGlass(this));
                surface_->update();
            });
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}

bool confirmThemedAction(QWidget* parent, const QString& title, const QString& message) {
    FrostedDialog dialog(parent);
    dialog.setObjectName("confirmThemedAction");
    dialog.setWindowTitle(title);
    dialog.setMinimumWidth(440);
    auto* text = new QLabel(message);
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    dialog.contentLayout()->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No);
    buttons->button(QDialogButtonBox::Yes)->setText(QStringLiteral("确认"));
    buttons->button(QDialogButtonBox::No)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Yes)->setAutoDefault(false);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.contentLayout()->addWidget(buttons);
    buttons->button(QDialogButtonBox::No)->setDefault(true);
    buttons->button(QDialogButtonBox::No)->setFocus();
    return dialog.exec() == QDialog::Accepted;
}

void showThemedMessage(QWidget* parent, const QString& title, const QString& message) {
    FrostedDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setMinimumWidth(440);
    auto* text = new QLabel(message);
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    dialog.contentLayout()->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("知道了"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    dialog.contentLayout()->addWidget(buttons);
    dialog.exec();
}

QImage renderCompactPreview(const QSize& maximum, const std::array<CompactDeviceStatus, 4>& devices) {
    if (maximum.isEmpty())
        return {};
    const QSize size = QSize(360, 220).scaled(maximum.boundedTo(QSize(2048, 2048)), Qt::KeepAspectRatio);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient glass(0, 0, size.width(), size.height());
    glass.setColorAt(0, QColor("#d8eaff"));
    glass.setColorAt(.5, QColor("#edf4ff"));
    glass.setColorAt(1, QColor("#e1dfff"));
    painter.setBrush(glass);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(image.rect(), 12, 12);
    const int margin = std::clamp(qRound(size.width() * .025), 4, 10);
    const int gap = margin;
    const qreal width = (size.width() - margin * 2. - gap) / 2.;
    const qreal height = (size.height() - margin * 2. - gap) / 2.;
    const int padding = std::clamp(qRound(height * .1), 5, 12);
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    const std::array<QString, 4> shortLabels{QStringLiteral("鼠标"), QStringLiteral("键盘"),
                                            QStringLiteral("亮度"), QStringLiteral("音量")};
    for (int index = 0; index < 4; ++index) {
        const auto& device = devices[static_cast<std::size_t>(index)];
        const QRectF cell(margin + (index % 2) * (width + gap), margin + (index / 2) * (height + gap), width, height);
        painter.setPen(QPen(QColor(255, 255, 255, 220), 1));
        painter.setBrush(QColor(255, 255, 255, 150));
        painter.drawRoundedRect(cell, 9, 9);
        const int iconSize = std::clamp(qRound(height * .18), 12, 20);
        const QRect iconRect(qRound(cell.left()) + padding, qRound(cell.top()) + padding, iconSize, iconSize);
        fluentIcon(device.icon, QColor("#4376ef")).paint(&painter, iconRect);
        font.setPixelSize(std::clamp(qRound(height * .13), 10, 12));
        painter.setFont(font);
        painter.setPen(QColor("#365782"));
        const QRectF labelRect(iconRect.right() + 6, cell.top() + padding,
                                cell.right() - padding - iconRect.right() - 6, iconSize);
        const auto labelText = size.width() < 260 ? shortLabels[static_cast<std::size_t>(index)] : device.label;
        painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                           painter.fontMetrics().elidedText(labelText, Qt::ElideRight, qRound(labelRect.width())));
        const qreal valueTop = iconRect.bottom() + 5;
        font.setPixelSize(std::clamp(qRound(height * .32), 16, 27));
        painter.setFont(font);
        painter.setPen(QColor("#1c3b69"));
        const QRectF valueRect(cell.left() + padding, valueTop, width - padding * 2, font.pixelSize() + 6);
        painter.drawText(valueRect, Qt::AlignLeft | Qt::AlignVCenter, device.value);
        const int valueWidth = painter.fontMetrics().horizontalAdvance(device.value);
        font.setPixelSize(std::clamp(qRound(height * .12), 9, 11));
        painter.setFont(font);
        painter.setPen(QColor("#3e5678"));
        const QRectF detailRect(valueRect.left() + valueWidth + 6, valueTop + 5,
                                 valueRect.width() - valueWidth - 6, valueRect.height() - 5);
        if (detailRect.width() >= 20)
            painter.drawText(detailRect, Qt::AlignLeft | Qt::AlignVCenter,
                               painter.fontMetrics().elidedText(device.detail, Qt::ElideRight, qRound(detailRect.width())));
        bool numeric = false;
        const int percent = device.value.endsWith('%') ? device.value.chopped(1).toInt(&numeric) : 0;
        if (numeric && height >= 50) {
            const QRectF track(cell.left() + padding, cell.bottom() - padding - 4, width - padding * 2, 4);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(87, 132, 244, 35));
            painter.drawRoundedRect(track, 2, 2);
            painter.setBrush(QColor("#5784f4"));
            painter.drawRoundedRect(QRectF(track.topLeft(), QSizeF(track.width() * std::clamp(percent, 0, 100) / 100., 4)), 2, 2);
        }
    }
    return image;
}

bool enableCompactTaskbarPreview(QWidget* window) {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != "windows")
        return false;
    const auto module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return false;
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    const auto set = reinterpret_cast<SetAttribute>(GetProcAddress(module, "DwmSetWindowAttribute"));
    const BOOL enabled = TRUE;
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    const bool success = set && SUCCEEDED(set(handle, DWMWA_FORCE_ICONIC_REPRESENTATION, &enabled, sizeof(enabled))) &&
                         SUCCEEDED(set(handle, DWMWA_HAS_ICONIC_BITMAP, &enabled, sizeof(enabled)));
    if (!success && set) {
        const BOOL disabled = FALSE;
        set(handle, DWMWA_FORCE_ICONIC_REPRESENTATION, &disabled, sizeof(disabled));
    }
    FreeLibrary(module);
    return success;
#else
    Q_UNUSED(window);
    return false;
#endif
}

void invalidateCompactTaskbarPreview(QWidget* window) {
#ifdef Q_OS_WIN
    if (!window->property("compactTaskbarPreviewRequested").toBool())
        return;
    const auto module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module) {
        using Invalidate = HRESULT(WINAPI*)(HWND);
        const auto invalidate = reinterpret_cast<Invalidate>(GetProcAddress(module, "DwmInvalidateIconicBitmaps"));
        if (invalidate)
            invalidate(reinterpret_cast<HWND>(window->winId()));
        FreeLibrary(module);
    }
#else
    Q_UNUSED(window);
#endif
}

bool presentCompactTaskbarBitmap(QWidget* window, const QImage& source, bool livePreview) {
#ifdef Q_OS_WIN
    if (source.isNull())
        return false;
    const auto module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return false;
    const auto image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width();
    info.bmiHeader.biHeight = -image.height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    bool success = false;
    if (bitmap && pixels) {
        for (int row = 0; row < image.height(); ++row)
            std::memcpy(static_cast<unsigned char*>(pixels) + static_cast<std::size_t>(row) * image.width() * 4,
                        image.constScanLine(row), static_cast<std::size_t>(image.width()) * 4);
        if (livePreview) {
            using Present = HRESULT(WINAPI*)(HWND, HBITMAP, POINT*, DWORD);
            const auto present = reinterpret_cast<Present>(GetProcAddress(module, "DwmSetIconicLivePreviewBitmap"));
            POINT origin{};
            success = present && SUCCEEDED(present(reinterpret_cast<HWND>(window->winId()), bitmap, &origin, 0));
        } else {
            using Present = HRESULT(WINAPI*)(HWND, HBITMAP, DWORD);
            const auto present = reinterpret_cast<Present>(GetProcAddress(module, "DwmSetIconicThumbnail"));
            success = present && SUCCEEDED(present(reinterpret_cast<HWND>(window->winId()), bitmap, 0));
        }
    }
    if (bitmap)
        DeleteObject(bitmap);
    FreeLibrary(module);
    return success;
#else
    Q_UNUSED(window);
    Q_UNUSED(source);
    Q_UNUSED(livePreview);
    return false;
#endif
}

QIcon fluentIcon(UiIcon icon, const QColor& color, const QColor& checkedColor) {
    static constexpr std::array names{
        "apps", "save_multiple", "cursor", "desktop", "keyboard", "headphones",
        "brightness_high", "speaker_2", "speaker_mute", "mic", "mic_off", "arrow_sync",
        "text_bullet_list_ltr", "link", "more_horizontal", "chevron_up", "chevron_down", "arrow_exit", "info", "settings"};
    const auto index = static_cast<std::size_t>(icon);
    if (index >= names.size())
        return {};
    const auto selectedColor = checkedColor.isValid() ? checkedColor : color;
    const QString key = QString::number(static_cast<int>(icon)) + color.name(QColor::HexArgb) +
                        selectedColor.name(QColor::HexArgb);
    // Called only by GUI widgets. Reuse tinted pixmaps across regular state refreshes.
    static QHash<QString, QIcon> cache;
    if (const auto found = cache.constFind(key); found != cache.cend())
        return *found;
    QIcon result;
    for (const auto state : {QIcon::Off, QIcon::On}) {
        const QString path = QString(":/peripheral/fluent/ic_fluent_%1_24_%2.png")
                                 .arg(names[index], state == QIcon::On ? "filled" : "regular");
        const QImage source(path);
        if (source.isNull()) {
            qWarning() << "Missing Fluent icon resource:" << path;
            return {};
        }
        for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled}) {
            QImage tinted = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QPainter painter(&tinted);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(tinted.rect(), mode == QIcon::Disabled ? QColor("#a3adbb")
                                           : state == QIcon::On ? selectedColor : color);
            painter.end();
            result.addPixmap(QPixmap::fromImage(tinted), mode, state);
        }
    }
    cache.insert(key, result);
    return result;
}

QComboBox* createThemedComboBox(QWidget* parent) {
    return new ThemedComboBox(parent);
}

QTreeWidget* createThemedTreeWidget(QWidget* parent) {
    return new ThemedTreeWidget(parent);
}

QWidget* createGlassSurface(QWidget* parent) {
    return new GlassSurface(parent);
}

QIcon applicationIcon() {
    static const QIcon icon = [] {
        QIcon result;
        for (const int size : {16, 20, 24, 32, 40, 48, 64, 128, 256, 512})
            result.addFile(QString(":/peripheral/app/peripheral-hub-%1.png").arg(size), QSize(size, size));
        return result;
    }();
    return icon;
}

QWidget* createWindowChrome(QWidget* window, bool dialog) {
    return new WindowChrome(window, dialog);
}

QString glassStyleSheet() {
    ensureFloatingSurfaceFilter();
    QFile file(":/peripheral/glass.qss");
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Missing glass theme resource";
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString plainTextToolTip(const QString& text) {
    return Qt::mightBeRichText(text) ? Qt::convertFromPlainText(text) : text;
}

QSpinBox* createThemedSpinBox(QWidget* parent) {
    return new ScrollingSpinBox(parent);
}

QSlider* createThemedSlider(Qt::Orientation orientation, QWidget* parent) {
    return new ScrollingSlider(orientation, parent);
}

QString comboBoxStyleSheet() {
    return QStringLiteral(R"(
        QComboBox {
            background:transparent; color:#1d345c;
            border:0;
            padding:8px 34px 8px 14px; min-height:27px;
            combobox-popup:0;
        }
        QComboBox:hover, QComboBox:focus { background:transparent; }
        QComboBox:disabled { background:transparent; color:#798ba5; }
        QComboBox::drop-down { border:0; width:26px; }
        QComboBox::down-arrow {
            image:url(:/peripheral/fluent/ic_fluent_chevron_down_24_regular.png); width:16px; height:16px;
        }
        QComboBox QAbstractItemView {
            background:transparent; color:#27364c;
            border:0; padding:0; outline:0;
            selection-background-color:#e4edff; selection-color:#253f68;
        }
        QComboBox QAbstractItemView::item {
            background:transparent; border:0; border-radius:5px;
            padding:7px 10px; min-height:20px; margin:1px 0;
        }
        QComboBox QAbstractItemView::item:hover { background:#f1f5fe; }
        QComboBox QAbstractItemView::item:selected { background:#e4edff; color:#253f68; }
        QComboBox QAbstractItemView::item:selected:hover { background:#dce8ff; }
        QComboBox QAbstractItemView::item:disabled { color:#9aa8bc; background:transparent; }
        QComboBox QAbstractItemView QScrollBar:vertical {
            background:transparent; width:8px; margin:2px 0;
        }
        QComboBox QAbstractItemView QScrollBar::handle:vertical {
            background:#d2ddf0; border-radius:4px; min-height:24px;
        }
        QComboBox QAbstractItemView QScrollBar::handle:vertical:hover { background:#a6bbdf; }
        QComboBox QAbstractItemView QScrollBar::add-line:vertical,
        QComboBox QAbstractItemView QScrollBar::sub-line:vertical { height:0; }
        QComboBox QAbstractItemView QScrollBar::add-page:vertical,
        QComboBox QAbstractItemView QScrollBar::sub-page:vertical { background:transparent; }
        QComboBox QAbstractItemView QScrollBar:horizontal { background:transparent; height:8px; margin:0 2px; }
        QComboBox QAbstractItemView QScrollBar::handle:horizontal { background:#d2ddf0; border-radius:4px; min-width:24px; }
        QComboBox QAbstractItemView QScrollBar::handle:horizontal:hover { background:#a6bbdf; }
        QComboBox QAbstractItemView QScrollBar::add-line:horizontal,
        QComboBox QAbstractItemView QScrollBar::sub-line:horizontal { width:0; }
        QComboBox QAbstractItemView QScrollBar::add-page:horizontal,
        QComboBox QAbstractItemView QScrollBar::sub-page:horizontal { background:transparent; }
    )");
}

} // namespace peripheral::app
