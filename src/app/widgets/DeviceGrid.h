#pragma once

#include <QPointer>
#include <QWidget>
#include <vector>

class QGridLayout;
class QLabel;
class QVBoxLayout;

namespace peripheral::app {

class DeviceCard;

// 只负责卡片的显示与排列，设备是否接入由调用方决定。
class DeviceGrid final : public QWidget {
  public:
    explicit DeviceGrid(QWidget* parent = nullptr);
    ~DeviceGrid() override;

    void addCard(DeviceCard* card);
    void setPresent(DeviceCard* card, bool present);
    void setScanning(bool scanning);
    int visibleCardCount() const;

  private:
    struct Entry {
        QPointer<DeviceCard> card;
        bool present = false;
    };

    void reflow();
    void updateEmptyText();

    std::vector<Entry> entries_;
    QVBoxLayout* layout_ = nullptr;
    QGridLayout* grid_ = nullptr;
    QLabel* empty_ = nullptr;
    bool scanning_ = false;
};

} // namespace peripheral::app
