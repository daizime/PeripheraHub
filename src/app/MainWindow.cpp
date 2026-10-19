#include "MainWindow.h"
#include "widgets/DeviceCard.h"
#include "widgets/PercentDetails.h"
#include "widgets/MonitorControls.h"
#include "widgets/DeviceGrid.h"
#include "widgets/UiTheme.h"
#include "widgets/SettingsPage.h"
#include "widgets/HeadsetDetails.h"
#include "widgets/MouseDetails.h"
#include "core/hid_discovery.h"
#include "BindingSelection.h"
#include "ProfileTracker.h"
#include <windows.h>
#include <windowsx.h>
// Win32 同名 API 宏会改写核心层的 DeviceCapabilities 类型名。
#undef DeviceCapabilities
#include <dwmapi.h>
#include <QApplication>
#include <QAbstractItemView>
#include <QAction>
#include <QKeySequence>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDesktopServices>
#include <QEvent>
#include <QFileInfo>
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QIcon>
#include <QImage>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QStackedWidget>
#include <QSplitter>
#include <QButtonGroup>
#include <QTimeZone>
#include <QWidgetAction>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QResizeEvent>
#include <QSlider>
#include <QScreen>
#include <QWindow>
#include <QStyle>
#include <QVBoxLayout>
#include <QUuid>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace peripheral::app {
namespace {
QString qs(const std::wstring& text) {
    return QString::fromStdWString(text);
}
QString qs(const std::string& text) {
    return QString::fromStdString(text);
}
qint64 monotonicNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// 双箭头在方形画布内严格居中；展开指向左，收起指向右。
QIcon sidebarChevron(bool collapsed) {
    constexpr int size = 64;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor("#45638e"));
    pen.setWidthF(5.0);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    const auto chevron = [&painter](qreal left, bool right) {
        QPainterPath path;
        if (right) {
            path.moveTo(left, 20);
            path.lineTo(left + 12, 32);
            path.lineTo(left, 44);
        } else {
            path.moveTo(left + 12, 20);
            path.lineTo(left, 32);
            path.lineTo(left + 12, 44);
        }
        painter.drawPath(path);
    };
    chevron(14, collapsed);
    chevron(38, collapsed);
    return QIcon(pixmap);
}
QString categoryText(const QString& key) {
    const QMap<QString, QString> names{
        {"mouse", "鼠标"}, {"keyboard", "键盘"}, {"audio", "音频设备"}, {"monitor", "显示器"}};
    return names.value(key, "其他设备");
}
constexpr int profileIdRole = Qt::UserRole;
constexpr int profileGroupRole = Qt::UserRole + 1;
QString audioDirectionText(const QString& direction) {
    return direction == "output" ? "输出设备" : (direction == "input" ? "输入设备" : "方向待识别");
}
QString profileGroup(const SavedDeviceProfile& profile) {
    if (profile.category == "audio")
        return "audio/" + (profile.direction == "output" || profile.direction == "input"
                               ? profile.direction
                               : QString("unknown"));
    return profile.category == "mouse" || profile.category == "keyboard" || profile.category == "monitor"
               ? profile.category
               : QString("other");
}
void placeTreeItem(QTreeWidget* tree, QTreeWidgetItem* item, QTreeWidgetItem* parent, int row) {
    const int current = parent ? parent->indexOfChild(item) : tree->indexOfTopLevelItem(item);
    if (item->treeWidget() == tree && item->parent() == parent && current == row)
        return;
    if (auto* previous = item->parent())
        previous->takeChild(previous->indexOfChild(item));
    else if (auto* previousTree = item->treeWidget())
        previousTree->takeTopLevelItem(previousTree->indexOfTopLevelItem(item));
    if (parent)
        parent->insertChild(row, item);
    else
        tree->insertTopLevelItem(row, item);
}
bool categoryReadable(const QString& category, const RuntimeSnapshot& snapshot) {
    if (category == "audio")
        return snapshot.audioEnumerationSucceeded;
    if (category == "monitor")
        return snapshot.monitorEnumerationSucceeded;
    return snapshot.hidEnumerationSucceeded;
}
QLabel* label(const QString& text, const char* role = "body") {
    auto* value = new QLabel(text);
    value->setTextFormat(Qt::PlainText);
    value->setProperty("role", role);
    value->setWordWrap(true);
    return value;
}
UiIcon categoryIcon(const QString& key) {
    if (key == "mouse")
        return UiIcon::Cursor;
    if (key == "keyboard")
        return UiIcon::Keyboard;
    if (key == "monitor")
        return UiIcon::Desktop;
    if (key == "audio/input")
        return UiIcon::Mic;
    if (key == "audio/output")
        return UiIcon::Speaker;
    if (key == "audio" || key.startsWith("audio/"))
        return UiIcon::Headphones;
    return UiIcon::Apps;
}
QLabel* glyph(UiIcon icon) {
    auto* result = new QLabel;
    result->setFixedSize(20, 20);
    result->setAlignment(Qt::AlignCenter);
    result->setScaledContents(true);
    result->setPixmap(fluentIcon(icon).pixmap(QSize(96, 96)));
    return result;
}
QString transportText(const DeviceSnapshot& d) {
    if (d.wiredPresent && d.receiverPresent)
        return "有线与 2.4G 接口均可见";
    if (d.wiredPresent)
        return "USB 有线接口";
    if (d.receiverPresent)
        return "2.4G 接收器";
    return d.endpoints.empty() ? "接口已移除" : "HID 接口可见";
}
QString linkText(LinkState state) {
    switch (state) {
    case LinkState::Online:
        return "设备已响应";
    case LinkState::Offline:
        return "设备报告离线";
    case LinkState::TransportLost:
        return "连接已移除";
    case LinkState::Reconnecting:
        return "正在重新连接";
    default:
        return "尚未确认设备在线";
    }
}
DeviceCard* card(DeviceGrid* grid, const QString& category, const QString& name, QVBoxLayout** body) {
    auto* frame = new DeviceCard(category, name);
    *body = frame->quickLayout();
    grid->addCard(frame);
    return frame;
}
QString availablePreference(const QList<QPair<QString, QString>>& items, const QStringList& preferences) {
    for (const auto& id : preferences)
        if (!id.isEmpty() &&
            std::any_of(items.begin(), items.end(), [&id](const auto& item) { return item.second == id; }))
            return id;
    return {};
}
void repopulate(QComboBox* combo, const QList<QPair<QString, QString>>& items, const QString& preferred = {},
                bool autoSingle = true) {
    // 用户正在展开列表时不重建模型：否则 3 秒一次的扫描会关闭弹层、
    // 重置滚动位置，并把鼠标悬停/键盘导航打断。
    if (const auto* view = combo->view(); view && view->isVisible())
        return;
    const QString current = combo->currentData().toString();
    const QSignalBlocker block(combo);
    bool same = combo->count() == items.size();
    if (same) {
        for (int index = 0; index < items.size(); ++index) {
            if (combo->itemText(index) != items.at(index).first ||
                combo->itemData(index).toString() != items.at(index).second) {
                same = false;
                break;
            }
        }
    }
    if (!same) {
        combo->clear();
        for (const auto& item : items)
            combo->addItem(item.first, item.second);
    }
    int index = combo->findData(current);
    if (index < 0 && !preferred.isEmpty())
        index = combo->findData(preferred);
    if (index < 0 && autoSingle && items.size() == 1)
        index = 0;
    if (combo->currentIndex() != index)
        combo->setCurrentIndex(index);
}
} // namespace

MainWindow::MainWindow(DeviceService* service, const QString& dataDirectory, QWidget* parent)
    : QMainWindow(parent), service_(service),
      settings_(dataDirectory + "/settings.ini", QSettings::IniFormat),
      appSettings_(dataDirectory + "/settings.ini"),
      profiles_(dataDirectory + "/devices.json") {
    QString settingsWarning;
    preferences_ = appSettings_.load(&settingsWarning);
    profiles_.load();
    if (QGuiApplication::platformName() == "windows")
        setAttribute(Qt::WA_TranslucentBackground);
    bindings_ = settings_.value("mouse/boundEndpoints").toStringList();
    setWindowTitle("外设中心 · Peripheral Hub · 0.0 初始预览版");
    setWindowIcon(applicationIcon());
    // 设计最小尺寸在 100% 缩放下合适；高 DPI/小屏时按实际工作区收缩，
    // 否则窗口会被强制撑出屏幕底部，页脚和对话框按钮无法到达。
    // 设计最小尺寸在 100% 缩放下合适；高 DPI/小屏时按实际工作区收缩。
    // 运行中切换 DPI/显示器时还会在 changeEvent 中重新计算，不能沿用启动时的逻辑尺寸。
    QSize initialSize(1440, 940);
    if (const auto* display = screen())
        initialSize = initialSize.boundedTo(display->availableGeometry().size());
    updateWindowConstraints();
    resize(initialSize.expandedTo(minimumSize()));
    buildUi();
    sidebarCollapsed_ = settings_.value("ui/sidebarCollapsed", false).toBool();
    setSidebarCollapsed(sidebarCollapsed_);
    profileTracker_ = new ProfileTracker(service_, &profiles_, this);
    applyPreferences();
    if (!settingsWarning.isEmpty())
        settingsPage_->setSaveStatus(settingsWarning, false);
    connect(profileTracker_, &ProfileTracker::changed, this, [this] {
        if (pages_->currentIndex() == 1)
            updateSavedProfilesView();
    });
    connect(profileTracker_, &ProfileTracker::statusMessage, this, [this](const QString& message) {
        if (!manualRefreshTicket_)
            notice_->setText(message);
    });
    connect(monitorSlider_, &QSlider::sliderReleased, profileTracker_, &ProfileTracker::flush);
    connect(audioSlider_, &QSlider::sliderReleased, profileTracker_, &ProfileTracker::flush);
    connect(audioInputSlider_, &QSlider::sliderReleased, profileTracker_, &ProfileTracker::flush);
    setupTray();
    connect(service_, &DeviceService::refreshed, this, &MainWindow::updateDevices);
    connect(service_, &DeviceService::operationFinished, this, &MainWindow::showOperation);
    connect(service_, &DeviceService::refreshFinished, this,
            [this](quint64 ticket, bool, const QString& message) {
                if (manualRefreshTicket_ && ticket == manualRefreshTicket_) {
                    manualRefreshTicket_ = 0;
                    manualRefreshMessageUntil_ = QDateTime::currentMSecsSinceEpoch() + 6000;
                    refreshButton_->setEnabled(true);
                    refreshButton_->setText("刷新状态");
                    notice_->setText(message);
                    notice_->setToolTip(plainTextToolTip(message));
                }
                finishProfileDeletion(ticket);
            });
    connect(service_, &DeviceService::monitorControlsChanged, this, &MainWindow::updateMonitorControls);
    connect(service_, &DeviceService::monitorFeatureProgress, this,
            [this](const QString& message) { monitorControls_->setOperationStatus(message); });
    connect(service_, &DeviceService::monitorFeatureFinished, this,
            [this](const QString& message) { monitorControls_->setOperationStatus(message, true); });
    // 显示拓扑/USB通知合并后重新扫描，轮询继续作为驱动漏报时的后备。
    displayChangeTimer_.setSingleShot(true);
    displayChangeTimer_.setInterval(750);
    connect(&displayChangeTimer_, &QTimer::timeout, this, [this] { service_->refresh(true); });
    // 休眠唤醒后设备需要重新枚举：HID 接收器与音频端点会短暂消失，DDC 也可能
    // 返回旧的拓扑。等待一段时间再强制整轮扫描，避免在恢复瞬间读到半就绪状态。
    resumeTimer_.setSingleShot(true);
    resumeTimer_.setInterval(1500);
    connect(&resumeTimer_, &QTimer::timeout, this, [this] {
        qInfo() << "System resume: forcing a full device refresh";
        resumeRefreshStartedMs_ = QDateTime::currentMSecsSinceEpoch();
        service_->refresh(true);
        if (monitorControls_)
            monitorControls_->setOperationStatus(QStringLiteral("系统已唤醒，正在重新读取设备"));
    });
    // 记录唤醒后第一次整轮扫描的耗时与各子系统结果，作为可靠性证据；
    // 手动刷新、连接通知不会写这条日志。
    connect(service_, &DeviceService::refreshed, this, [this] {
        if (!resumeRefreshStartedMs_)
            return;
        const auto elapsedMs = QDateTime::currentMSecsSinceEpoch() - resumeRefreshStartedMs_;
        resumeRefreshStartedMs_ = 0;
        const auto& snapshot = service_->snapshot();
        qInfo() << "Resume refresh completed in" << elapsedMs << "ms"
                << "usb" << snapshot.hidEnumerationSucceeded
                << "audio" << snapshot.audioEnumerationSucceeded
                << "monitor" << snapshot.monitorEnumerationSucceeded
                << "errors" << snapshot.errors.size();
    });
    taskbarPreviewTimer_.setSingleShot(true);
    taskbarPreviewTimer_.setInterval(250);
    connect(&taskbarPreviewTimer_, &QTimer::timeout, this, [this] { invalidateCompactTaskbarPreview(this); });
}

