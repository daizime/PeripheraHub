#pragma once
#include "app/AppSettings.h"
#include <QWidget>

class QComboBox;
class QLabel;

namespace peripheral::app {

// 只管理程序偏好的呈现与用户意图，不访问设备或配置文件。
class SettingsPage final : public QWidget {
    Q_OBJECT
  public:
    SettingsPage(const QString& dataDirectory, const AppPreferences& preferences, QWidget* parent = nullptr);
    void setPreferences(const AppPreferences& preferences);
    void setSaveStatus(const QString& text, bool success);

  signals:
    void preferencesRequested(const peripheral::app::AppPreferences& preferences);
    void openDataDirectoryRequested();
    void openLogRequested();

  private:
    AppPreferences preferences() const;
    QComboBox *closeBehavior_, *restoreBehavior_, *refreshInterval_, *lowBatteryNotification_,
        *experimentalWrites_;
    QLabel* saveStatus_;
};

} // namespace peripheral::app
