#pragma once
#include "platform/WindowsBackend.h"
#include <QWidget>

class QLabel;
namespace peripheral::app {
// Read-only view of model-specific data; no device discovery or HID access.
class HeadsetDetails final : public QWidget {
  public:
    explicit HeadsetDetails(QWidget* parent = nullptr);
    void setInfo(const platform::N9ControlsInfo* info);
  private:
    QLabel *state_, *peq_;
};
} // namespace peripheral::app