void MainWindow::buildUi() {
    setStyleSheet(glassStyleSheet() + comboBoxStyleSheet());
    auto* central = createGlassSurface();
    auto* windowLayout = new QHBoxLayout(central);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    windowLayout->setSpacing(0);
    sidebar_ = new QFrame;
    sidebar_->setObjectName("sidebar");
    sidebar_->setFixedWidth(230);
    auto* side = new QVBoxLayout(sidebar_);
    side->setContentsMargins(18, 18, 18, 36);
    side->setSpacing(12);
    sidebarToggle_ = new QToolButton;
    sidebarToggle_->setObjectName("sidebarToggle");
    sidebarToggle_->setFixedSize(34, 34);
    sidebarToggle_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    sidebarToggle_->setAutoRaise(true);
    sidebarToggle_->setIcon(sidebarChevron(false));
    sidebarToggle_->setIconSize(QSize(20, 20));
    sidebarToggle_->setCursor(Qt::PointingHandCursor);
    sidebarToggle_->setToolTip("收起侧栏");
    sidebarToggle_->setAccessibleName("收起侧栏");
    toggleRow_ = new QHBoxLayout;
    toggleRow_->setContentsMargins(0, 0, 0, 0);
    toggleRow_->addStretch(1);
    toggleRow_->addWidget(sidebarToggle_);
    toggleRow_->addStretch(0);
    side->addLayout(toggleRow_);
    brandLayout_ = new QHBoxLayout;
    brandLayout_->setContentsMargins(16, 0, 0, 0);
    brandLayout_->setSpacing(12);
    brandIcon_ = new QLabel;
    brandIcon_->setFixedSize(32, 32);
    brandIcon_->setScaledContents(true);
    brandIcon_->setPixmap(applicationIcon().pixmap(QSize(96, 96)));
    brandLayout_->addWidget(brandIcon_);
    brandName_ = label("外设中心", "brand");
    brandName_->setToolTip("初始预览版 · " + QCoreApplication::applicationVersion());
    brandLayout_->addWidget(brandName_);
    side->addLayout(brandLayout_);
    brandTagline_ = label("PERIPHERAL HUB", "muted");
    brandTagline_->setObjectName("brandTagline");
    brandTagline_->setWordWrap(false);
    brandTagline_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    brandTagline_->setContentsMargins(64, 0, 0, 0);
    side->addWidget(brandTagline_);
    side->addSpacing(18);
    sidebarSeparator_ = new QFrame;
    sidebarSeparator_->setObjectName("sidebarSeparator");
    sidebarSeparator_->setFixedHeight(1);
    side->addWidget(sidebarSeparator_);
    side->addSpacing(6);
    auto* navigation = new QButtonGroup(this);
    auto* overview = new QPushButton("设备总览");
    overview->setObjectName("overviewNavigation");
    auto* saved = new QPushButton("已保存设备");
    saved->setObjectName("savedNavigation");
    settingsNavigation_ = new QPushButton("设置");
    settingsNavigation_->setObjectName("settingsNavigation");
    settingsNavigation_->setIcon(fluentIcon(UiIcon::Settings, QColor("#45638e"), QColor("#2d5fae")));
    overview->setIcon(fluentIcon(UiIcon::Apps, QColor("#45638e"), QColor("#2d5fae")));
    saved->setIcon(fluentIcon(UiIcon::SaveMultiple, QColor("#45638e"), QColor("#2d5fae")));
    for (auto* button : {overview, saved, settingsNavigation_}) {
        button->setCheckable(true);
        button->setProperty("navigation", true);
        button->setProperty("navigationText", button->text());
        button->setIconSize(QSize(26, 26));
        navigation->addButton(button);
        side->addWidget(button);
    }
    overview->setChecked(true);
    connect(overview, &QPushButton::clicked, this, [this] { pages_->setCurrentIndex(0); });
    connect(saved, &QPushButton::clicked, this, [this] {
        updateSavedProfilesView();
        pages_->setCurrentIndex(1);
    });
    connect(settingsNavigation_, &QPushButton::clicked, this, [this] { pages_->setCurrentIndex(2); });
    side->addStretch();
    connectionPanel_ = new QWidget;
    auto* connection = new QHBoxLayout(connectionPanel_);
    connection->setContentsMargins(16, 0, 0, 0);
    connection->setSpacing(14);
    connection->addWidget(glyph(UiIcon::Link), 0, Qt::AlignTop);
    auto* connectionText = new QVBoxLayout;
    connectionText->setSpacing(6);
    connectionText->addWidget(label("有线 / 2.4G", "controlLabel"));
    connectionText->addWidget(label("随连接自动更新", "muted"));
    connection->addLayout(connectionText, 1);
    side->addWidget(connectionPanel_);
    windowLayout->addWidget(sidebar_);
    auto* bodySurface = new QWidget;
    windowLayout->addWidget(bodySurface, 1);
    auto* root = new QVBoxLayout(bodySurface);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(createWindowChrome(this));
    auto* page = new QWidget;
    page->setObjectName("overviewPage");
    auto* content = new QVBoxLayout(page);
    content->setContentsMargins(38, 18, 30, 10);
    content->setSpacing(18);
    auto* top = new QHBoxLayout;
    auto* titles = new QVBoxLayout;
    titles->setSpacing(7);
    titles->addWidget(label("设备总览", "title"));
    auto* subtitle = label("查看状态，管理每一次连接。", "muted");
    subtitle->setWordWrap(false);
    subtitle->setMinimumWidth(0);
    titles->addWidget(subtitle);
    top->addLayout(titles);
    top->addStretch();
    auto* detail = new QPushButton("设备详情");
    detail->setIcon(fluentIcon(UiIcon::Details));
    detail->setIconSize(QSize(22, 22));
    detail->setProperty("toolbarAction", true);
    connect(detail, &QPushButton::clicked, this, &MainWindow::showDetails);
    refreshButton_ = new QPushButton("刷新状态");
    refreshButton_->setObjectName("primary");
    refreshButton_->setIcon(fluentIcon(UiIcon::Refresh, Qt::white));
    refreshButton_->setIconSize(QSize(22, 22));
    refreshButton_->setProperty("toolbarAction", true);
    refreshButton_->setAccessibleName("刷新设备状态");
    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::requestManualRefresh);
    top->addWidget(detail);
    top->addWidget(refreshButton_);
    content->addLayout(top);
    auto* noticeSurface = new QFrame;
    noticeSurface->setObjectName("notice");
    noticeSurface->setMinimumHeight(50);
    auto* noticeRow = new QHBoxLayout(noticeSurface);
    noticeRow->setContentsMargins(18, 10, 18, 10);
    noticeRow->setSpacing(16);
    auto* infoSymbol = glyph(UiIcon::Info);
    infoSymbol->setPixmap(fluentIcon(UiIcon::Info, QColor("#326fff")).pixmap(QSize(24, 24)));
    infoSymbol->setAccessibleName("信息");
    noticeRow->addWidget(infoSymbol);
    notice_ = label("正在识别设备。电量与连接状态以实际读取结果为准。", "noticeText");
    notice_->setObjectName("noticeText");
    noticeRow->addWidget(notice_, 1);
    content->addWidget(noticeSurface);
    deviceGrid_ = new DeviceGrid;
    deviceGrid_->setScanning(true);
    auto* grid = deviceGrid_;
    QVBoxLayout* body;
    mouseCard_ = card(grid, "鼠标  /  MOUSE", "等待识别鼠标", &body);
    mouseCard_->setCardId("mouse");
    mouseCard_->setCategoryIcon(fluentIcon(UiIcon::Cursor, QColor("#4376ef")));
    mouseSelector_ = createThemedComboBox();
    mouseSelector_->setPlaceholderText("等待发现鼠标");
    body->addWidget(mouseSelector_);
    body->addSpacing(20);
    connect(mouseSelector_, &QComboBox::currentIndexChanged, this, [this] {
        const auto selected = mouseSelector_->currentData().toString();
        if (!selected.isEmpty()) {
            settings_.setValue("mouse/selectedId", selected);
            settings_.sync();
        }
        updateMouse();
    });
    auto* mouseMetric = new QHBoxLayout;
    mouseValue_ = label("—", "metric");
    mouseState_ = label("尚无有效电量", "status");
    mouseMetric->addWidget(mouseValue_);
    mouseMetric->addStretch();
    mouseMetric->addWidget(mouseState_);
    body->addLayout(mouseMetric);
    mouseBattery_ = new QProgressBar;
    mouseBattery_->setRange(0, 100);
    mouseBattery_->setTextVisible(false);
    mouseBattery_->hide();
    body->addWidget(mouseBattery_);
    mouseConnection_ = label("等待 USB 接口", "muted");
    mouseFreshness_ = label("接收器存在与鼠标在线会分别确认", "muted");
    body->addWidget(mouseConnection_);
    body->addWidget(mouseFreshness_);
    mouseConfiguration_ = label({}, "status");
    mouseConfiguration_->setObjectName("mouseConfigurationSummary");
    mouseConfiguration_->hide();
    body->addWidget(mouseConfiguration_);
    body->addStretch();
    auto* more = mouseCard_->detailLayout();
    more->addWidget(label("连接与电量", "status"));
    mouseMore_ = label("等待设备读数", "muted");
    more->addWidget(mouseMore_);
    mouseDetails_ = new MouseDetails;
    // 写入区只发出意图；是否真正发送由服务的实验性写入闸门决定。
    connect(mouseDetails_, &MouseDetails::dpiWriteRequested, this, [this](int dpi) {
        if (vgnWriteTarget_.isEmpty() || vgnActiveStage_ < 0)
            return;
        service_->writeVgnDpiStage(vgnWriteTarget_, vgnActiveStage_, dpi, dpi);
    });
    connect(mouseDetails_, &MouseDetails::rateWriteRequested, this, [this](int hertz) {
        if (!vgnWriteTarget_.isEmpty())
            service_->writeVgnPollingRate(vgnWriteTarget_, hertz);
    });
    connect(mouseDetails_, &MouseDetails::stageWriteRequested, this, [this](int stage) {
        if (vgnWriteTarget_.isEmpty())
            return;
        // 先按用户意图记下活动档位：紧接着的 DPI 写入应作用于新档位，
        // 而不是等下一次扫描回来才更新（最多 3 秒的错档风险）。
        vgnActiveStage_ = stage;
        service_->writeVgnActiveStage(vgnWriteTarget_, stage);
    });
    more->addWidget(mouseDetails_);
    bindMouseButton_ = new QPushButton("关联有线与无线连接");
    bindMouseButton_->setIcon(fluentIcon(UiIcon::Link));
    bindMouseButton_->setIconSize(QSize(18, 18));
    connect(bindMouseButton_, &QPushButton::clicked, this, &MainWindow::bindMouseConnections);
    more->addWidget(bindMouseButton_);
    more->addStretch();
    monitorCard_ = card(grid, "显示器  /  DISPLAY", "等待识别显示器", &body);
    monitorCard_->setCardId("monitor");
    monitorCard_->setCategoryIcon(fluentIcon(UiIcon::Desktop, QColor("#4376ef")));
    monitorSelector_ = createThemedComboBox();
    monitorSelector_->setPlaceholderText("选择要控制的显示器");
    monitorSelector_->setObjectName("monitorSelector");
    body->addWidget(monitorSelector_);
    auto* brightnessControl = new QWidget;
    brightnessControl->setObjectName("monitorBrightnessControl");
    auto* brightnessRow = new QHBoxLayout(brightnessControl);
    brightnessRow->setContentsMargins(0, 0, 0, 0);
    brightnessRow->setSpacing(8);
    auto* brightnessLabel = label("亮度", "controlLabel");
    brightnessLabel->setFixedWidth(78);
    brightnessRow->addWidget(brightnessLabel);
    monitorValue_ = label("—", "audioMetric");
    monitorValue_->setFixedWidth(55);
    brightnessRow->addWidget(monitorValue_);
    monitorSlider_ = createThemedSlider(Qt::Horizontal);
    monitorSlider_->setRange(0, 100);
    monitorSlider_->setEnabled(false);
    monitorSlider_->setAccessibleName("显示器亮度");
    brightnessRow->addWidget(monitorSlider_, 1);
    brightnessControl->setMinimumHeight(34);
    body->addWidget(brightnessControl);
    body->addWidget(label("亮度 · 仅对读取成功的显示器开放", "muted"));
    body->addWidget(label("DDC/CI · 控制能力按当前连接检测", "muted"));
    body->addStretch();
    more = monitorCard_->detailLayout();
    monitorControls_ = new MonitorControls;
    more->addWidget(monitorControls_);
    connect(monitorControls_, &MonitorControls::refreshRequested, this,
            [this] { service_->requestMonitorControls(monitorSelector_->currentData().toString()); });
    connect(monitorControls_, &MonitorControls::valueRequested, this, [this](int code, int value) {
        const auto feature = featureForMonitorVcp(static_cast<std::uint8_t>(code));
        if (!feature)
            return;
        rememberSetting("monitor:" + monitorSelector_->currentData().toString(),
                        QString("vcp_%1").arg(code, 2, 16, QChar('0')), value);
        service_->setFeature(monitorSelector_->currentData().toString(), *feature, value);
    });
    connect(monitorCard_, &DeviceCard::expandedChanged, this,
            [this, quick = body, detail = more, brightnessControl](bool expanded) {
        // 页面切换只移动原有控件，不生成第二个滑条或重复连接写入信号。
        if (expanded) {
            detail->insertWidget(0, monitorSelector_);
            monitorControls_->setBrightnessControl(brightnessControl);
        } else {
            quick->insertWidget(0, monitorSelector_);
            quick->insertWidget(1, brightnessControl);
        }
        monitorSelector_->setVisible(!expanded || monitorSelector_->count() != 1);
        brightnessControl->show();
        if (expanded)
            service_->requestMonitorControls(monitorSelector_->currentData().toString());
    });
    monitorDetails_ = new PercentDetails("亮度", {25, 50, 75});
    more->addWidget(monitorDetails_);
    more->addStretch();
    connect(monitorDetails_, &PercentDetails::refreshRequested, this, &MainWindow::requestManualRefresh);
    connect(monitorDetails_, &PercentDetails::valueRequested, this, [this](int value) {
        const QSignalBlocker block(monitorSlider_);
        monitorSlider_->setValue(value);
        monitorValue_->setText(QString::number(value) + "%");
        rememberSetting("monitor:" + monitorSelector_->currentData().toString(), "brightness", value);
        service_->setFeature(monitorSelector_->currentData().toString(), DeviceFeature::Brightness, value);
        service_->finishBrightness();
    });
    connect(monitorSelector_, &QComboBox::currentIndexChanged, this, [this] {
        const QSignalBlocker block(monitorSlider_);
        monitorSlider_->setSliderDown(false);
        // 切换设备必须清除旧目标的编辑状态与读数。
        monitorDetails_->setAvailable(false);
        updateMonitor();
    });
    connect(monitorSlider_, &QSlider::valueChanged, this, [this](int value) {
        monitorValue_->setText(QString::number(value) + "%");
        monitorDetails_->setValue(value);
        rememberSetting("monitor:" + monitorSelector_->currentData().toString(), "brightness", value);
        service_->setFeature(monitorSelector_->currentData().toString(), DeviceFeature::Brightness, value);
    });
    connect(monitorSlider_, &QSlider::sliderReleased, service_, &DeviceService::finishBrightness);
    keyboardCard_ = card(grid, "键盘  /  KEYBOARD", "等待识别键盘", &body);
    keyboardCard_->setCardId("keyboard");
    keyboardCard_->setCategoryIcon(fluentIcon(UiIcon::Keyboard, QColor("#4376ef")));
    keyboardSelector_ = createThemedComboBox();
    keyboardSelector_->setPlaceholderText("选择键盘接口");
    body->addWidget(keyboardSelector_);
    body->addSpacing(20);
    connect(keyboardSelector_, &QComboBox::currentIndexChanged, this, [this] {
        const auto selected = keyboardSelector_->currentData().toString();
        if (!selected.isEmpty()) {
            settings_.setValue("keyboard/selectedId", selected);
            settings_.sync();
        }
        updateKeyboard();
    });
    body->addWidget(label("—", "metric"));
    keyboardState_ = label("正在检测接口", "muted");
    body->addWidget(keyboardState_);
    keyboardDetail_ = label("电量暂不可读", "muted");
    body->addWidget(keyboardDetail_);
    body->addStretch();
    more = keyboardCard_->detailLayout();
    more->addWidget(label("接口状态", "status"));
    keyboardMore_ = label("正在检测接口", "muted");
    more->addWidget(keyboardMore_);
    more->addWidget(
        label("电量、充电状态、按键映射与 RGB 暂不可用；接口可见不代表无线设备已在线。", "muted"));
    auto* keyboardRefresh = new QPushButton("更新状态");
    keyboardRefresh->setIcon(fluentIcon(UiIcon::Refresh));
    keyboardRefresh->setIconSize(QSize(16, 16));
    connect(keyboardRefresh, &QPushButton::clicked, this, &MainWindow::requestManualRefresh);
    more->addWidget(keyboardRefresh);
    more->addStretch();
    audioCard_ = card(grid, "音频  /  AUDIO", "音频设备", &body);
    audioCard_->setCardId("audio");
    audioCard_->setCategoryIcon(fluentIcon(UiIcon::Headphones, QColor("#4376ef")));
    more = audioCard_->detailLayout();
    const auto addAudioSection = [this, body, more](bool capture) {
        const QString direction = capture ? "输入" : "输出";
        auto* combo = createThemedComboBox();
        combo->setObjectName(capture ? "audioInputSelector" : "audioOutputSelector");
        combo->setAccessibleName(direction + "设备");
        combo->setPlaceholderText("选择" + direction + "设备");
        auto* nameRow = new QHBoxLayout;
        nameRow->setSpacing(8);
        auto* directionLabel = label(direction, "controlLabel");
        directionLabel->setFixedWidth(60);
        nameRow->addWidget(directionLabel);
        nameRow->addWidget(combo, 1);
        auto* valueLabel = label("—", "audioMetric");
        valueLabel->setFixedWidth(55);
        auto* slider = createThemedSlider(Qt::Horizontal);
        slider->setObjectName(capture ? "audioInputVolume" : "audioOutputVolume");
        slider->setRange(0, 100);
        slider->setEnabled(false);
        slider->setMinimumWidth(70);
        slider->setAccessibleName(direction + "音量");
        auto* mute = new QPushButton("静音");
        mute->setIcon(fluentIcon(capture ? UiIcon::Mic : UiIcon::Speaker));
        mute->setIconSize(QSize(20, 20));
        mute->setObjectName(capture ? "audioInputMute" : "audioOutputMute");
        mute->setAccessibleName(direction + "静音");
        mute->setEnabled(false);
        mute->setProperty("compact", true);
        auto* controlRow = new QHBoxLayout;
        controlRow->setSpacing(8);
        controlRow->addSpacing(68);
        controlRow->addWidget(valueLabel);
        controlRow->addWidget(slider, 1);
        controlRow->addWidget(mute);
        auto* section = new QVBoxLayout;
        section->setSpacing(5);
        section->addLayout(nameRow);
        section->addLayout(controlRow);
        body->addLayout(section);
        auto* details = new PercentDetails(direction + "音量", {10, 25, 50});
        more->addWidget(details);
        auto* detailMute = new QPushButton(direction + "静音");
        detailMute->setIcon(fluentIcon(capture ? UiIcon::Mic : UiIcon::Speaker));
        detailMute->setIconSize(QSize(18, 18));
        detailMute->setEnabled(false);
        more->addWidget(detailMute);
        if (capture) {
            audioInputSelector_ = combo;
            audioInputValue_ = valueLabel;
            audioInputSlider_ = slider;
            inputMuteButton_ = mute;
            inputDetailMuteButton_ = detailMute;
            audioInputDetails_ = details;
        } else {
            audioSelector_ = combo;
            audioValue_ = valueLabel;
            audioSlider_ = slider;
            muteButton_ = mute;
            detailMuteButton_ = detailMute;
            audioDetails_ = details;
        }
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, slider, details] {
            const QSignalBlocker block(slider);
            slider->setSliderDown(false);
            details->setAvailable(false);
            updateAudio();
        });
        connect(slider, &QSlider::valueChanged, this, [this, combo, valueLabel, details](int value) {
            valueLabel->setText(QString::number(value) + "%");
            details->setValue(value);
            const auto id = combo->currentData().toString();
            rememberSetting("audio:" + id, "volume", value);
            service_->setFeature(id, DeviceFeature::Volume, value);
        });
        connect(slider, &QSlider::sliderReleased, this,
                [this, combo] { service_->finishVolume(combo->currentData().toString()); });
        connect(details, &PercentDetails::refreshRequested, this, &MainWindow::requestManualRefresh);
        connect(details, &PercentDetails::valueRequested, this, [this, combo, slider, valueLabel](int value) {
            const QSignalBlocker block(slider);
            slider->setValue(value);
            valueLabel->setText(QString::number(value) + "%");
            const auto id = combo->currentData().toString();
            rememberSetting("audio:" + id, "volume", value);
            service_->setFeature(id, DeviceFeature::Volume, value);
            service_->finishVolume(id);
        });
        connect(mute, &QPushButton::clicked, this, [this, combo, capture] {
            const auto id = combo->currentData().toString();
            for (const auto& endpoint : service_->snapshot().audio)
                if (qs(endpoint.id) == id && endpoint.capture == capture) {
                    rememberSetting("audio:" + id, "mute", !endpoint.muted);
                    service_->setFeature(id, DeviceFeature::Mute, !endpoint.muted);
                    break;
                }
        });
        connect(detailMute, &QPushButton::clicked, mute, &QPushButton::click);
    };
    addAudioSection(false);
    addAudioSection(true);
    headsetBatteryStatus_ = label({}, "muted");
    headsetBatteryStatus_->setObjectName("headsetBatteryStatus");
    headsetBatteryStatus_->hide();
    body->addWidget(headsetBatteryStatus_);
    headsetDetails_ = new HeadsetDetails;
    more->addWidget(headsetDetails_);
    body->addStretch();
    more->addWidget(label("分别调节 Windows 输出与输入端点；无线在线、电量与硬件 EQ 按型号适配。", "muted"));
    more->addStretch();
    content->addWidget(deviceGrid_, 1);
    auto* footer = new QHBoxLayout;
    auto* footerHint = label("设备状态以实际读取结果为准", "muted");
    footerHint->setWordWrap(false);
    footerHint->setMinimumWidth(0);
    footer->addWidget(footerHint);
    footer->addStretch();
    updated_ = label("尚未扫描", "muted");
    updated_->setWordWrap(false);
    footer->addWidget(updated_);
    content->addLayout(footer);
    pages_ = new QStackedWidget;
    // 小窗口/高 DPI 下整页可滚动，而不是切换另一套字号和图标。
    // 这样缩放窗口时不会突然变大，也不需要重建整窗样式。
    auto* overviewScroll = new QScrollArea;
    overviewScroll->setObjectName("overviewScroll");
    overviewScroll->setFrameShape(QFrame::NoFrame);
    overviewScroll->setWidgetResizable(true);
    overviewScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    overviewScroll->setWidget(page);
    pages_->addWidget(overviewScroll);
    auto* savedPage = new QWidget;
    savedPage->setObjectName("savedPage");
    auto* savedLayout = new QVBoxLayout(savedPage);
    savedLayout->setContentsMargins(30, 28, 30, 22);
    savedLayout->setSpacing(16);
    savedLayout->addWidget(label("已保存设备", "title"));
    savedProfilesHint_ = label({}, "muted");
    savedLayout->addWidget(savedProfilesHint_);
    auto* savedColumns = new QSplitter;
    savedTree_ = createThemedTreeWidget();
    savedTree_->setObjectName("已保存设备分类树");
    savedTree_->setAccessibleName("已保存设备分类树");
    savedTree_->setHeaderHidden(true);
    savedTree_->setColumnCount(1);
    savedTree_->setIndentation(16);
    savedTree_->setIconSize(QSize(20, 20));
    savedTree_->setExpandsOnDoubleClick(false);
    savedDetail_ = new QPlainTextEdit;
    savedDetail_->setReadOnly(true);
    savedDetail_->setAccessibleName("设备保存设置与状态");
    auto* savedDetailPanel = new QWidget;
    auto* savedDetailLayout = new QVBoxLayout(savedDetailPanel);
    savedDetailLayout->setContentsMargins(0, 0, 0, 0);
    savedDetailLayout->setSpacing(12);
    savedDetailLayout->addWidget(savedDetail_, 1);
    deleteProfileResult_ = label({}, "muted");
    deleteProfileResult_->setObjectName("deleteProfileResult");
    deleteProfileResult_->setAccessibleName("档案删除结果");
    deleteProfileResult_->setTextFormat(Qt::PlainText);
    deleteProfileResult_->hide();
    savedDetailLayout->addWidget(deleteProfileResult_);
    deleteProfileHint_ = label("请选择需要删除的设备档案。", "muted");
    deleteProfileHint_->setTextFormat(Qt::PlainText);
    deleteProfileHint_->setAccessibleName("档案删除条件");
    savedDetailLayout->addWidget(deleteProfileHint_);
    deleteProfileButton_ = new QPushButton("删除设备档案");
    deleteProfileButton_->setObjectName("deleteDeviceProfile");
    deleteProfileButton_->setAccessibleName("删除设备档案");
    deleteProfileButton_->setEnabled(false);
    savedDetailLayout->addWidget(deleteProfileButton_, 0, Qt::AlignRight);
    connect(deleteProfileButton_, &QPushButton::clicked, this, &MainWindow::requestProfileDeletion);
    savedColumns->addWidget(savedTree_);
    savedColumns->addWidget(savedDetailPanel);
    savedColumns->setStretchFactor(0, 1);
    savedColumns->setStretchFactor(1, 2);
    savedLayout->addWidget(savedColumns, 1);
    connect(savedTree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current) {
        deleteProfileResult_->clear();
        deleteProfileResult_->hide();
        if (current && !current->data(0, profileIdRole).toString().isEmpty())
            showSavedProfile();
        else
            updateProfileDeleteControls();
    });
    connect(savedTree_, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem* item) {
        if (!item->data(0, profileGroupRole).toString().isEmpty())
            item->setExpanded(!item->isExpanded());
    });
    const auto rememberExpansion = [this](QTreeWidgetItem* item) {
        const auto key = item->data(0, profileGroupRole).toString();
        if (!key.isEmpty())
            settings_.setValue("saved/groups/" + key, item->isExpanded());
    };
    connect(savedTree_, &QTreeWidget::itemExpanded, this, rememberExpansion);
    connect(savedTree_, &QTreeWidget::itemCollapsed, this, rememberExpansion);
    pages_->addWidget(savedPage);
    settingsPage_ = new SettingsPage(QFileInfo(settings_.fileName()).absolutePath(), preferences_);
    pages_->addWidget(settingsPage_);
    connect(settingsPage_, &SettingsPage::preferencesRequested, this, &MainWindow::changePreferences);
    connect(settingsPage_, &SettingsPage::openDataDirectoryRequested, this,
            [this] { openSettingsLocation(false); });
    connect(settingsPage_, &SettingsPage::openLogRequested, this, [this] { openSettingsLocation(true); });
    root->addWidget(pages_, 1);
    setCentralWidget(central);
    monitorCard_->setExpanded(true);
    connect(sidebarToggle_, &QToolButton::clicked, this, [this] {
        setSidebarCollapsed(!sidebarCollapsed_);
        settings_.setValue("ui/sidebarCollapsed", sidebarCollapsed_);
        settings_.sync();
    });
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ScreenChangeInternal ||
        event->type() == QEvent::DevicePixelRatioChange) {
        // Qt 在 DPI/屏幕切换过程中还会更新窗口几何；延后一拍再按新工作区重算，
        // 否则会用旧屏幕的可用尺寸，窗口在下一次布局时突然被撑大。
        QTimer::singleShot(0, this, [this] {
            watchCurrentScreen();
            updateWindowConstraints();
        });
    }
}

