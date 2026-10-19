#include "core/device_registry.h"
#include "core/hid_discovery.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int checks = 0;
void check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        std::cerr << "Failed at line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using namespace peripheral;
using peripheral::platform::HidDeviceInfo;

HidDeviceInfo mouse(const std::wstring& path, const std::wstring& container = {}) {
    HidDeviceInfo device;
    device.path = path;
    device.containerId = container;
    device.vid = 0x1234;
    device.pid = 0x5678;
    device.usagePage = 1;
    device.usage = 2;
    return device;
}

std::vector<DeviceSnapshot> snapshots(const std::vector<Endpoint>& endpoints) {
    DeviceRegistry registry;
    registry.setEndpoints(endpoints);
    return registry.snapshots();
}

const Endpoint& find(const std::vector<Endpoint>& endpoints, const std::string& path) {
    const auto item = std::find_if(endpoints.begin(), endpoints.end(),
                                   [&](const auto& endpoint) { return endpoint.endpointId == path; });
    CHECK(item != endpoints.end());
    return *item;
}
} // namespace

int main() {
    // 从未见过的普通键鼠，也只需要输入集合的 Usage，不要求品牌白名单。
    auto unknownMouse = mouse(L"unknown-mouse");
    unknownMouse.product = L"  M900  ";
    unknownMouse.manufacturer = L"Example";
    auto keyboard = mouse(L"unknown-keyboard");
    keyboard.usage = 6;
    keyboard.product = L"example K800";
    keyboard.manufacturer = L"Example";
    auto endpoints = discoverHidEndpoints({unknownMouse, keyboard});
    CHECK(endpoints.size() == 2);
    CHECK(find(endpoints, "unknown-mouse").displayName == "Example M900");
    CHECK(find(endpoints, "unknown-keyboard").displayName == "example K800");
    CHECK(find(endpoints, "unknown-mouse").kind == DeviceKind::Mouse);
    CHECK(find(endpoints, "unknown-keyboard").kind == DeviceKind::Keyboard);
    CHECK(find(endpoints, "unknown-mouse").transport == TransportKind::Unknown);
    CHECK(snapshots(endpoints).size() == 2);

    auto emptyMouse = mouse(L"empty-mouse");
    auto genericKeyboard = keyboard;
    genericKeyboard.path = L"generic-keyboard";
    genericKeyboard.product = L"USB Receiver";
    endpoints = discoverHidEndpoints({emptyMouse, genericKeyboard});
    CHECK(find(endpoints, "empty-mouse").displayName == "USB 鼠标（型号未提供）");
    CHECK(find(endpoints, "generic-keyboard").displayName == "USB 键盘（型号未提供）");
    genericKeyboard.product = L"USB 键盘";
    CHECK(discoverHidEndpoints({genericKeyboard}).front().displayName == "USB 键盘（型号未提供）");
    genericKeyboard.product = L"朗途 L98";
    genericKeyboard.manufacturer.clear();
    CHECK(discoverHidEndpoints({genericKeyboard}).front().displayName == "朗途 L98");

    // 集合的路径、ContainerId、串号和接口号不丢失，同容器同类型由 Registry 合并。
    auto collectionA = mouse(L"a", L"{ABC-123}");
    collectionA.product = L"Model 123";
    collectionA.serial = L"serial-A";
    collectionA.interfaceNumber = 4;
    auto collectionB = collectionA;
    collectionB.path = L"b";
    collectionB.product.clear();
    collectionB.containerId = L"abc-123";
    collectionB.interfaceNumber = 5;
    endpoints = discoverHidEndpoints({collectionA, collectionB, collectionA});
    CHECK(endpoints.size() == 2);
    CHECK(endpoints[0].groupKey == endpoints[1].groupKey);
    CHECK(endpoints[0].containerId == "{ABC-123}");
    CHECK(endpoints[0].serial == "serial-A");
    CHECK(endpoints[0].vid == 0x1234);
    CHECK(endpoints[0].pid == 0x5678);
    CHECK(endpoints[0].usagePage == 1);
    CHECK(endpoints[0].usage == 2);
    CHECK(endpoints[0].interfaceNumber == 4);
    CHECK(endpoints[1].interfaceNumber == 5);
    CHECK(endpoints[1].displayName == "Model 123");
    auto devices = snapshots(endpoints);
    CHECK(devices.size() == 1);
    CHECK(devices.front().endpoints.size() == 2);
    CHECK(devices.front().name == "Model 123");
    CHECK(!devices.front().battery.reportedPercent.has_value());
    CHECK(devices.front().linkState == LinkState::Unknown);
    CHECK(!devices.front().receiverPresent);
    CHECK(!devices.front().wiredPresent);

    // 复合接收器可同时暴露键盘和鼠标，不能因 ContainerId 相同就混成一台鼠标。
    auto sameContainerKeyboard = collectionA;
    sameContainerKeyboard.path = L"keyboard-collection";
    sameContainerKeyboard.usage = 6;
    endpoints = discoverHidEndpoints({collectionA, sameContainerKeyboard});
    CHECK(endpoints.size() == 2);
    CHECK(endpoints[0].groupKey != endpoints[1].groupKey);
    CHECK(snapshots(endpoints).size() == 2);

    auto separate = collectionA;
    separate.path = L"other-unit";
    separate.containerId = L"{different-container}";
    separate.serial.clear();
    collectionA.serial.clear();
    CHECK(snapshots(discoverHidEndpoints({collectionA, separate})).size() == 2);
    collectionA.containerId.clear();
    separate.containerId.clear();
    endpoints = discoverHidEndpoints({collectionA, separate});
    CHECK(endpoints[0].groupKey.empty());
    CHECK(endpoints[1].groupKey.empty());
    CHECK(snapshots(endpoints).size() == 2); // 相同 VID/PID、名称不能合并实体。

    // 旧有 LANGTU VID/PID 只是候选证据，不能覆盖设备自己提供的型号。
    auto langtu = keyboard;
    langtu.vid = 0x1A2C;
    langtu.pid = 0x7FFF;
    langtu.product = L"Another keyboard";
    langtu.manufacturer.clear();
    endpoints = discoverHidEndpoints({langtu});
    CHECK(endpoints.front().displayName == "Another keyboard");
    langtu.product.clear();
    CHECK(discoverHidEndpoints({langtu}).front().displayName == "USB 键盘（型号未提供）");

    // 没有输入用途时，不根据 VID/PID、名称或相邻设备推断其是键鼠。
    auto vendor = mouse(L"vendor");
    vendor.usagePage = 0xFF01;
    auto consumer = mouse(L"consumer");
    consumer.usagePage = 0x0C;
    consumer.usage = 1;
    auto gamepad = mouse(L"gamepad");
    gamepad.usage = 5;
    auto untyped = mouse(L"untyped");
    untyped.usagePage = 0;
    untyped.usage = 0;
    auto noPath = mouse(L"");
    CHECK(discoverHidEndpoints({vendor, consumer, gamepad, untyped, noPath}).empty());
    auto ordinaryInput = collectionA;
    ordinaryInput.containerId = L"{ordinary-container}";
    vendor.containerId = ordinaryInput.containerId;
    CHECK(discoverHidEndpoints({ordinaryInput, vendor}).size() == 1);

    // VGN 电量观察的 Endpoint 必须保留，但只有后端已验证的专用集合可例外分类。
    auto vgn = vendor;
    vgn.path = L"vgn-receiver";
    vgn.containerId = L"{vgn-container}";
    vgn.vid = 0x3554;
    vgn.pid = 0xFB56;
    vgn.usagePage = 0xFF02;
    vgn.usage = 2;
    vgn.inputReportBytes = 17;
    vgn.outputReportBytes = 17;
    CHECK(discoverHidEndpoints({vgn}).empty());
    vgn.vgnBatteryCandidate = true;
    auto vgnInput = vgn;
    vgnInput.path = L"vgn-input";
    vgnInput.usagePage = 1;
    endpoints = discoverHidEndpoints({vgn, vgnInput});
    CHECK(endpoints.size() == 2);
    CHECK(find(endpoints, "vgn-receiver").usagePage == 0xFF02);
    CHECK(find(endpoints, "vgn-receiver").displayName == "VGN Dragonfly F2 Master+");
    CHECK(find(endpoints, "vgn-receiver").transport == TransportKind::Receiver24G);
    CHECK(snapshots(endpoints).size() == 1);
    auto vgnMacro = vgnInput;
    vgnMacro.path = L"vgn-macro-keyboard";
    vgnMacro.usage = 6;
    CHECK(discoverHidEndpoints({vgn, vgnInput, vgnMacro}).size() == 2);
    CHECK(discoverHidEndpoints({vgnMacro}).empty());
    auto invalidVgn = vgn;
    invalidVgn.outputReportBytes = 16;
    CHECK(discoverHidEndpoints({invalidVgn}).empty());
    invalidVgn = vgn;
    invalidVgn.vid = 1;
    CHECK(discoverHidEndpoints({invalidVgn}).empty());
    invalidVgn = vgn;
    invalidVgn.usagePage = 0xFF01;
    CHECK(discoverHidEndpoints({invalidVgn}).empty());

    auto wired = vgn;
    wired.path = L"vgn-wired";
    wired.pid = 0xFB57;
    wired.containerId = L"{vgn-wired-container}";
    endpoints = discoverHidEndpoints({vgn, vgnInput, wired});
    CHECK(find(endpoints, "vgn-wired").transport == TransportKind::WiredUsb);
    DeviceRegistry registry;
    registry.setEndpoints(endpoints);
    CHECK(registry.snapshots().size() == 2);
    CHECK(registry.bindEndpoints({"vgn-input", "vgn-receiver", "vgn-wired"}, "user-mouse"));
    DeviceObservation observation;
    observation.linkState = LinkState::Online;
    observation.reportedPercent = 42;
    CHECK(registry.observe("vgn-receiver", observation));
    devices = registry.snapshots();
    CHECK(devices.size() == 1);
    CHECK(devices.front().endpoints.size() == 3);
    CHECK(devices.front().battery.reportedPercent == 42);
    CHECK(devices.front().receiverPresent);
    CHECK(devices.front().wiredPresent);
    registry.setEndpoints({});
    CHECK(registry.snapshots().front().endpoints.empty()); // UI 应隐藏历史但不删除缓存。

    std::cout << "HID discovery: " << checks << " checks passed\n";
}
