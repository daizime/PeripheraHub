#include "BindingSelection.h"

namespace peripheral::app {
namespace {

const DeviceSnapshot* findDevice(const std::vector<DeviceSnapshot>& devices, const QString& id) {
    if (id.isEmpty())
        return nullptr;
    for (const auto& device : devices)
        if (QString::fromStdString(device.logicalId) == id)
            return &device;
    return nullptr;
}

QStringList endpointPaths(const DeviceSnapshot& device) {
    QStringList result;
    for (const auto& endpoint : device.endpoints)
        result << QString::fromStdString(endpoint.endpointId);
    return result;
}

} // namespace

MouseBindingSelection resolveMouseBinding(const std::vector<DeviceSnapshot>& devices,
                                          const QString& receiverId, const QString& wiredId) {
    if (receiverId.isEmpty() || wiredId.isEmpty())
        return {{}, QStringLiteral("请选择接收器与有线接口。")};
    if (receiverId == wiredId)
        return {{}, QStringLiteral("接收器与有线接口不能是同一项。")};
    const auto* receiver = findDevice(devices, receiverId);
    const auto* wired = findDevice(devices, wiredId);
    if (!receiver || !wired)
        return {{}, QStringLiteral("所选接口已在确认前移除，请重新选择。")};
    if (!receiver->receiverPresent)
        return {{}, QStringLiteral("所选接收器当前未接入，请刷新后重试。")};
    if (!wired->wiredPresent)
        return {{}, QStringLiteral("所选有线接口当前未接入，请刷新后重试。")};

    QStringList paths = endpointPaths(*receiver);
    paths << endpointPaths(*wired);
    paths.removeDuplicates();
    if (paths.isEmpty())
        return {{}, QStringLiteral("所选连接没有可用接口路径，请刷新后重试。")};
    return {paths, QString{}};
}

} // namespace peripheral::app