void MainWindow::watchCurrentScreen() {
    auto* current = screen();
    if (current == watchedScreen_)
        return;
    if (watchedScreen_)
        disconnect(watchedScreen_, nullptr, this, nullptr);
    watchedScreen_ = current;
    if (!watchedScreen_)
        return;
    connect(watchedScreen_, &QScreen::availableGeometryChanged, this,
            [this](const QRect&) { updateWindowConstraints(); });
    connect(watchedScreen_, &QScreen::logicalDotsPerInchChanged, this,
            [this](qreal) { updateWindowConstraints(); });
}

void MainWindow::updateWindowConstraints() {
    // 960×600 是 100% 缩放下可用的设计下限；最终上限永远不超过当前工作区。
    const QSize design(960, 600);
    const auto* display = screen();
    if (!display) {
        if (minimumSize() != design)
            setMinimumSize(design);
        return;
    }
    const auto available = display->availableGeometry().size();
    auto minimum = design.boundedTo(available);
    QSize maximum = available;
    if (windowHandle()) {
        const auto frame = windowHandle()->frameMargins();
        const QSize client = available - QSize(frame.left() + frame.right(), frame.top() + frame.bottom());
        if (!client.isEmpty()) {
            maximum = client;
            minimum = minimum.boundedTo(client);
        }
    }
    if (minimumSize() != minimum)
        setMinimumSize(minimum);
    if (!isVisible() || maximum.isEmpty())
        return;
    const auto target = size().expandedTo(minimum).boundedTo(maximum);
    if (target != size())
        resize(target);
}

