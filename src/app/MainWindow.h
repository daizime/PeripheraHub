#pragma once
#include "DeviceService.h"
#include "DeviceProfiles.h"
#include "AppSettings.h"
#include "core/device_registry.h"
#include <QMainWindow>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QSet>
#include <QHash>
#include <map>
class QLabel;
class QComboBox;
class QSlider;
class QPushButton;
class QProgressBar;
class QCloseEvent;
class QShowEvent;
class QResizeEvent;
class QTreeWidget;
class QTreeWidgetItem;
class QPlainTextEdit;
class QStackedWidget;
class QImage;
class QScreen;
class QFrame;
class QHBoxLayout;
class QToolButton;
class QMenu;
class QAction;
namespace peripheral::app {
class PercentDetails;
class MonitorControls;
class DeviceCard;
class DeviceGrid;
class ProfileTracker;
class SettingsPage;
class HeadsetDetails;
class MouseDetails;
class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    MainWindow(DeviceService* service, const QString& dataDirectory, QWidget* parent = nullptr);
    bool saveState(const QString& path) const;
    QImage taskbarPreviewImage(const QSize& maximum) const;
    // 系统唤醒后的重新枚举入口。nativeEvent 只负责识别消息，行为放在这里，
    // 便于在无真实窗口句柄的测试环境中验证。
    void handleSystemResume();

  signals:
    // 供托盘通知与测试观察；界面同时保留通知区提示。
    void lowBatteryAlert(const QString& message);

  protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

  private:
    void buildUi();
    // 侧栏收放：收起时保留图标轨道，展开时恢复文字和连接状态。
    void setSidebarCollapsed(bool collapsed);
    // 设计最小尺寸随当前屏幕工作区和 DPI 重新计算；高 DPI/小屏上不能沿用启动时的逻辑尺寸。
    void updateWindowConstraints();
    void watchCurrentScreen();
    void updateDevices();
    void updateMouse();
    void updateKeyboard();
    void updateAudio();
    void updateAudioEndpoint(bool capture);
    void updateMonitor();
    void updateMonitorControls();
    void updateProfiles();
    void updateSavedProfilesView();
    void showSavedProfile();
    void updateProfileDeleteControls();
    void requestProfileDeletion();
    void finishProfileDeletion(quint64 ticket);
    void rememberSetting(const QString& id, const QString& key, int value);
    void requestManualRefresh();
    void showDetails();
    void bindMouseConnections();
    void showOperation(const QString& message, bool success);
    void setupTray();
    void buildTrayMenu();
    void refreshTrayMenu();
    // 托盘滑条与主窗口共用 setFeature/实时合并队列；松手时提交最终值。
    void applyTrayLive(bool brightness, bool capture, int percent);
    void finishTrayLive(bool brightness, bool capture);
    void toggleTrayMute(bool capture);
    void updateLowBatteryNotifications();
    void applyPreferences();
    void changePreferences(const AppPreferences& preferences);
    void openSettingsLocation(bool log);
    QString detailsText() const;
    DeviceService* service_;
    QSettings settings_;
    AppSettings appSettings_;
    AppPreferences preferences_;
    DeviceProfiles profiles_;
    DeviceRegistry registry_;
    std::vector<DeviceSnapshot> devices_;
    std::map<std::wstring, qint64> ingested_;
    QStringList bindings_;
    bool quitting_ = false;
    bool chromeFrameReady_ = false;
    QLabel *updated_, *notice_, *mouseValue_, *mouseState_, *mouseConnection_, *mouseFreshness_,
        *keyboardState_, *keyboardDetail_, *audioValue_, *audioInputValue_, *monitorValue_, *mouseMore_,
        *keyboardMore_;
    QProgressBar* mouseBattery_;
    QComboBox *mouseSelector_, *keyboardSelector_, *audioSelector_, *audioInputSelector_, *monitorSelector_;
    QSlider *audioSlider_, *audioInputSlider_, *monitorSlider_;
    QPushButton *muteButton_, *detailMuteButton_, *inputMuteButton_, *inputDetailMuteButton_,
        *bindMouseButton_, *refreshButton_;
    PercentDetails *audioDetails_, *audioInputDetails_, *monitorDetails_;
    DeviceCard *mouseCard_, *keyboardCard_, *audioCard_, *monitorCard_;
    DeviceGrid* deviceGrid_;
    MonitorControls* monitorControls_;
    QTimer displayChangeTimer_;
    QTimer resumeTimer_;
    // 当前鼠标的 VGN 写入口与活动档位；为空或 -1 时界面不显示写入区。
    QString vgnWriteTarget_;
    int vgnActiveStage_ = -1;
    // 唤醒后强制扫描的起点；用于记录"恢复后第一次有效读数"的耗时。
    qint64 resumeRefreshStartedMs_ = 0;
    QTimer taskbarPreviewTimer_;
    ProfileTracker* profileTracker_ = nullptr;
    QStackedWidget* pages_;
    SettingsPage* settingsPage_;
    HeadsetDetails* headsetDetails_;
    QLabel* headsetBatteryStatus_;
    MouseDetails* mouseDetails_;
    QLabel* mouseConfiguration_;
    QPushButton* settingsNavigation_;
    QLabel* savedProfilesHint_;
    QTreeWidget* savedTree_;
    QMap<QString, QTreeWidgetItem*> savedGroups_;
    QMap<QString, QTreeWidgetItem*> savedItems_;
    QPlainTextEdit* savedDetail_;
    QPushButton* deleteProfileButton_;
    QLabel *deleteProfileHint_, *deleteProfileResult_;
    QString savedDetailProfileId_;
    QString deletingProfileId_, deletingProfileName_;
    quint64 deleteProfileTicket_ = 0;
    quint64 manualRefreshTicket_ = 0;
    qint64 manualRefreshMessageUntil_ = 0;
    qint64 lowBatteryNoticeUntil_ = 0;
    bool mousePresent_ = false, keyboardPresent_ = false, audioPresent_ = false, monitorPresent_ = false;
    QSystemTrayIcon* tray_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QLabel* trayBatteryStatus_ = nullptr;
    QSlider *trayBrightnessSlider_ = nullptr, *trayOutputSlider_ = nullptr, *trayInputSlider_ = nullptr;
    QLabel *trayBrightnessValue_ = nullptr, *trayOutputValue_ = nullptr, *trayInputValue_ = nullptr;
    QToolButton *trayOutputMuteButton_ = nullptr, *trayInputMuteButton_ = nullptr;
    // 读回前先记录用户最后一次静音意图，快速连续点击也能交替开关。
    QHash<QString, QPair<bool, qint64>> trayMuteOverrides_;
    QSet<QString> lowBatteryNotified_;
    QScreen* watchedScreen_ = nullptr;
    bool screenSignalReady_ = false;
    QFrame* sidebar_ = nullptr;
    QToolButton* sidebarToggle_ = nullptr;
    QLabel* brandName_ = nullptr;
    QLabel* brandTagline_ = nullptr;
    QLabel* brandIcon_ = nullptr;
    QFrame* sidebarSeparator_ = nullptr;
    QWidget* connectionPanel_ = nullptr;
    QHBoxLayout* brandLayout_ = nullptr;
    QHBoxLayout* toggleRow_ = nullptr;
    bool sidebarCollapsed_ = false;
};
} // namespace peripheral::app
