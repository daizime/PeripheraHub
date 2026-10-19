#include "SettingsPage.h"
#include "UiTheme.h"
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace peripheral::app {
namespace {
QLabel* text(const QString& value, const char* role = "body") {
    auto* label = new QLabel(value);
    label->setProperty("role", role);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
QVBoxLayout* section(QVBoxLayout* parent, const QString& title) {
    auto* frame = new QFrame;
    frame->setObjectName("settingsSection");
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(24, 20, 24, 22);
    layout->setSpacing(18);
    layout->addWidget(text(title, "sectionTitle"));
    parent->addWidget(frame);
    return layout;
}
void addOption(QVBoxLayout* parent, const QString& title, const QString& description, QComboBox* control) {
    auto* row = new QHBoxLayout;
    row->setSpacing(24);
    auto* labels = new QVBoxLayout;
    labels->setSpacing(6);
    labels->addWidget(text(title, "controlLabel"));
    labels->addWidget(text(description, "muted"));
    row->addLayout(labels, 1);
    control->setMinimumWidth(230);
    control->setMaximumWidth(280);
    row->addWidget(control, 0, Qt::AlignVCenter);
    parent->addLayout(row);
}
} // namespace

SettingsPage::SettingsPage(const QString& dataDirectory, const AppPreferences& preferences, QWidget* parent)
    : QWidget(parent) {
    setObjectName("settingsPage");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(38, 18, 30, 10);
    layout->setSpacing(18);
    layout->addWidget(text("设置", "title"));
    layout->addWidget(text("管理程序行为与设备恢复，修改后立即保存。", "muted"));
    auto* scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget;
    auto* sections = new QVBoxLayout(content);
    sections->setContentsMargins(0, 0, 8, 0);
    sections->setSpacing(16);

    auto* general = section(sections, "常规");
    closeBehavior_ = createThemedComboBox();
    closeBehavior_->setObjectName("settingsCloseBehavior");
    closeBehavior_->setAccessibleName("关闭窗口时");
    closeBehavior_->addItem("收起到系统托盘", true);
    closeBehavior_->addItem("退出程序", false);
    addOption(general, "关闭窗口时", "无可用系统托盘时，关闭窗口会直接退出。", closeBehavior_);

    auto* device = section(sections, "设备管理");
    restoreBehavior_ = createThemedComboBox();
    restoreBehavior_->setObjectName("settingsRestoreBehavior");
    restoreBehavior_->setAccessibleName("重连自动恢复");
    restoreBehavior_->addItem("自动恢复已确认参数", true);
    restoreBehavior_->addItem("只读取设备当前参数", false);
    addOption(device, "重连自动恢复", "影响后续接入的亮度、音量与可用情景；输入源保持当前选择。",
              restoreBehavior_);
    refreshInterval_ = createThemedComboBox();
    refreshInterval_->setObjectName("settingsRefreshInterval");
    refreshInterval_->setAccessibleName("后台刷新间隔");
    for (const auto seconds : {3, 5, 10, 30})
        refreshInterval_->addItem(QStringLiteral("每 %1 秒").arg(seconds), seconds);
    addOption(device, "后台刷新间隔", "调整自动检测频率；连接通知、手动刷新与滑块调节继续独立响应。",
              refreshInterval_);
    lowBatteryNotification_ = createThemedComboBox();
    lowBatteryNotification_->setObjectName("settingsLowBatteryNotification");
    lowBatteryNotification_->setAccessibleName("低电量提醒");
    lowBatteryNotification_->addItem("开启", true);
    lowBatteryNotification_->addItem("关闭", false);
    addOption(device, "低电量提醒", "鼠标或耳机电量低于 10% 时通过系统通知提示；关闭后不再提示。",
              lowBatteryNotification_);
    experimentalWrites_ = createThemedComboBox();
    experimentalWrites_->setObjectName("settingsExperimentalWrites");
    experimentalWrites_->setAccessibleName("实验性设备写入");
    experimentalWrites_->addItem("关闭", false);
    experimentalWrites_->addItem("开启（仅已验证型号）", true);
    addOption(device, "实验性设备写入",
              "开启后，鼠标详情出现写入控件：VGN F2 Master+ 的档位 DPI、回报率与当前档位。"
              "每次写入立即回读，失败不回滚；未验证的型号不会出现任何写入控件。",
              experimentalWrites_);

    auto* storageSection = section(sections, "本地数据与版本");
    storageSection->addWidget(text("数据目录", "controlLabel"));
    auto* path = text(QDir::toNativeSeparators(dataDirectory), "muted");
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setAccessibleName("程序数据目录");
    storageSection->addWidget(path);
    auto* actions = new QHBoxLayout;
    auto* directory = new QPushButton("打开数据目录");
    directory->setObjectName("settingsOpenDataDirectory");
    auto* log = new QPushButton("打开日志");
    log->setObjectName("settingsOpenLog");
    actions->addWidget(directory);
    actions->addWidget(log);
    actions->addStretch();
    storageSection->addLayout(actions);
    storageSection->addWidget(text("初始预览版 · " + QCoreApplication::applicationVersion(), "muted"));
    sections->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);

    auto* footer = new QHBoxLayout;
    saveStatus_ = text("修改后立即保存并生效。", "muted");
    saveStatus_->setObjectName("settingsSaveStatus");
    saveStatus_->setAccessibleName("程序设置保存状态");
    footer->addWidget(saveStatus_, 1);
    auto* defaults = new QPushButton("恢复默认程序设置");
    defaults->setObjectName("settingsRestoreDefaults");
    defaults->setToolTip("仅重置本页程序选项，设备档案继续保留。");
    footer->addWidget(defaults);
    layout->addLayout(footer);

    setPreferences(preferences);
    for (auto* control : {closeBehavior_, restoreBehavior_, refreshInterval_, lowBatteryNotification_,
                          experimentalWrites_})
        connect(control, &QComboBox::currentIndexChanged, this,
                [this] { emit preferencesRequested(this->preferences()); });
    connect(defaults, &QPushButton::clicked, this, [this] { emit preferencesRequested(AppPreferences{}); });
    connect(directory, &QPushButton::clicked, this, &SettingsPage::openDataDirectoryRequested);
    connect(log, &QPushButton::clicked, this, &SettingsPage::openLogRequested);
}

AppPreferences SettingsPage::preferences() const {
    return {closeBehavior_->currentData().toBool(), restoreBehavior_->currentData().toBool(),
            refreshInterval_->currentData().toInt(), lowBatteryNotification_->currentData().toBool(),
            experimentalWrites_->currentData().toBool()};
}

void SettingsPage::setPreferences(const AppPreferences& preferences) {
    const QSignalBlocker close(closeBehavior_), restore(restoreBehavior_), refresh(refreshInterval_),
        battery(lowBatteryNotification_), writes(experimentalWrites_);
    closeBehavior_->setCurrentIndex(closeBehavior_->findData(preferences.closeToTray));
    restoreBehavior_->setCurrentIndex(restoreBehavior_->findData(preferences.restoreOnReconnect));
    refreshInterval_->setCurrentIndex(refreshInterval_->findData(preferences.refreshIntervalSeconds));
    lowBatteryNotification_->setCurrentIndex(
        lowBatteryNotification_->findData(preferences.lowBatteryNotification));
    experimentalWrites_->setCurrentIndex(experimentalWrites_->findData(preferences.experimentalDeviceWrites));
}

void SettingsPage::setSaveStatus(const QString& value, bool success) {
    saveStatus_->setText(value);
    saveStatus_->setProperty("saveSucceeded", success);
}

} // namespace peripheral::app