void MainWindow::setSidebarCollapsed(bool collapsed) {
    sidebarCollapsed_ = collapsed;
    auto* side = static_cast<QVBoxLayout*>(sidebar_->layout());
    sidebar_->setFixedWidth(collapsed ? 64 : 230);
    side->setContentsMargins(collapsed ? 10 : 18, collapsed ? 16 : 18, collapsed ? 10 : 18,
                             collapsed ? 24 : 36);
    side->setSpacing(collapsed ? 10 : 12);
    toggleRow_->setStretch(0, 1);
    toggleRow_->setStretch(2, collapsed ? 1 : 0);
    brandName_->setVisible(!collapsed);
    brandTagline_->setVisible(!collapsed);
    sidebarSeparator_->setVisible(!collapsed);
    connectionPanel_->setVisible(!collapsed);
    brandLayout_->setContentsMargins(collapsed ? 0 : 16, 0, 0, 0);
    brandLayout_->setAlignment(brandIcon_, collapsed ? Qt::AlignHCenter : Qt::Alignment());
    for (auto* button : sidebar_->findChildren<QPushButton*>()) {
        if (!button->property("navigation").toBool())
            continue;
        const auto text = button->property("navigationText").toString();
        button->setText(collapsed ? QString{} : text);
        button->setToolTip(collapsed ? text : QString{});
        button->setAccessibleName(text);
        button->setIconSize(collapsed ? QSize(24, 24) : QSize(26, 26));
        button->setStyleSheet(collapsed ? QStringLiteral("padding:0; text-align:center;") : QString{});
    }
    sidebarToggle_->setIcon(sidebarChevron(collapsed));
    sidebarToggle_->setToolTip(collapsed ? "展开侧栏" : "收起侧栏");
    sidebarToggle_->setAccessibleName(collapsed ? "展开侧栏" : "收起侧栏");
}

void MainWindow::updateDevices() {
    const auto& snapshotData = service_->snapshot();
    if (snapshotData.hidEnumerationSucceeded) {
        auto endpoints = discoverHidEndpoints(snapshotData.hid);
        std::vector<std::string> activeBindingIds;
        for (const auto& e : endpoints)
            if (bindings_.contains(qs(e.endpointId)))
                activeBindingIds.push_back(e.endpointId);
        registry_.setEndpoints(std::move(endpoints));
        if (bindings_.size() > 1) {
            if (!activeBindingIds.empty())
                registry_.bindEndpoints(
                    activeBindingIds,
                    settings_.value("mouse/bindingId", "user:primary-f2").toString().toStdString());
        }
        for (const auto& [path, reading] : snapshotData.batteries) {
            if (ingested_[path] == reading.sampledMs)
                continue;
            ingested_[path] = reading.sampledMs;
            DeviceObservation observation;
            const auto& b = reading.value;
            observation.linkState = b.onlineKnown ? (b.deviceOnline ? LinkState::Online : LinkState::Offline)
                                                  : LinkState::Unknown;
            if (b.batteryValid) {
                observation.reportedPercent = b.percentage;
                observation.charging = b.charging;
            }
            observation.statusDetail = b.error;
            DeviceCapabilities caps;
            caps.battery = b.batteryValid;
            caps.charging = b.batteryValid;
            observation.caps = caps;
            // 墙上时间只用于显示；注册表的顺序和新鲜度使用单调时间，系统时间校正不会冻结状态。
            observation.sampledAt = TimePoint(std::chrono::milliseconds(reading.sampledMs));
            registry_.observe(qs(path).toStdString(), observation,
                              reading.sampledMonotonicMs > 0
                                  ? MonotonicTimePoint(
                                        std::chrono::milliseconds(reading.sampledMonotonicMs))
                                  : MonotonicClock::now());
        }
    }
    devices_ = registry_.snapshots(MonotonicClock::now(), std::chrono::seconds(45));
    QList<QPair<QString, QString>> mice;
    QString verifiedMouse;
    int verifiedMouseCount = 0;
    for (const auto& d : devices_)
        if (d.kind == DeviceKind::Mouse && !d.endpoints.empty()) {
            auto name = QString("%1 · %2").arg(qs(d.name), transportText(d));
            if (devices_.size() > 1 && !d.endpoints.empty()) {
                const auto& e = d.endpoints.front();
                if (!e.serial.empty())
                    name += " · " + qs(e.serial).right(8);
            }
            mice.append({name, qs(d.logicalId)});
            if (std::any_of(d.endpoints.begin(), d.endpoints.end(), isVgnF2MasterPlus)) {
                verifiedMouse = qs(d.logicalId);
                ++verifiedMouseCount;
            }
        }
    // 当前选择仍在时保持不变；无选择时恢复保存项，再尝试唯一已适配鼠标。
    const auto preferredMouse = availablePreference(
        mice, {settings_.value("mouse/selectedId").toString(), settings_.value("mouse/bindingId").toString(),
               verifiedMouseCount == 1 ? verifiedMouse : QString{}});
    mouseSelector_->setPlaceholderText(mice.isEmpty() ? "等待发现鼠标"
                                                      : QString("已发现 %1 台，请选择鼠标").arg(mice.size()));
    repopulate(mouseSelector_, mice, preferredMouse);
    updateMouse();
    QList<QPair<QString, QString>> keyboards;
    for (const auto& d : devices_)
        if (d.kind == DeviceKind::Keyboard && !d.endpoints.empty())
            keyboards.append({qs(d.name), qs(d.logicalId)});
    keyboardSelector_->setPlaceholderText(
        keyboards.isEmpty() ? "等待发现键盘" : QString("已发现 %1 台，请选择键盘").arg(keyboards.size()));
    repopulate(keyboardSelector_, keyboards, settings_.value("keyboard/selectedId").toString());
    updateKeyboard();
    if (snapshotData.hidEnumerationSucceeded) {
        mousePresent_ = !mice.isEmpty();
        keyboardPresent_ = !keyboards.isEmpty();
    }
    for (const bool capture : {false, true}) {
        QList<QPair<QString, QString>> audio;
        QString known;
        int knownCount = 0;
        for (const auto& endpoint : snapshotData.audio) {
            if (endpoint.capture != capture)
                continue;
            const auto name = qs(endpoint.name);
            audio.append({name, qs(endpoint.id)});
            if (name.contains("ATK", Qt::CaseInsensitive) || name.contains("N9", Qt::CaseInsensitive) ||
                name.contains("Neptune", Qt::CaseInsensitive)) {
                known = qs(endpoint.id);
                ++knownCount;
            }
        }
        auto* combo = capture ? audioInputSelector_ : audioSelector_;
        auto* slider = capture ? audioInputSlider_ : audioSlider_;
        const auto key = capture ? "audio/inputSelectedId" : "audio/outputSelectedId";
        const auto preferred = availablePreference(audio, {settings_.value(key).toString(),
                                                           settings_.value("audio/selected").toString(),
                                                           knownCount == 1 ? known : QString{}});
        const auto current = combo->currentData().toString();
        const bool present = std::any_of(audio.begin(), audio.end(),
                                         [&current](const auto& item) { return item.second == current; });
        if (snapshotData.audioEnumerationSucceeded &&
            (!present || (!slider->isSliderDown() && !service_->volumeUpdatePending(current))))
            // 只有一个端点时它是唯一可选目标，自动选中不构成身份猜测；多端点仍要求用户明确选择。
            repopulate(combo, audio, preferred, true);
    }
    updateAudio();
    QList<QPair<QString, QString>> monitors;
    QString preferredMonitor;
    for (const auto& m : snapshotData.monitors) {
        const auto name = qs(m.name);
        monitors.append({name + (m.brightnessReadable ? "" : " · 亮度暂不可调"), qs(m.id)});
    }
    const auto savedMonitor = settings_.value("monitor/selected").toString();
    if (!savedMonitor.isEmpty())
        preferredMonitor = savedMonitor;
    const auto currentMonitor = monitorSelector_->currentData().toString();
    const bool monitorStillPresent =
        std::any_of(monitors.begin(), monitors.end(),
                    [&currentMonitor](const auto& m) { return m.second == currentMonitor; });
    if (snapshotData.monitorEnumerationSucceeded &&
        (!monitorStillPresent ||
         (!monitorSlider_->isSliderDown() && !service_->brightnessUpdatePending(currentMonitor))))
        // 单屏可直接使用，多屏保留已选目标；不能按列表顺序猜测控制对象。
        repopulate(monitorSelector_, monitors, preferredMonitor, true);
    updateMonitor();
    if (snapshotData.audioEnumerationSucceeded)
        audioPresent_ = !snapshotData.audio.empty();
    if (snapshotData.monitorEnumerationSucceeded)
        monitorPresent_ = !monitors.isEmpty();
    deviceGrid_->setPresent(mouseCard_, mousePresent_);
    deviceGrid_->setPresent(keyboardCard_, keyboardPresent_);
    deviceGrid_->setPresent(audioCard_, audioPresent_);
    deviceGrid_->setPresent(monitorCard_, monitorPresent_);
    deviceGrid_->setScanning(false);
    updateProfiles();
    updated_->setText("最近扫描 " +
                      QDateTime::fromMSecsSinceEpoch(snapshotData.sampledMs).toString("HH:mm:ss"));
    if (manualRefreshTicket_) {
        notice_->setText("正在重新读取设备状态…");
        notice_->setToolTip({});
    } else if (QDateTime::currentMSecsSinceEpoch() >= manualRefreshMessageUntil_ &&
               QDateTime::currentMSecsSinceEpoch() >= lowBatteryNoticeUntil_) {
        if (!snapshotData.errors.isEmpty()) {
            notice_->setText("部分状态暂不可读取，可打开设备详情查看原因。");
            notice_->setToolTip(plainTextToolTip(snapshotData.errors.join('\n')));
        } else {
            notice_->setText("自动检测连接变化。有线与 2.4G 可分别识别，未知电量保持为空。");
            notice_->setToolTip({});
        }
    }
    updateLowBatteryNotifications();
}

void MainWindow::updateLowBatteryNotifications() {
    if (!preferences_.lowBatteryNotification) {
        lowBatteryNotified_.clear();
        return;
    }
    constexpr int threshold = 10;
    const auto& snapshot = service_->snapshot();
    QSet<QString> lowNow;
    QList<QPair<QString, QString>> alerts;

    for (const auto& device : devices_) {
        if (device.kind != DeviceKind::Mouse || device.endpoints.empty())
            continue;
        const auto key = QStringLiteral("mouse:") + qs(device.logicalId);
        if (!device.battery.reportedPercent || device.stale || device.linkState != LinkState::Online ||
            device.battery.charging.value_or(false)) {
            continue;
        }
        const int percent = *device.battery.reportedPercent;
        if (percent < threshold) {
            lowNow.insert(key);
            if (!lowBatteryNotified_.contains(key)) {
                const auto name = device.name.empty() ? QStringLiteral("鼠标") : qs(device.name);
                alerts.append({key, QStringLiteral("%1 电量 %2%，请及时充电。").arg(name).arg(percent)});
            }
        }
    }

    const auto headsetName = [&snapshot](const std::wstring& container) {
        for (const auto& endpoint : snapshot.audio)
            if (endpoint.containerId == container && !endpoint.capture && !endpoint.name.empty())
                return qs(endpoint.name);
        for (const auto& endpoint : snapshot.audio)
            if (endpoint.containerId == container && !endpoint.name.empty())
                return qs(endpoint.name);
        return QStringLiteral("耳机");
    };
    for (const auto& entry : snapshot.headsets) {
        const auto& reading = entry.second;
        if (!reading.value.status)
            continue;
        const auto& status = *reading.value.status;
        const auto key = QStringLiteral("headset:") + qs(entry.first);
        if (status.batteryKind != n9::BatteryKind::Discharging ||
            (reading.sampledMonotonicMs > 0 &&
             monotonicNowMs() - reading.sampledMonotonicMs > 45000)) {
            continue;
        }
        if (status.batteryPercent < threshold) {
            lowNow.insert(key);
            if (!lowBatteryNotified_.contains(key)) {
                alerts.append({key, QStringLiteral("%1 电量 %2%，请及时充电。")
                                        .arg(headsetName(reading.containerId))
                                        .arg(status.batteryPercent)});
            }
        }
    }

    // 电量恢复、开始充电或设备移除后重新布防，下一次低电量可以再次提醒。
    for (auto it = lowBatteryNotified_.begin(); it != lowBatteryNotified_.end();) {
        if (!lowNow.contains(*it))
            it = lowBatteryNotified_.erase(it);
        else
            ++it;
    }
    for (const auto& alert : alerts) {
        lowBatteryNotified_.insert(alert.first);
        qInfo().noquote() << "低电量提醒：" << alert.second;
        notice_->setText(alert.second);
        notice_->setToolTip(plainTextToolTip(alert.second));
        lowBatteryNoticeUntil_ = QDateTime::currentMSecsSinceEpoch() + 15000;
        emit lowBatteryAlert(alert.second);
        if (tray_)
            tray_->showMessage(QStringLiteral("外设电量不足"), alert.second,
                               QSystemTrayIcon::Warning, 10000);
    }
}

