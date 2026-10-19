#pragma once

#include <QFrame>
#include <QPoint>

class QLabel;
class QIcon;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace peripheral::app {

// 每张卡片独立切换内容；详情只在内部滚动，不改变外层网格的空间分配。
class DeviceCard final : public QFrame {
    Q_OBJECT

  public:
    explicit DeviceCard(const QString& category, const QString& title, QWidget* parent = nullptr);

    QVBoxLayout* quickLayout() const { return quickLayout_; }
    QVBoxLayout* detailLayout() const { return detailLayout_; }
    bool isExpanded() const { return expanded_; }
    QString cardId() const { return cardId_; }
    void setCardId(const QString& id) { cardId_ = id; }
    void setTitle(const QString& title);
    void setCategoryIcon(const QIcon& icon);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override;

  public slots:
    void setExpanded(bool expanded);

  signals:
    void expandedChanged(bool expanded);
    void expansionChanged(const QString& cardId, bool expanded);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    bool isBlankClickTarget(const QWidget* widget) const;
    void updateTitleLabel();

    QString cardId_;
    QString title_;
    QLabel* titleLabel_ = nullptr;
    QLabel* categoryIcon_ = nullptr;
    QVBoxLayout* quickLayout_ = nullptr;
    QVBoxLayout* detailLayout_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QPushButton* toggleButton_ = nullptr;
    bool expanded_ = false;
    bool clickArmed_ = false;
    QPoint pressPosition_;
};

} // namespace peripheral::app
