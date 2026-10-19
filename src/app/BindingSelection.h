#pragma once

#include "core/device_model.h"

#include <QString>
#include <QStringList>
#include <vector>

namespace peripheral::app {

struct MouseBindingSelection {
    QStringList endpointPaths;
    QString error;
    explicit operator bool() const { return error.isEmpty() && !endpointPaths.isEmpty(); }
};

// Re-check both channels against the latest complete enumeration. The dialog's
// initial item list is not evidence that a receiver or cable is still present
// when the user confirms, so the same check runs in the dialog and again before
// saving the binding.
MouseBindingSelection resolveMouseBinding(const std::vector<DeviceSnapshot>& devices,
                                          const QString& receiverId, const QString& wiredId);

} // namespace peripheral::app