void MainWindow::updateMouse() {
    if (!taskbarPreviewTimer_.isActive())
        taskbarPreviewTimer_.start();
    const auto id = mouseSelector_->currentData().toString();
    auto it = std::find_if(devices_.begin(), devices_.end(),
                           [&id](const auto& d) { return qs(d.logicalId) == id; });
    if (it == devices_.end()) {
        mouseCard_->setTitle(mouseSelector_->count() ? "选择鼠标" : "等待识别鼠标");
        bindMouseButton_->hide();
        mouseValue_->setText("—");
        mouseBattery_->hide();
        mouseState_->setText("尚无有效电量");
        mouseConnection_->setText(mouseSelector_->count() ? "请在上方选择要查看的鼠标" : "等待设备接口");
        mouseFreshness_->setText("选择设备后查看可读取的状态");
        mouseMore_->setText("等待设备接口或选择设备；电量与充电状态未知。");
        mouseDetails_->setInfo(nullptr);
        mouseDetails_->setWritable(false);
        vgnWriteTarget_.clear();
        vgnActiveStage_ = -1;
        mouseConfiguration_->clear();
        mouseConfiguration_->hide();
        if (tray_)
            tray_->setToolTip("外设中心 · 鼠标尚无有效读数");
        return;
    }
    const auto& d = *it;
    const auto config = service_->snapshot().mouseForDevice(d);
    // 写入目标是已核验的 VGN 控制接口路径，不是逻辑设备名。
    vgnWriteTarget_.clear();
    vgnActiveStage_ = -1;
    for (const auto& endpoint : d.endpoints)
        if (isVgnF2MasterPlus(endpoint)) {
            vgnWriteTarget_ = qs(endpoint.endpointId);
            break;
        }
    if (config && config->value.profile)
        vgnActiveStage_ = config->value.profile->activeStage;
    const bool known = std::any_of(d.endpoints.begin(), d.endpoints.end(),
        [](const auto& endpoint) { return isVgnF2MasterPlus(endpoint); });
    const bool profileStale =
        !config || !config->value.profileError.empty() || config->profileSampledMonotonicMs <= 0 ||
        monotonicNowMs() - config->profileSampledMonotonicMs > 45000;
    mouseConfiguration_->setVisible(known);
    mouseDetails_->setInfo(config && config->value.profile ? &*config->value.profile : nullptr,
        config ? config->profileSampledMs : 0, profileStale,
        config ? qs(config->value.profileError) : QString{});
    mouseDetails_->setWritable(preferences_.experimentalDeviceWrites && !vgnWriteTarget_.isEmpty());
    if (config && config->value.profile->activeStage >= 0 &&
        static_cast<std::size_t>(config->value.profile->activeStage) < config->value.profile->stages.size()) {
        const auto& profile = *config->value.profile;
        mouseConfiguration_->setText(QStringLiteral("%1 · %2 Hz%3")
            .arg(MouseDetails::dpiText(profile.stages[profile.activeStage])).arg(profile.pollingRateHz)
            .arg(profileStale ? QStringLiteral(" · 上次读数") : QString{}));
    } else {
        mouseConfiguration_->setText(QStringLiteral("DPI / 回报率：当前尚未读取成功"));
    }
    mouseCard_->setTitle(qs(d.name));
    bindMouseButton_->setVisible(std::any_of(d.endpoints.begin(), d.endpoints.end(),
                                             [](const auto& e) { return isVgnF2MasterPlus(e); }));
    mouseConnection_->setText(transportText(d));
    mouseState_->setText(linkText(d.linkState));
    if (d.battery.reportedPercent) {
        mouseValue_->setText(QString::number(*d.battery.reportedPercent) + "%");
        mouseBattery_->show();
        mouseBattery_->setValue(*d.battery.reportedPercent);
        QString status =
            d.stale ? "上次读数" : (d.battery.charging.value_or(false) ? "正在充电" : "设备上报电量");
        if (d.lastUpdated)
            status += QString(" · %1").arg(
                QDateTime::fromMSecsSinceEpoch(
                    std::chrono::duration_cast<std::chrono::milliseconds>(d.lastUpdated->time_since_epoch())
                        .count())
                    .toString("HH:mm:ss"));
        mouseFreshness_->setText(status);
    } else {
        mouseValue_->setText("—");
        mouseBattery_->hide();
        mouseFreshness_->setText("电量暂不可读");
    }
    mouseState_->setToolTip(plainTextToolTip(qs(d.statusDetail)));
    QString moreText = transportText(d) + " · " + linkText(d.linkState) + "\n";
    moreText += "电量：" + mouseValue_->text() + " · " + mouseFreshness_->text();
    if (d.battery.charging && !d.stale)
        moreText += *d.battery.charging ? "\n充电状态：正在充电" : "\n充电状态：未充电";
    else
        moreText += "\n当前充电状态：未知";
    mouseMore_->setText(moreText);
    mouseMore_->setToolTip(plainTextToolTip(qs(d.statusDetail)));
    if (tray_) {
        QString tooltip = "外设中心 · 鼠标 " + linkText(d.linkState);
        if (d.battery.reportedPercent)
            tooltip +=
                QString(" · %1% %2").arg(*d.battery.reportedPercent).arg(d.stale ? "（上次读数）" : "");
        tray_->setToolTip(tooltip);
    }
}
void MainWindow::updateKeyboard() {
    if (!taskbarPreviewTimer_.isActive())
        taskbarPreviewTimer_.start();
    const auto selected = keyboardSelector_->currentData().toString();
    const auto it = std::find_if(devices_.begin(), devices_.end(), [&selected](const auto& d) {
        return qs(d.logicalId) == selected && !d.endpoints.empty();
    });
    if (it == devices_.end()) {
        keyboardCard_->setTitle("选择键盘");
        keyboardState_->setText(keyboardSelector_->count() ? "请在上方选择要查看的键盘" : "等待键盘接口");
        keyboardDetail_->setText("型号以系统提供的名称为准");
    } else {
        keyboardCard_->setTitle(qs(it->name));
        keyboardState_->setText("已检测到键盘接口");
        keyboardDetail_->setText("电量与充电状态尚不可读；接口存在不代表无线设备在线");
    }
    keyboardMore_->setText(keyboardState_->text() + "\n" + keyboardDetail_->text());
}
void MainWindow::updateAudio() {
    updateAudioEndpoint(false);
    updateAudioEndpoint(true);
    const auto& snapshot = service_->snapshot();
    const RuntimeSnapshot::HeadsetReading* reading = nullptr;
    for (auto* selector : {audioSelector_, audioInputSelector_}) {
        const auto id = selector->currentData().toString();
        const auto audio = std::find_if(snapshot.audio.begin(), snapshot.audio.end(),
            [&](const auto& item) { return qs(item.id) == id; });
        if (audio != snapshot.audio.end())
            reading = snapshot.headsetForContainer(audio->containerId);
        if (reading)
            break;
    }
    headsetDetails_->setInfo(reading ? &reading->value : nullptr);
    headsetBatteryStatus_->setVisible(reading != nullptr);
    if (reading && reading->value.status) {
        const auto& status = *reading->value.status;
        const auto charging = status.batteryKind == n9::BatteryKind::Charging ? QStringLiteral("充电中")
            : status.batteryKind == n9::BatteryKind::Full ? QStringLiteral("已充满") : QStringLiteral("未充电");
        headsetBatteryStatus_->setText(QStringLiteral("耳机电量 %1% · %2 · 采样 %3")
            .arg(status.batteryPercent).arg(charging)
            .arg(QDateTime::fromMSecsSinceEpoch(reading->sampledMs).toString("HH:mm:ss")));
    } else if (reading) {
        headsetBatteryStatus_->setText(QStringLiteral("耳机接口可见 · 当前电量未确认"));
    }
}
void MainWindow::updateAudioEndpoint(bool capture) {
    if (!taskbarPreviewTimer_.isActive())
        taskbarPreviewTimer_.start();
    auto* combo = capture ? audioInputSelector_ : audioSelector_;
    auto* slider = capture ? audioInputSlider_ : audioSlider_;
    auto* metric = capture ? audioInputValue_ : audioValue_;
    auto* mute = capture ? inputMuteButton_ : muteButton_;
    auto* detailMute = capture ? inputDetailMuteButton_ : detailMuteButton_;
    auto* details = capture ? audioInputDetails_ : audioDetails_;
    const QString direction = capture ? "输入" : "输出";
    const auto selected = combo->currentData().toString();
    auto it = std::find_if(
        service_->snapshot().audio.begin(), service_->snapshot().audio.end(),
        [&selected, capture](const auto& a) { return qs(a.id) == selected && a.capture == capture; });
    const bool valid = it != service_->snapshot().audio.end() && it->volumeReadable;
    service_->selectVolumeTarget(valid ? selected : QString{}, capture);
    slider->setEnabled(valid);
    mute->setEnabled(valid);
    detailMute->setEnabled(valid);
    details->setAvailable(valid);
    if (!valid) {
        const QSignalBlocker block(slider);
        slider->setSliderDown(false);
        metric->setText("—");
        // 无有效端点时复位图标和操作文字，不沿用上一台设备的静音状态。
        const auto icon = fluentIcon(capture ? UiIcon::Mic : UiIcon::Speaker);
        mute->setIcon(icon);
        detailMute->setIcon(icon);
        mute->setText("静音");
        detailMute->setText(direction + "静音");
        mute->setAccessibleName(direction + "静音（当前不可用）");
        detailMute->setAccessibleName(direction + "静音（当前不可用）");
        mute->setToolTip(direction + "静音状态暂不可读");
        detailMute->setToolTip(direction + "静音状态暂不可读");
        details->setDescription(it == service_->snapshot().audio.end()
                                    ? "请先选择" + direction + "设备。"
                                    : "当前端点音量不可读，请更新状态后重试。");
        return;
    }
    settings_.setValue(capture ? "audio/inputSelectedId" : "audio/outputSelectedId", selected);
    // 最后一次写入读回前保留用户目标值，避免旧快照把拖动中的滑块拉回去。
    if (!slider->isSliderDown() && !service_->volumeUpdatePending(selected)) {
        QSignalBlocker block(slider);
        slider->setValue(static_cast<int>(std::lround(it->volumePercent)));
        metric->setText(QString::number(slider->value()) + "%");
        if (!details->isEditing())
            details->setValue(slider->value());
    }
    const auto headset = service_->snapshot().headsetForContainer(it->containerId);
    details->setDescription(direction + " · " + qs(it->name) +
        (headset && headset->value.status ? QStringLiteral("\n调节 Windows 端点音量；耳机私有状态已响应。")
                                        : QStringLiteral("\n调节 Windows 端点音量；无线在线状态尚未确认。")));
    mute->setText(it->muted ? "取消静音" : "静音");
    detailMute->setText(direction + (it->muted ? "取消静音" : "静音"));
    const auto icon = fluentIcon(capture ? (it->muted ? UiIcon::MicOff : UiIcon::Mic)
                                        : (it->muted ? UiIcon::SpeakerMute : UiIcon::Speaker));
    mute->setIcon(icon);
    detailMute->setIcon(icon);
    const auto muteAction = direction + (it->muted ? "取消静音" : "静音");
    mute->setAccessibleName(muteAction);
    detailMute->setAccessibleName(muteAction);
    const auto muteStatus = direction + (it->muted ? "当前已静音" : "当前未静音");
    mute->setToolTip(muteStatus);
    detailMute->setToolTip(muteStatus);
}
void MainWindow::updateMonitor() {
    if (!taskbarPreviewTimer_.isActive())
        taskbarPreviewTimer_.start();
    const auto selected = monitorSelector_->currentData().toString();
    auto it = std::find_if(service_->snapshot().monitors.begin(), service_->snapshot().monitors.end(),
                           [&selected](const auto& m) { return qs(m.id) == selected; });
    const bool valid = it != service_->snapshot().monitors.end() && it->brightnessReadable;
    const bool present = it != service_->snapshot().monitors.end();
    monitorSelector_->setVisible(!monitorCard_->isExpanded() || monitorSelector_->count() != 1);
    monitorCard_->setTitle(present ? qs(it->name) : "等待识别显示器");
    service_->selectMonitorControlsTarget(present ? selected : QString{});
    if (present && !service_->monitorControlsReadAttempted())
        service_->requestMonitorControls(selected);
    service_->selectBrightnessTarget(valid ? selected : QString{});
    monitorSlider_->setEnabled(valid);
    monitorDetails_->setAvailable(valid);
    if (!valid) {
        const QSignalBlocker block(monitorSlider_);
        monitorSlider_->setSliderDown(false);
        monitorValue_->setText("—");
        monitorDetails_->setDescription(it == service_->snapshot().monitors.end()
                                            ? "请先在常用视图中选择显示器。"
                                            : "当前连接的亮度不可读，请检查 DDC/CI 后更新状态。");
        return;
    }
    settings_.setValue("monitor/selected", selected);
    if (!monitorSlider_->isSliderDown() && !service_->brightnessUpdatePending(selected)) {
        QSignalBlocker block(monitorSlider_);
        monitorSlider_->setValue(it->brightnessPercent);
        monitorValue_->setText(QString::number(it->brightnessPercent) + "%");
        if (!monitorDetails_->isEditing())
            monitorDetails_->setValue(it->brightnessPercent);
    }
    monitorDetails_->setDescription(qs(it->name) + " · DDC/CI\n数值为 0–100%；实际响应速度取决于显示器。");
}

void MainWindow::updateMonitorControls() {
    const auto& info = service_->monitorControls();
    if (info.id.empty()) {
        monitorControls_->clear();
        return;
    }
    monitorControls_->setInfo(info);
    monitorControls_->setBusy(service_->monitorControlsBusy());
    if (service_->monitorControlsBusy())
        monitorControls_->setStatus("正在读取或设置显示器功能…");
    profileTracker_->updateMonitorControls();
}

void MainWindow::updateProfiles() {
    profileTracker_->update(devices_);
}

void MainWindow::rememberSetting(const QString& id, const QString& key, int value) {
    profileTracker_->remember(id, key, value);
}
void MainWindow::requestManualRefresh() {
    if (manualRefreshTicket_)
        return;
    manualRefreshMessageUntil_ = 0;
    refreshButton_->setText("正在刷新…");
    refreshButton_->setEnabled(false);
    notice_->setText("正在重新读取设备状态…");
    notice_->setToolTip({});
    manualRefreshTicket_ = service_->requestRefresh();
    // 新的手动刷新替代待完成扫描时，删档核对也必须等待这一轮完整结果。
    if (manualRefreshTicket_ && !deletingProfileId_.isEmpty())
        deleteProfileTicket_ = manualRefreshTicket_;
    if (!manualRefreshTicket_) {
        refreshButton_->setEnabled(true);
        refreshButton_->setText("刷新状态");
        notice_->setText("刷新未启动，程序正在退出。");
    }
}

