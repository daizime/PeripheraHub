#pragma once

#include <QString>
#include <QColor>
#include <QIcon>
#include <QDialog>
#include <Qt>
#include <array>

class QComboBox;
class QSlider;
class QSpinBox;
class QTreeWidget;
class QWidget;
class QVBoxLayout;
class QShowEvent;
class QImage;

namespace peripheral::app {

enum class UiIcon {
    Apps, SaveMultiple, Cursor, Desktop, Keyboard, Headphones, BrightnessHigh,
    Speaker, SpeakerMute, Mic, MicOff, Refresh, Details, Link, More, ChevronUp,
    ChevronDown, Exit, Info, Settings
};

// 官方 Fluent 24px 资源；On 使用 filled，Disabled 使用明确的禁用颜色。
// PNG 已在开发阶段按 4x 渲染，运行时不需要 Qt SVG 模块或网络。
QIcon fluentIcon(UiIcon icon, const QColor& color = QColor("#48618a"),
                 const QColor& checkedColor = {});
// 品牌图标保留原始颜色；多个 PNG 尺寸不依赖额外的 ICO 解码插件。
QIcon applicationIcon();

// 拼接到窗口样式末尾，自动覆盖其后创建的子对话框和 QComboBox 弹出列表。
QString comboBoxStyleSheet();
QString glassStyleSheet();
// QToolTip 自动识别 HTML；设备名称和诊断字符串始终作为原文显示。
QString plainTextToolTip(const QString& text);
QWidget* createGlassSurface(QWidget* parent = nullptr);
QWidget* createWindowChrome(QWidget* window, bool dialog = false);
bool applySystemGlass(QWidget* window);
void initializeGlassFrame(QWidget* window);
bool handleGlassFrame(QWidget* window, const QByteArray& eventType, void* message, qintptr* result);

class FrostedDialog final : public QDialog {
  public:
    explicit FrostedDialog(QWidget* parent = nullptr);
    QVBoxLayout* contentLayout() const { return contentLayout_; }
  protected:
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
  private:
    QVBoxLayout* contentLayout_ = nullptr;
    QWidget* surface_ = nullptr;
    bool frameReady_ = false;
};

bool confirmThemedAction(QWidget* parent, const QString& title, const QString& message);
void showThemedMessage(QWidget* parent, const QString& title, const QString& message);

struct CompactDeviceStatus {
    UiIcon icon;
    QString label;
    QString value;
    QString detail;
};
QImage renderCompactPreview(const QSize& maximum, const std::array<CompactDeviceStatus, 4>& devices);
bool enableCompactTaskbarPreview(QWidget* window);
void invalidateCompactTaskbarPreview(QWidget* window);
bool presentCompactTaskbarBitmap(QWidget* window, const QImage& image, bool livePreview = false);

// 闭合框按布局收缩并省略长名称；完整文字保留在模型、提示和独立弹层中。
QComboBox* createThemedComboBox(QWidget* parent = nullptr);
QTreeWidget* createThemedTreeWidget(QWidget* parent = nullptr);

// 数值只通过明确输入、键盘或拖动修改；滚轮继续滚动外层内容区域。
QSpinBox* createThemedSpinBox(QWidget* parent = nullptr);
QSlider* createThemedSlider(Qt::Orientation orientation, QWidget* parent = nullptr);

} // namespace peripheral::app