void MainWindow::updateSavedProfilesView() {
    const auto* current = savedTree_->currentItem();
    const auto currentId = current ? current->data(0, profileIdRole).toString() : QString{};
    const QString selected = currentId.isEmpty() ? savedDetailProfileId_ : currentId;
    const int scroll = savedTree_->verticalScrollBar()->value();
    const auto profiles = profiles_.profiles();
    const QSignalBlocker block(savedTree_);
    QMap<QString, QList<SavedDeviceProfile>> grouped;
    QMap<QString, int> counts;
    QSet<QString> ids;
    for (const auto& profile : profiles) {
        const auto key = profileGroup(profile);
        grouped[key].append(profile);
        ++counts[key];
        if (profile.category == "audio")
            ++counts["audio"];
        ids.insert(profile.id);
    }
    for (auto it = savedItems_.begin(); it != savedItems_.end();) {
        if (!ids.contains(it.key())) {
            delete it.value();
            it = savedItems_.erase(it);
        } else {
            ++it;
        }
    }
    const auto ensureGroup = [this, &counts](const QString& key, const QString& name, QTreeWidgetItem* parent,
                                             int row) {
        auto*& item = savedGroups_[key];
        if (!item) {
            item = new QTreeWidgetItem;
            item->setData(0, profileGroupRole, key);
            item->setFlags(Qt::ItemIsEnabled);
            auto font = savedTree_->font();
            font.setWeight(QFont::DemiBold);
            item->setFont(0, font);
            item->setIcon(0, fluentIcon(categoryIcon(key)));
        }
        item->setText(0, QString("%1（%2）").arg(name).arg(counts.value(key)));
        placeTreeItem(savedTree_, item, parent, row);
        return item;
    };
    int topRow = 0;
    for (const auto& key :
         {QString("mouse"), QString("keyboard"), QString("monitor"), QString("audio"), QString("other")}) {
        if (!counts.value(key))
            continue;
        auto* parent = ensureGroup(key, key == "other" ? "其他设备" : categoryText(key), nullptr, topRow++);
        if (key == "audio") {
            int row = 0;
            for (const auto& direction : {QString("output"), QString("input"), QString("unknown")}) {
                const auto sub = "audio/" + direction;
                if (counts.value(sub))
                    ensureGroup(sub, audioDirectionText(direction), parent, row++);
            }
        }
    }
    // 同 ID 叶子和分组在普通刷新中持续复用，方向补全时只移动对应叶子。
    for (auto group = grouped.cbegin(); group != grouped.cend(); ++group) {
        int row = 0;
        for (const auto& profile : group.value()) {
            auto*& item = savedItems_[profile.id];
            if (!item) {
                item = new QTreeWidgetItem;
                item->setData(0, profileIdRole, profile.id);
            }
            item->setIcon(0, fluentIcon(categoryIcon(profileGroup(profile))));
            const QString state = !profile.connected                                         ? "未接入"
                                  : categoryReadable(profile.category, service_->snapshot()) ? "已检测到接口"
                                                                                             : "读取暂不可用";
            item->setText(0, profile.name + "\n" + state);
            item->setToolTip(0, plainTextToolTip(profile.name + " · " + state));
            placeTreeItem(savedTree_, item, savedGroups_.value(group.key()), row++);
        }
    }
    auto unusedGroups = savedGroups_.keys();
    std::sort(unusedGroups.begin(), unusedGroups.end(),
              [](const auto& a, const auto& b) { return a.size() > b.size(); });
    for (const auto& key : unusedGroups)
        if (!counts.value(key))
            delete savedGroups_.take(key);
    auto* selectedItem = savedItems_.value(selected, nullptr);
    if (!selectedItem) {
        // 首次进入只默认选择可见叶子，避免控件显示时为隐藏选中项延迟展开祖先。
        const auto visibleByPreference = [this](const QTreeWidgetItem* item) {
            for (auto* parent = item->parent(); parent; parent = parent->parent())
                if (!settings_.value("saved/groups/" + parent->data(0, profileGroupRole).toString(), true)
                         .toBool())
                    return false;
            return true;
        };
        for (QTreeWidgetItemIterator it(savedTree_); *it; ++it)
            if (!(*it)->data(0, profileIdRole).toString().isEmpty() && visibleByPreference(*it)) {
                selectedItem = *it;
                break;
            }
    }
    savedTree_->setCurrentItem(selectedItem);
    // 恢复选中项可能自动展开祖先；在此后恢复用户明确的展开/收起偏好。
    for (auto it = savedGroups_.cbegin(); it != savedGroups_.cend(); ++it)
        it.value()->setExpanded(settings_.value("saved/groups/" + it.key(), true).toBool());
    savedTree_->doItemsLayout();
    savedTree_->verticalScrollBar()->setValue(scroll);
    showSavedProfile();
}

void MainWindow::showSavedProfile() {
    updateProfileDeleteControls();
    const auto* selected = savedTree_->currentItem();
    const auto id = selected ? selected->data(0, profileIdRole).toString() : QString{};
    if (selected && id.isEmpty())
        return; // 类别仅用于展开收起，继续保留已选设备详情。
    const int scroll = id == savedDetailProfileId_ ? savedDetail_->verticalScrollBar()->value() : 0;
    savedDetailProfileId_ = id;
    const auto profile = selected ? profiles_.find(id) : std::nullopt;
    if (!profile) {
        savedDetail_->setPlainText(profiles_.errorString().isEmpty()
                                       ? "接入设备后，将在这里保留它的最后设置。"
                                       : "档案读取异常，原文件已保留：\n" + profiles_.errorString());
        return;
    }
    QString text = profile->name + "\n" + categoryText(profile->category) + "\n\n";
    if (profile->category == "audio")
        text = profile->name + "\n音频设备 · " + audioDirectionText(profile->direction) + "\n\n";
    text += !profile->connected ? "状态：未接入\n"
            : categoryReadable(profile->category, service_->snapshot())
                ? "状态：已检测到接口\n"
                : "状态：读取暂不可用，保留上次记录\n";
    text += "最后检测：" +
            profile->lastSeen.toTimeZone(QTimeZone("Asia/Shanghai")).toString("yyyy-MM-dd HH:mm:ss") + "\n\n";
    const QMap<QString, QString> names{{"brightness", "亮度"},
                                       {"volume", "音量"},
                                       {"mute", "静音"},
                                       {"vcp_12", "对比度"},
                                       {"vcp_dc", "情景代码"},
                                       {"vcp_14", "色彩预设代码"},
                                       {"vcp_60", "输入源代码"},
                                       {"battery", "电量（最后读取）"},
                                       {"charging", "充电（最后读取）"},
                                       {"n9_eq_mode", "耳机 EQ 模式代码"},
                                       {"n9_sidetone", "耳机耳返标志"},
                                       {"n9_sidetone_volume", "耳机耳返音量原值"},
                                       {"n9_surround", "耳机 7.1 标志"},
                                       {"n9_spatial", "耳机空间音效标志"},
                                       {"n9_rgb", "耳机 RGB 标志"},
                                       {"n9_autooff", "耳机自动关机代码"}};
    const QMap<QString, QString> mouseNames{{"vgn_rate_hz", "鼠标回报率（Hz）"},
        {"vgn_stage_count", "DPI 档位数"}, {"vgn_active_stage", "当前 DPI 档位"},
        {"vgn_lod_code", "LOD 代码"}, {"vgn_debounce_ms", "按键去抖（ms）"},
        {"vgn_sleep_seconds", "休眠参数（s）"}, {"vgn_motion_sync", "Motion Sync"},
        {"vgn_angle_snap", "直线修正"}, {"vgn_ripple", "波纹修正"}, {"vgn_performance_code", "性能代码"}};
    if (profile->settings.isEmpty())
        text += "暂未取得可保存的设置；电量、DPI、按键或 EQ 不会使用模拟值。\n";
    for (auto it = profile->settings.cbegin(); it != profile->settings.cend(); ++it) {
        QString value = QString::number(it.value());
        if (it.key() == "brightness" || it.key() == "volume" || it.key() == "battery")
            value += "%";
        else if (it.key() == "mute" || it.key() == "charging")
            value = it.value() ? "开启" : "关闭";
        else if (it.key().startsWith("vcp_") && it.key() != "vcp_12")
            value = QString("0x%1").arg(it.value(), 2, 16, QChar('0'));
        QString name = names.value(it.key(), it.key());
        if (it.key().startsWith("vgn_")) {
            name = mouseNames.value(it.key(), it.key());
            if (it.key() == "vgn_active_stage")
                value = QString::number(it.value() + 1);
            if (it.key().startsWith("vgn_dpi_"))
                name = QString("DPI 档位 %1 · %2").arg(it.key().mid(8,1).toInt() + 1).arg(it.key().right(1).toUpper());
            if (it.key().startsWith("vgn_key_")) {
                name = QString("按键 %1 · %2").arg(it.key().mid(8,1).toInt() + 1)
                    .arg(it.key().endsWith("_class") ? QStringLiteral("类型") : QStringLiteral("参数"));
                if (it.key().endsWith("_param"))
                    value = QString("0x%1").arg(it.value(), 4, 16, QLatin1Char('0'));
            }
        }
        if (it.key().startsWith("n9_peq_")) {
            const auto suffix = it.key().mid(8);
            const QMap<QString, QString> fields{{"_hz", "频率"}, {"_gain10", "增益"},
                {"_q10", "Q 值"}, {"_type", "滤波类型代码"}, {"_enabled", "启用标志"}};
            name = QString("PEQ %1 · %2").arg(it.key().mid(7, 1).toInt() + 1).arg(fields.value(suffix, suffix));
            if (suffix == "_gain10" || suffix == "_q10")
                value = QString::number(it.value() / 10.0, 'f', 1) + (suffix == "_gain10" ? " dB" : "");
            else if (suffix == "_hz")
                value += " Hz";
        }
        text += name + "：" + value +
                (profile->confirmedSettings.contains(it.key()) ? "（已读取确认）\n" : "（目标待确认）\n");
    }
    const auto message = profileTracker_->messageFor(profile->id);
    if (!message.isEmpty())
        text += "\n最近恢复：" + message + "\n";
    text += preferences_.restoreOnReconnect
                ? "\n重连自动恢复已确认的亮度、音量和可用情景。输入源保持当前选择；其他记录仅供查看。"
                : "\n自动恢复已关闭；后续重连只读取设备当前参数并保存。输入源保持当前选择。";
    if (savedDetail_->toPlainText() != text)
        savedDetail_->setPlainText(text);
    savedDetail_->verticalScrollBar()->setValue(scroll);
}

void MainWindow::updateProfileDeleteControls() {
    const auto* selected = savedTree_->currentItem();
    const auto id = selected ? selected->data(0, profileIdRole).toString() : QString{};
    const bool pending = !deletingProfileId_.isEmpty();
    const auto reason = profileTracker_ ? profileTracker_->removalBlockReason(id)
                                        : QStringLiteral("请等待设备状态读取完成。");
    deleteProfileButton_->setEnabled(!pending && reason.isEmpty());
    deleteProfileButton_->setText(pending ? "正在确认断开…" : "删除设备档案");
    deleteProfileHint_->setText(
        pending ? QStringLiteral("正在重新检测“%1”是否已断开…").arg(deletingProfileName_)
        : !reason.isEmpty() ? reason
                           : QStringLiteral("设备已断开。删除后，重新接入将重新读取型号、功能和当前参数。"));
    deleteProfileButton_->setToolTip(deleteProfileHint_->text());
}

void MainWindow::requestProfileDeletion() {
    if (!deletingProfileId_.isEmpty())
        return;
    const auto* selected = savedTree_->currentItem();
    const auto id = selected ? selected->data(0, profileIdRole).toString() : QString{};
    if (!profileTracker_->removalBlockReason(id).isEmpty()) {
        updateProfileDeleteControls();
        return;
    }
    const auto profile = profiles_.find(id);
    if (!profile)
        return;
    // 固定确认时的 ID，不能因后台重排或用户选择变化而删掉另一项。
    const auto name = profile->name;
    FrostedDialog dialog(this);
    dialog.setObjectName("confirmProfileDeletion");
    dialog.setWindowTitle("删除设备档案");
    dialog.setMinimumWidth(440);
    auto* layout = dialog.contentLayout();
    layout->setSpacing(16);
    auto* question = label(QStringLiteral("删除“%1”的档案？").arg(name));
    question->setTextFormat(Qt::PlainText);
    layout->addWidget(question);
    layout->addWidget(label("将清除该设备在本软件中保存的名称和参数。重新接入后，会读取设备当前设置并建立新档案。", "muted"));
    auto* buttons = new QDialogButtonBox;
    auto* cancel = buttons->addButton("取消", QDialogButtonBox::RejectRole);
    auto* confirm = buttons->addButton("删除档案", QDialogButtonBox::AcceptRole);
    confirm->setObjectName("confirmDeleteDeviceProfile");
    cancel->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    deletingProfileId_ = id;
    deletingProfileName_ = name;
    deleteProfileResult_->clear();
    deleteProfileResult_->hide();
    updateProfileDeleteControls();
    // 确认框打开期间可能重新插入设备，必须重新扫描后才允许提交删除。
    deleteProfileTicket_ = service_->requestRefresh();
    if (manualRefreshTicket_ && deleteProfileTicket_)
        manualRefreshTicket_ = deleteProfileTicket_;
    if (!deleteProfileTicket_) {
        deletingProfileId_.clear();
        deletingProfileName_.clear();
        deleteProfileResult_->setText("删除未启动，程序正在退出；档案已保留。");
        deleteProfileResult_->show();
        updateProfileDeleteControls();
    }
}

void MainWindow::finishProfileDeletion(quint64 ticket) {
    if (!deleteProfileTicket_ || ticket != deleteProfileTicket_)
        return;
    const auto id = std::exchange(deletingProfileId_, {});
    const auto name = std::exchange(deletingProfileName_, {});
    deleteProfileTicket_ = 0;
    QString error;
    const bool removed = profileTracker_->removeProfile(id, error);
    updateSavedProfilesView();
    deleteProfileResult_->setText(removed ? QStringLiteral("已删除“%1”的档案。重新接入后将建立新档案。").arg(name)
                                         : QStringLiteral("未删除“%1”：%2").arg(name, error));
    deleteProfileResult_->show();
}

void MainWindow::bindMouseConnections() {
    FrostedDialog dialog(this);
    dialog.setObjectName("mouseBindingDialog");
    dialog.setWindowTitle("关联鼠标连接方式");
    dialog.setMinimumWidth(480);
    auto* layout = dialog.contentLayout();
    layout->addWidget(
        label("当接收器和数据线属于同一只鼠标时，将它们关联。之后插拔数据线会保留同一设备档案。"));
    auto* receiver = createThemedComboBox();
    receiver->setObjectName("mouseBindingReceiver");
    auto* wired = createThemedComboBox();
    wired->setObjectName("mouseBindingWired");
    for (const auto& d : devices_) {
        if (d.kind != DeviceKind::Mouse)
            continue;
        const auto name = qs(d.name) + " · " + transportText(d) + " · " + qs(d.logicalId).right(12);
        if (d.receiverPresent)
            receiver->addItem(name, qs(d.logicalId));
        if (d.wiredPresent)
            wired->addItem(name, qs(d.logicalId));
    }
    layout->addWidget(label("2.4G 接收器"));
    layout->addWidget(receiver);
    layout->addWidget(label("有线连接"));
    layout->addWidget(wired);
    layout->addWidget(label(
        "首次关联时请同时接上数据线和接收器。没有共同设备标识时，需要你确认它们属于同一只鼠标。", "muted"));
    auto* error = label({}, "status");
    error->setObjectName("mouseBindingError");
    error->setTextFormat(Qt::PlainText);
    error->hide();
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("确认为同一只鼠标");
    buttons->button(QDialogButtonBox::Ok)->setObjectName("confirmMouseBinding");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    buttons->button(QDialogButtonBox::Ok)->setEnabled(receiver->count() && wired->count());
    // 对话框打开期间设备可能被拔掉；确认时用最新一次完整枚举重新核对两个通道，
    // 而不是沿用打开对话框时的列表（0.2.0 审计第 1 项）。
    const auto selection = [this, receiver, wired] {
        return resolveMouseBinding(devices_, receiver->currentData().toString(),
                                   wired->currentData().toString());
    };
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, &selection, error] {
        const auto result = selection();
        if (!result) {
            error->setText(result.error);
            error->show();
            return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    // accept 前已校验一次；这里再取一次同一结果，避免把有效期外推。
    const auto result = selection();
    if (!result) {
        showOperation(result.error, false);
        return;
    }
    const auto paths = result.endpointPaths;
    std::vector<std::string> ids;
    for (const auto& path : paths)
        ids.push_back(path.toStdString());
    const auto logicalId = "user:f2:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!registry_.bindEndpoints(ids, logicalId.toStdString())) {
        showOperation("设备关联失败，请刷新后重试", false);
        return;
    }
    bindings_ = paths;
    settings_.setValue("mouse/boundEndpoints", bindings_);
    settings_.setValue("mouse/bindingId", logicalId);
    settings_.sync();
    updateDevices();
    mouseSelector_->setCurrentIndex(mouseSelector_->findData(logicalId));
    showOperation("已关联连接方式", true);
}

QString MainWindow::detailsText() const {
    QString text = "设备读取详情（仅保存于本机）\n\n";
    const auto& snapshotData = service_->snapshot();
    text += QString("扫描时间：%1\nHID 接口：%2\n\n")
                .arg(QDateTime::fromMSecsSinceEpoch(snapshotData.sampledMs).toString(Qt::ISODate))
                .arg(snapshotData.hid.size());
    for (const auto& h : snapshotData.hid) {
        text += QString("%1 %2 | %3:%4 | Usage %5:%6 | IN/OUT/FEATURE %7/%8/%9\n")
                    .arg(qs(h.manufacturer), qs(h.product))
                    .arg(h.vid, 4, 16, QChar('0'))
                    .arg(h.pid, 4, 16, QChar('0'))
                    .arg(h.usagePage, 4, 16, QChar('0'))
                    .arg(h.usage, 4, 16, QChar('0'))
                    .arg(h.inputReportBytes)
                    .arg(h.outputReportBytes)
                    .arg(h.featureReportBytes);
        text += "Container: " + qs(h.containerId) + "\nSerial: " + qs(h.serial) + "\n";
        if (!h.error.empty())
            text += "读取说明: " + qs(h.error) + "\n";
    }
    text += "\n鼠标协议查询\n";
    for (const auto& [path, reading] : snapshotData.batteries) {
        const auto& b = reading.value;
        text += QString("在线已确认=%1，在线=%2，电量有效=%3")
                    .arg(b.onlineKnown)
                    .arg(b.deviceOnline)
                    .arg(b.batteryValid);
        if (b.batteryValid)
            text += QString("，电量=%1%，充电=%2，原始电压=%3mV")
                        .arg(b.percentage)
                        .arg(b.charging)
                        .arg(b.voltageMillivolts);
        text += "\n" + qs(b.error) + "\n";
    }
    text += "\n显示器\n";
    for (const auto& m : snapshotData.monitors)
        text += QString("%1 | %2 | 亮度可读=%3，原始范围=%4..%5，当前=%6\n%7\n")
                    .arg(qs(m.name), qs(m.id))
                    .arg(m.brightnessReadable)
                    .arg(m.minBrightness)
                    .arg(m.maxBrightness)
                    .arg(m.currentBrightness)
                    .arg(qs(m.error));
    text += "\n音频\n";
    for (const auto& a : snapshotData.audio)
        text += (a.capture ? "录音 " : "播放 ") + qs(a.name) + "\n" + qs(a.error) + "\n";
    if (!snapshotData.errors.isEmpty())
        text += "\n扫描错误\n" + snapshotData.errors.join('\n');
    return text;
}
void MainWindow::showDetails() {
    FrostedDialog dialog(this);
    dialog.setWindowTitle("设备详情");
    dialog.resize(850, 590);
    auto* layout = dialog.contentLayout();
    auto* heading = new QHBoxLayout;
    heading->setSpacing(12);
    heading->addWidget(glyph(UiIcon::Details));
    heading->addWidget(label("设备读取详情", "sectionTitle"), 1);
    layout->addLayout(heading);
    layout->addWidget(label("查看当前接口与读取状态，诊断信息仅保存在本机。", "muted"));
    auto* text = new QPlainTextEdit(detailsText());
    text->setProperty("diagnostic", true);
    text->setReadOnly(true);
    layout->addWidget(text, 1);
    auto* close = new QPushButton("关闭");
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(close, 0, Qt::AlignRight);
    dialog.exec();
}
void MainWindow::showOperation(const QString& message, bool success) {
    notice_->setText(message);
    notice_->setToolTip(plainTextToolTip(message));
    if (!success)
        showThemedMessage(this, "操作未完成", message);
}
void MainWindow::setupTray() {
    buildTrayMenu();
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    tray_ = new QSystemTrayIcon(windowIcon(), this);
    tray_->setToolTip("外设中心");
    tray_->setContextMenu(trayMenu_);
    connect(tray_, &QSystemTrayIcon::activated, this, [this](auto reason) {
        // 单击托盘图标即打开/唤醒窗口；右键仍由 Qt 弹出上下文菜单。
        if (reason == QSystemTrayIcon::Trigger) {
            showNormal();
            raise();
            activateWindow();
        }
    });
    tray_->show();
}

void MainWindow::buildTrayMenu() {
    if (trayMenu_)
        return;
    trayMenu_ = new QMenu(this);
    trayMenu_->setObjectName("trayMenu");
    trayMenu_->setMinimumWidth(320);
    trayMenu_->setToolTipsVisible(true);

    auto* header = new QWidget;
    header->setObjectName("trayHeader");
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(14, 12, 14, 8);
    headerLayout->setSpacing(4);
    auto* title = new QLabel("外设中心");
    title->setObjectName("trayTitle");
    title->setTextFormat(Qt::PlainText);
    trayBatteryStatus_ = new QLabel("正在读取设备状态…");
    trayBatteryStatus_->setObjectName("trayBatteryStatus");
    trayBatteryStatus_->setTextFormat(Qt::PlainText);
    trayBatteryStatus_->setWordWrap(true);
    headerLayout->addWidget(title);
    headerLayout->addWidget(trayBatteryStatus_);
    auto* headerAction = new QWidgetAction(trayMenu_);
    headerAction->setDefaultWidget(header);
    trayMenu_->addAction(headerAction);
    trayMenu_->addSeparator();

    const auto addControl = [this](UiIcon icon, const QString& title, const QString& key, bool audio,
                                   bool capture, QSlider*& slider, QLabel*& value,
                                   QToolButton*& mute) {
        auto* row = new QWidget;
        row->setObjectName("trayControl");
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(14, 4, 14, 4);
        layout->setSpacing(8);
        auto* glyphLabel = new QLabel;
        glyphLabel->setFixedSize(20, 20);
        glyphLabel->setScaledContents(true);
        glyphLabel->setPixmap(fluentIcon(icon, QColor("#45638e")).pixmap(QSize(40, 40)));
        layout->addWidget(glyphLabel);
        auto* name = new QLabel(title);
        name->setObjectName("trayControlLabel");
        name->setFixedWidth(38);
        layout->addWidget(name);
        slider = createThemedSlider(Qt::Horizontal);
        slider->setRange(0, 100);
        slider->setObjectName(key + QStringLiteral("Slider"));
        slider->setMinimumWidth(112);
        slider->setAccessibleName(title);
        layout->addWidget(slider, 1);
        value = new QLabel(QStringLiteral("—"));
        value->setObjectName(key + QStringLiteral("Value"));
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setFixedWidth(40);
        layout->addWidget(value);
        if (audio) {
            mute = new QToolButton;
            mute->setObjectName(key + QStringLiteral("Mute"));
            mute->setFixedSize(30, 30);
            mute->setAutoRaise(true);
            mute->setIconSize(QSize(18, 18));
            mute->setCursor(Qt::PointingHandCursor);
            mute->setToolTip(title + QStringLiteral("静音"));
            mute->setAccessibleName(title + "静音");
            connect(mute, &QToolButton::clicked, this, [this, capture] { toggleTrayMute(capture); });
            layout->addWidget(mute);
        } else {
            layout->addSpacing(26);
        }
        auto* action = new QWidgetAction(trayMenu_);
        action->setDefaultWidget(row);
        trayMenu_->addAction(action);
        connect(slider, &QSlider::valueChanged, this,
                [this, value, audio, capture](int percent) {
                    value->setText(QStringLiteral("%1%").arg(percent));
                    applyTrayLive(!audio, capture, percent);
                });
        connect(slider, &QSlider::sliderReleased, this,
                [this, audio, capture] { finishTrayLive(!audio, capture); });
    };
    QToolButton* unusedMute = nullptr;
    addControl(UiIcon::BrightnessHigh, "亮度", "trayBrightness", false, false,
               trayBrightnessSlider_, trayBrightnessValue_, unusedMute);
    QToolButton* outputMute = nullptr;
    addControl(UiIcon::Speaker, "输出", "trayOutput", true, false, trayOutputSlider_,
               trayOutputValue_, outputMute);
    trayOutputMuteButton_ = outputMute;
    QToolButton* inputMute = nullptr;
    addControl(UiIcon::Mic, "输入", "trayInput", true, true, trayInputSlider_, trayInputValue_,
               inputMute);
    trayInputMuteButton_ = inputMute;

    trayMenu_->addSeparator();
    auto* open = trayMenu_->addAction(fluentIcon(UiIcon::Apps), "打开外设中心", this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    open->setObjectName("trayOpen");
    auto* refresh =
        trayMenu_->addAction(fluentIcon(UiIcon::Refresh), "刷新状态", this, &MainWindow::requestManualRefresh);
    refresh->setObjectName("trayRefresh");
    auto* settings = trayMenu_->addAction(fluentIcon(UiIcon::Settings), "设置", this, [this] {
        settingsNavigation_->click();
        showNormal();
        raise();
        activateWindow();
    });
    settings->setObjectName("traySettings");
    trayMenu_->addSeparator();
    auto* quitAction = new QAction(fluentIcon(UiIcon::Exit), QStringLiteral("退出"), this);
    quitAction->setObjectName("trayQuit");
    quitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));
    addAction(quitAction);
    connect(quitAction, &QAction::triggered, this, [this] {
        profileTracker_->flush();
        quitting_ = true;
        QApplication::quit();
    });
    trayMenu_->addAction(quitAction);
    connect(trayMenu_, &QMenu::aboutToShow, this, &MainWindow::refreshTrayMenu);
}

void MainWindow::refreshTrayMenu() {
    if (!trayMenu_ || !trayBatteryStatus_)
        return;
    const auto& snapshot = service_->snapshot();
    const auto findDevice = [this](const QString& id) -> const DeviceSnapshot* {
        for (const auto& device : devices_)
            if (qs(device.logicalId) == id)
                return &device;
        return nullptr;
    };

    const DeviceSnapshot* mouse = findDevice(mouseSelector_->currentData().toString());
    if (!mouse)
        for (const auto& device : devices_)
            if (device.kind == DeviceKind::Mouse && !device.endpoints.empty()) {
                mouse = &device;
                break;
            }
    QString mouseLine = "鼠标：电量未知";
    if (mouse && mouse->battery.reportedPercent) {
        mouseLine = QStringLiteral("鼠标：%1%%2")
                        .arg(*mouse->battery.reportedPercent)
                        .arg(mouse->stale ? QStringLiteral("（上次读数）") : QString{});
        if (mouse->battery.charging && !mouse->stale)
            mouseLine += *mouse->battery.charging ? QStringLiteral(" · 充电中")
                                                  : QStringLiteral(" · 未充电");
    }
    QString headsetLine = "耳机：电量未知";
    for (const auto& entry : snapshot.headsets) {
        const auto& reading = entry.second;
        if (!reading.value.status)
            continue;
        const auto& status = *reading.value.status;
        const QString kind = status.batteryKind == n9::BatteryKind::Charging
                                 ? QStringLiteral("充电中")
                             : status.batteryKind == n9::BatteryKind::Full ? QStringLiteral("已充满")
                                                                           : QStringLiteral("未充电");
        const bool stale = reading.sampledMonotonicMs > 0 &&
                           monotonicNowMs() - reading.sampledMonotonicMs > 45000;
        headsetLine = QStringLiteral("耳机：%1% · %2%3")
                          .arg(status.batteryPercent)
                          .arg(kind, stale ? QStringLiteral(" · 上次读数") : QString{});
        break;
    }
    trayBatteryStatus_->setText(mouseLine + QLatin1Char('\n') + headsetLine);

    const auto updateSlider = [](QSlider* slider, QLabel* value, bool ready, int percent,
                                 const QString& toolTip) {
        if (!slider || !value)
            return;
        const QSignalBlocker block(slider);
        slider->setEnabled(ready);
        slider->setValue(ready ? std::clamp(percent, 0, 100) : 0);
        value->setText(ready ? QStringLiteral("%1%").arg(std::clamp(percent, 0, 100))
                             : QStringLiteral("—"));
        slider->setToolTip(ready ? toolTip : QString());
    };

    const auto monitorId = monitorSelector_->currentData().toString();
    const platform::MonitorInfo* monitor = nullptr;
    for (const auto& item : snapshot.monitors)
        if (qs(item.id) == monitorId)
            monitor = &item;
    const bool brightnessReady = monitor && monitor->brightnessReadable;
    updateSlider(trayBrightnessSlider_, trayBrightnessValue_, brightnessReady,
                 brightnessReady ? monitor->brightnessPercent : 0,
                 brightnessReady ? qs(monitor->name) + QStringLiteral(" · DDC/CI") : QString{});

    const auto updateAudio = [&](bool capture) {
        auto* combo = capture ? audioInputSelector_ : audioSelector_;
        QSlider* slider = capture ? trayInputSlider_ : trayOutputSlider_;
        QLabel* value = capture ? trayInputValue_ : trayOutputValue_;
        QToolButton* mute = capture ? trayInputMuteButton_ : trayOutputMuteButton_;
        const auto id = combo->currentData().toString();
        const platform::AudioEndpointInfo* endpoint = nullptr;
        for (const auto& item : snapshot.audio)
            if (item.capture == capture && qs(item.id) == id && item.volumeReadable) {
                endpoint = &item;
                break;
            }
        const int percent = endpoint ? static_cast<int>(std::lround(endpoint->volumePercent)) : 0;
        updateSlider(slider, value, endpoint != nullptr, percent,
                     endpoint ? qs(endpoint->name) : QString{});
        if (!mute)
            return;
        bool muted = endpoint && endpoint->muted;
        const auto overrideIt = trayMuteOverrides_.find(id);
        if (overrideIt != trayMuteOverrides_.end() && endpoint) {
            const bool confirmed = endpoint->muted == overrideIt->first;
            if (confirmed || monotonicNowMs() - overrideIt->second > 5000)
                trayMuteOverrides_.erase(overrideIt);
            else
                muted = overrideIt->first;
        }
        mute->setEnabled(endpoint != nullptr);
        mute->setIcon(fluentIcon(endpoint && muted
                                     ? (capture ? UiIcon::MicOff : UiIcon::SpeakerMute)
                                     : (capture ? UiIcon::Mic : UiIcon::Speaker),
                                 QColor("#45638e")));
        mute->setToolTip(endpoint ? (muted ? QStringLiteral("取消静音") : QStringLiteral("静音"))
                                  : QStringLiteral("未选择设备"));
        mute->setAccessibleName(capture ? QStringLiteral("输入静音") : QStringLiteral("输出静音"));
    };
    updateAudio(false);
    updateAudio(true);
}

void MainWindow::applyTrayLive(bool brightness, bool capture, int percent) {
    const auto id = brightness ? monitorSelector_->currentData().toString()
                               : (capture ? audioInputSelector_ : audioSelector_)->currentData().toString();
    if (id.isEmpty())
        return;
    if (brightness) {
        rememberSetting("monitor:" + id, "brightness", percent);
        service_->setFeature(id, DeviceFeature::Brightness, percent);
    } else {
        rememberSetting("audio:" + id, "volume", percent);
        service_->setFeature(id, DeviceFeature::Volume, percent);
    }
}

void MainWindow::finishTrayLive(bool brightness, bool capture) {
    if (brightness) {
        service_->finishBrightness();
        return;
    }
    service_->finishVolume((capture ? audioInputSelector_ : audioSelector_)->currentData().toString());
}

void MainWindow::toggleTrayMute(bool capture) {
    const auto id = (capture ? audioInputSelector_ : audioSelector_)->currentData().toString();
    if (id.isEmpty())
        return;
    for (const auto& endpoint : service_->snapshot().audio) {
        if (endpoint.capture != capture || qs(endpoint.id) != id)
            continue;
        bool currentMuted = endpoint.muted;
        const auto overrideIt = trayMuteOverrides_.find(id);
        if (overrideIt != trayMuteOverrides_.end() && monotonicNowMs() - overrideIt->second <= 5000)
            currentMuted = overrideIt->first;
        const bool nextMuted = !currentMuted;
        trayMuteOverrides_.insert(id, {nextMuted, monotonicNowMs()});
        rememberSetting("audio:" + id, "mute", nextMuted ? 1 : 0);
        service_->setFeature(id, DeviceFeature::Mute, nextMuted ? 1 : 0);
        if (auto* button = capture ? trayInputMuteButton_ : trayOutputMuteButton_) {
            button->setIcon(fluentIcon(nextMuted ? (capture ? UiIcon::MicOff : UiIcon::SpeakerMute)
                                                 : (capture ? UiIcon::Mic : UiIcon::Speaker),
                                       QColor("#45638e")));
            button->setToolTip(nextMuted ? QStringLiteral("取消静音") : QStringLiteral("静音"));
        }
        break;
    }
}
void MainWindow::closeEvent(QCloseEvent* event) {
    profileTracker_->flush();
    if (tray_ && QSystemTrayIcon::isSystemTrayAvailable() && preferences_.closeToTray && !quitting_) {
        hide();
        event->ignore();
    } else {
        event->accept();
        QApplication::quit();
    }
}

void MainWindow::applyPreferences() {
    service_->setRefreshIntervalSeconds(preferences_.refreshIntervalSeconds);
    service_->setExperimentalWritesEnabled(preferences_.experimentalDeviceWrites);
    profileTracker_->setRestoreOnReconnect(preferences_.restoreOnReconnect);
    if (!preferences_.lowBatteryNotification) {
        lowBatteryNotified_.clear();
        lowBatteryNoticeUntil_ = 0;
    }
    settingsPage_->setPreferences(preferences_);
    savedProfilesHint_->setText(preferences_.restoreOnReconnect
                                   ? "每台设备单独保存最后设置。重连自动恢复已确认参数；输入源沿用当前选择。"
                                   : "每台设备单独保存最后设置。自动恢复已关闭；重连读取设备当前参数。");
    if (pages_->currentIndex() == 1)
        showSavedProfile();
    // 开关变更立即反映到界面，不必等下一次扫描；写入目标仍由 updateMouse 维护。
    if (mouseDetails_)
        mouseDetails_->setWritable(preferences_.experimentalDeviceWrites && !vgnWriteTarget_.isEmpty());
}

void MainWindow::changePreferences(const AppPreferences& preferences) {
    QString error;
    if (!appSettings_.save(preferences, error)) {
        settingsPage_->setPreferences(preferences_);
        settingsPage_->setSaveStatus(error, false);
        return;
    }
    preferences_ = preferences;
    applyPreferences();
    settingsPage_->setSaveStatus("程序设置已保存并生效。", true);
}

void MainWindow::openSettingsLocation(bool log) {
    const auto directory = QFileInfo(settings_.fileName()).absolutePath();
    const auto path = log ? directory + "/peripheral-hub.log" : directory;
    if (!QFileInfo::exists(path)) {
        settingsPage_->setSaveStatus(log ? "日志尚未生成。" : "数据目录尚未创建。", false);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        settingsPage_->setSaveStatus("无法打开本地位置：" + QDir::toNativeSeparators(path), false);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (!chromeFrameReady_ && QGuiApplication::platformName() == "windows") {
        chromeFrameReady_ = true;
        initializeGlassFrame(this);
        if (const auto* display = screen()) {
            const QRect available = display->availableGeometry();
            const QMargins frame = windowHandle()->frameMargins();
            const QSize maximumClient =
                (available.size() - QSize(frame.left() + frame.right(), frame.top() + frame.bottom()))
                    .expandedTo(minimumSize());
            resize(size().boundedTo(maximumClient));
            const QRect bounds = frameGeometry();
            const int maximumLeft = (std::max)(available.left(), available.right() - bounds.width() + 1);
            const int maximumTop = (std::max)(available.top(), available.bottom() - bounds.height() + 1);
            move(std::clamp(bounds.left(), available.left(), maximumLeft),
                 std::clamp(bounds.top(), available.top(), maximumTop));
        }
    }
    // 首次显示后才拿得到原生窗口边框；DPI/屏幕切换后也在这里重新核对尺寸。
    if (!screenSignalReady_) {
        screenSignalReady_ = true;
        if (auto* handle = windowHandle())
            connect(handle, &QWindow::screenChanged, this, [this](QScreen*) {
                watchCurrentScreen();
                updateWindowConstraints();
            });
    }
    watchCurrentScreen();
    updateWindowConstraints();
    const bool active = applySystemGlass(this);
    if (!property("systemGlassActive").isValid() || property("systemGlassActive").toBool() != active)
        qInfo() << "Glass theme:" << property("glassCompositionMode") << "active:" << active;
    setProperty("systemGlassActive", active);
    if (!property("compactTaskbarPreviewRequested").isValid())
        setProperty("compactTaskbarPreviewRequested", enableCompactTaskbarPreview(this));
    centralWidget()->update();
    taskbarPreviewTimer_.start();
}

void MainWindow::handleSystemResume() {
    resumeTimer_.start();
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        const auto* native = static_cast<const MSG*>(message);
        if (native && native->message == WM_DWMSENDICONICTHUMBNAIL) {
            const QSize maximum(HIWORD(native->lParam), LOWORD(native->lParam));
            if (presentCompactTaskbarBitmap(this, taskbarPreviewImage(maximum))) {
                *result = 0;
                return true;
            }
        }
        if (native && native->message == WM_DWMSENDICONICLIVEPREVIEWBITMAP) {
            if (presentCompactTaskbarBitmap(this, grab().toImage(), true)) {
                *result = 0;
                return true;
            }
        }
        if (handleGlassFrame(this, eventType, message, result))
            return true;
        if (native && (native->message == WM_DISPLAYCHANGE || native->message == WM_DEVICECHANGE))
            displayChangeTimer_.start();
        if (native && native->message == WM_POWERBROADCAST &&
            (native->wParam == PBT_APMRESUMEAUTOMATIC || native->wParam == PBT_APMRESUMESUSPEND))
            handleSystemResume();
        if (native && (native->message == WM_DWMCOMPOSITIONCHANGED || native->message == WM_THEMECHANGED ||
                       native->message == WM_SETTINGCHANGE))
            QTimer::singleShot(0, this, [this] {
                setProperty("systemGlassActive", applySystemGlass(this));
                centralWidget()->update();
            });
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
QImage MainWindow::taskbarPreviewImage(const QSize& maximum) const {
    const bool mouseOffline = !mousePresent_ || mouseState_->text().contains("离线") ||
                               mouseState_->text().contains("移除");
    const QString mouseValue = mouseOffline ? QStringLiteral("—") : mouseValue_->text();
    const QString mouseDetail = !mousePresent_ ? QStringLiteral("未连接")
                                  : mouseOffline ? QStringLiteral("离线")
                                  : mouseFreshness_->text().contains("上次") ? QStringLiteral("上次读数")
                                  : mouseValue == QStringLiteral("—") ? QStringLiteral("未知") : QString{};
    const std::array<CompactDeviceStatus, 4> statuses{{
        {UiIcon::Cursor, "鼠标电量", mouseValue, mouseDetail},
        {UiIcon::Keyboard, "键盘电量", "—", keyboardPresent_ ? "未知" : "未连接"},
        {UiIcon::Desktop, "显示器亮度", monitorValue_->text(), monitorSelector_->currentText()},
        {UiIcon::Headphones, "输出音量", audioValue_->text(), audioValue_->text() == "—" ? "未知"
            : muteButton_->text() == "取消静音" ? "已静音" : audioSelector_->currentText()}
    }};
    return renderCompactPreview(maximum, statuses);
}

bool MainWindow::saveState(const QString& path) const {
    QJsonObject root;
    root["schemaVersion"] = 1;
    root["ui"] = QJsonObject{{"theme", "frosted-glass"},
                              {"systemBackdropRequested", property("systemGlassActive").toBool()},
                              {"composition", property("glassCompositionMode").toString()},
                              {"customChrome", true},
                              {"compactTaskbarPreviewRequested", property("compactTaskbarPreviewRequested").toBool()}};
    if (const auto* display = screen()) {
        const auto frame = frameGeometry();
        const auto available = display->availableGeometry();
        auto ui = root["ui"].toObject();
        ui["window"] = QJsonObject{{"width", width()}, {"height", height()},
                                   {"frameWidth", frame.width()}, {"frameHeight", frame.height()},
                                   {"availableWidth", available.width()},
                                   {"availableHeight", available.height()},
                                   {"fitsAvailableArea", available.contains(frame)}};
        root["ui"] = ui;
    }
    root["sampledAt"] =
        QDateTime::fromMSecsSinceEpoch(service_->snapshot().sampledMs).toUTC().toString(Qt::ISODate);
    QJsonArray devices;
    for (const auto& d : devices_) {
        QJsonObject item;
        item["id"] = qs(d.logicalId);
        item["name"] = qs(d.name);
        item["linkState"] = linkText(d.linkState);
        item["receiverPresent"] = d.receiverPresent;
        item["wiredPresent"] = d.wiredPresent;
        item["stale"] = d.stale;
        item["batteryPercent"] =
            d.battery.reportedPercent ? QJsonValue(*d.battery.reportedPercent) : QJsonValue(QJsonValue::Null);
        item["charging"] =
            d.battery.charging ? QJsonValue(*d.battery.charging) : QJsonValue(QJsonValue::Null);
        if (d.lastUpdated)
            item["batterySampledAt"] =
                QDateTime::fromMSecsSinceEpoch(
                    std::chrono::duration_cast<std::chrono::milliseconds>(d.lastUpdated->time_since_epoch())
                        .count())
                    .toUTC()
                    .toString(Qt::ISODate);
        item["statusDetail"] = qs(d.statusDetail);
        item["endpointCount"] = static_cast<int>(d.endpoints.size());
        devices.append(item);
    }
    root["devices"] = devices;
    QJsonArray monitors;
    for (const auto& m : service_->snapshot().monitors) {
        QJsonObject item;
        item["name"] = qs(m.name);
        item["brightnessReadable"] = m.brightnessReadable;
        item["brightnessPercent"] =
            m.brightnessReadable ? QJsonValue(m.brightnessPercent) : QJsonValue(QJsonValue::Null);
        item["error"] = qs(m.error);
        monitors.append(item);
    }
    root["monitors"] = monitors;
    QJsonArray audio;
    for (const auto& a : service_->snapshot().audio) {
        QJsonObject item;
        item["name"] = qs(a.name);
        item["capture"] = a.capture;
        item["volumeReadable"] = a.volumeReadable;
        item["volumePercent"] = a.volumeReadable ? QJsonValue(a.volumePercent) : QJsonValue(QJsonValue::Null);
        audio.append(item);
    }
    root["audioEndpoints"] = audio;
    QJsonArray headsets;
    for (const auto& [hidPath, reading] : service_->snapshot().headsets) {
        QJsonObject item;
        item["sampledMs"] = reading.sampledMs;
        item["statusValid"] = reading.value.status.has_value();
        item["error"] = qs(reading.value.error);
        if (const auto& status = reading.value.status) {
            item["batteryPercent"] = status->batteryPercent;
            item["batteryKind"] = static_cast<int>(status->batteryKind);
            item["eqMode"] = status->eqMode;
        }
        QJsonArray points;
        for (const auto& point : reading.value.peq) {
            if (!point) { points.append(QJsonValue::Null); continue; }
            points.append(QJsonObject{{"frequencyHz", point->frequencyHz}, {"gainTenthsDb", point->gainTenthsDb},
                {"qTenths", point->qTenths}, {"filterType", point->filterType}, {"enabled", point->enabled}});
        }
        item["peq"] = points;
        headsets.append(item);
    }
    root["headsetControls"] = headsets;
    QJsonArray errors;
    for (const auto& e : service_->snapshot().errors)
        errors.append(e);
    root["errors"] = errors;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}
} // namespace peripheral::app
