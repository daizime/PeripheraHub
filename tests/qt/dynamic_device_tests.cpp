#include "app/DeviceProfiles.h"
#include "app/MainWindow.h"
#include "app/BindingSelection.h"
#include "app/widgets/SettingsPage.h"
#include "app/widgets/DeviceCard.h"
#include "app/widgets/DeviceGrid.h"
#include "app/widgets/MonitorControls.h"
#include "app/widgets/PercentDetails.h"
#include "app/widgets/UiTheme.h"
#include "core/hid_discovery.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QAction>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QStackedWidget>
#include <QScreen>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyleOptionComboBox>
#include <QStyleOptionSlider>
#include <QTemporaryDir>
#include <QToolTip>
#include <QToolButton>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fake {
using namespace peripheral::platform;
std::mutex mutex;
std::condition_variable condition;
std::atomic<bool> holdNextHid{false}, hidEntered{false}, releaseHid{false};
struct Fixture {
    std::vector<HidDeviceInfo> hid;
    std::vector<AudioEndpointInfo> audio;
    std::vector<MonitorInfo> monitors;
    int n9BatteryPercent = -1;
    bool failHid = false, failAudio = false, failMonitor = false;
} state;
struct Write {
    std::wstring target;
    std::string kind;
    unsigned code = 0;
    int value = 0;
};
std::vector<Write> writes;
std::atomic<int> batteryQueries{0}, advancedQueries{0}, enumerations{0}, privateWrites{0};

void record(const std::wstring& target, const char* kind, unsigned code, int value) {
    std::lock_guard lock(mutex);
    writes.push_back({target, kind, code, value});
}
int writeCount() {
    std::lock_guard lock(mutex);
    return static_cast<int>(writes.size());
}
} // namespace fake

// The complete backend is fake: no USB/HID, Core Audio or DDC/CI implementation is linked.
namespace peripheral::platform {
std::vector<HidDeviceInfo> WindowsBackend::enumerateHidDevices() const {
    ++fake::enumerations;
    std::unique_lock lock(fake::mutex);
    if (fake::holdNextHid.exchange(false)) {
        fake::hidEntered = true;
        fake::condition.wait(lock, [] { return fake::releaseHid.load(); });
    }
    if (fake::state.failHid)
        throw std::runtime_error("fixture HID enumeration failed");
    return fake::state.hid;
}
std::vector<AudioEndpointInfo> WindowsBackend::enumerateAudioEndpoints() const {
    ++fake::enumerations;
    std::lock_guard lock(fake::mutex);
    if (fake::state.failAudio)
        throw std::runtime_error("fixture audio enumeration failed");
    return fake::state.audio;
}
std::vector<MonitorInfo> WindowsBackend::enumerateMonitors() const {
    ++fake::enumerations;
    std::lock_guard lock(fake::mutex);
    if (fake::state.failMonitor)
        throw std::runtime_error("fixture monitor enumeration failed");
    return fake::state.monitors;
}
VgnBatteryInfo WindowsBackend::queryVgnBattery(const HidDeviceInfo&) const {
    ++fake::batteryQueries;
    return {};
}
// 实验性写入在测试里只有一种可能：调用被拒绝，因此替身永远不报告写入成功。
VgnFlashWriteResult WindowsBackend::writeVgnDpiStage(const HidDeviceInfo&, int, int, int) const {
    ++fake::privateWrites;
    return {};
}
VgnScalarWriteResult WindowsBackend::writeVgnPollingRate(const HidDeviceInfo&, int) const {
    ++fake::privateWrites;
    return {};
}
VgnScalarWriteResult WindowsBackend::writeVgnActiveStage(const HidDeviceInfo&, int) const {
    ++fake::privateWrites;
    return {};
}
N9ControlsInfo WindowsBackend::queryN9Controls(const HidDeviceInfo&, bool) const {
    std::lock_guard lock(fake::mutex);
    if (fake::state.n9BatteryPercent < 0)
        return {};
    N9ControlsInfo info;
    info.supported = true;
    n9::Status status;
    status.batteryPercent = fake::state.n9BatteryPercent;
    status.batteryKind = n9::BatteryKind::Discharging;
    info.status = status;
    return info;
}
MonitorControlsInfo WindowsBackend::queryMonitorControls(const std::wstring& id) const {
    ++fake::advancedQueries;
    MonitorControlsInfo info;
    info.id = id;
    info.capabilities = "(vcp(12 60(0F 11) DC(00 03))mccs_ver(2.1))";
    info.version = "2.1";
    MonitorFeatureInfo contrast;
    contrast.code = 0x12;
    contrast.advertised = contrast.readable = contrast.writable = true;
    contrast.current = 50;
    contrast.maximum = 100;
    info.features.push_back(contrast);
    MonitorFeatureInfo source;
    source.code = 0x60;
    source.advertised = source.readable = source.writable = true;
    source.current = 0x0F;
    source.allowedValues = {0x0F, 0x11};
    info.features.push_back(source);
    MonitorFeatureInfo scenario;
    scenario.code = 0xDC;
    scenario.advertised = scenario.readable = scenario.writable = true;
    scenario.current = 0;
    scenario.allowedValues = {0, 3};
    info.features.push_back(scenario);
    return info;
}
MonitorFeatureInfo WindowsBackend::queryMonitorFeature(const std::wstring& id, std::uint8_t code) const {
    // 仅复用模拟数据，不用此夹具证明原生 DDC/CI 的单项调用次数或耗时。
    const auto controls = queryMonitorControls(id);
    const auto found = std::find_if(controls.features.begin(), controls.features.end(),
                                    [code](const auto& feature) { return feature.code == code; });
    if (found == controls.features.end())
        return {};
    auto feature = *found;
    feature.advertised = feature.writable = false;
    return feature;
}
void WindowsBackend::setAudioVolume(const std::wstring& id, float value) const {
    fake::record(id, "volume", 0, static_cast<int>(value));
}
void WindowsBackend::setAudioMute(const std::wstring& id, bool value) const {
    fake::record(id, "mute", 0, value);
}
void WindowsBackend::setMonitorBrightness(const std::wstring& id, int value) const {
    fake::record(id, "brightness", 0, value);
}
void WindowsBackend::setMonitorFeature(const std::wstring& id, std::uint8_t code, std::uint16_t value) const {
    fake::record(id, "feature", code, value);
}
MonitorWriteTimings WindowsBackend::setMonitorFeatureTimed(const std::wstring& id, std::uint8_t code,
                                                           std::uint16_t value) const {
    setMonitorFeature(id, code, value);
    return {}; // 模拟时序，不作为真实设备延迟证据。
}
MonitorWriteTimings WindowsBackend::setMonitorFeatureFromCapabilities(
    const std::wstring& id, std::uint8_t code, std::uint16_t value, const MonitorControlsInfo&) const {
    return setMonitorFeatureTimed(id, code, value);
}
} // namespace peripheral::platform

using namespace peripheral;
using namespace peripheral::app;
int checks = 0, failures = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cout << "FAIL " << message << std::endl;
    }
}
bool waitUntil(const std::function<bool()>& condition, int timeoutMs = 3500) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QApplication::processEvents();
    return condition();
}
void settle() {
    for (int index = 0; index < 5; ++index)
        QApplication::processEvents();
}
platform::HidDeviceInfo hid(const wchar_t* path, const wchar_t* container, const wchar_t* model,
                            unsigned usage) {
    platform::HidDeviceInfo result;
    result.path = path;
    result.containerId = container;
    result.product = model;
    result.manufacturer = L"ACME";
    result.vid = 0x4444;
    result.pid = usage == 2 ? 0x1010 : 0x2020;
    result.usagePage = 1;
    result.usage = static_cast<std::uint16_t>(usage);
    return result;
}
platform::MonitorInfo monitor(const wchar_t* id, const wchar_t* name, int brightness) {
    platform::MonitorInfo result;
    result.id = id;
    result.name = name;
    result.brightnessReadable = true;
    result.currentBrightness = result.brightnessPercent = brightness;
    result.maxBrightness = 100;
    return result;
}
platform::AudioEndpointInfo audio(const wchar_t* id, const wchar_t* name, bool capture = false) {
    platform::AudioEndpointInfo result;
    result.id = id;
    result.name = name;
    result.volumeReadable = true;
    result.volumePercent = 20;
    result.capture = capture;
    return result;
}
platform::HidDeviceInfo n9Headset(const wchar_t* path, const wchar_t* container) {
    auto result = hid(path, container, L"模拟 ATK N9 V2 ULTRA", 1);
    result.vid = 0x373B;
    result.pid = 0x12D6;
    result.usagePage = 0xFF90;
    result.usage = 1;
    result.inputReportBytes = 64;
    result.outputReportBytes = 64;
    result.n9ControlCandidate = true;
    return result;
}
QComboBox* selector(DeviceCard* card) {
    if (card->cardId() == "monitor")
        return card->findChild<QComboBox*>("monitorSelector");
    return card->quickLayout()->parentWidget()->findChild<QComboBox*>();
}
QString title(DeviceCard* card) {
    for (const auto* label : card->findChildren<QLabel*>())
        if (label->property("role") == "cardTitle")
            return label->toolTip();
    return {};
}
QPushButton* button(MainWindow& window, const QString& text) {
    for (auto* candidate : window.findChildren<QPushButton*>())
        if (candidate->text() == text)
            return candidate;
    return nullptr;
}
QJsonArray savedRows(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("profiles").toArray();
}
QJsonObject savedRow(const QString& path, const QString& id) {
    for (const auto entry : savedRows(path))
        if (entry.toObject().value("id").toString() == id)
            return entry.toObject();
    return {};
}
QList<QTreeWidgetItem*> leaves(QTreeWidget* tree) {
    QList<QTreeWidgetItem*> result;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if (!(*it)->data(0, Qt::UserRole).toString().isEmpty())
            result.append(*it);
    return result;
}
QTreeWidgetItem* leaf(QTreeWidget* tree, const QString& id) {
    for (auto* item : leaves(tree))
        if (item->data(0, Qt::UserRole) == id)
            return item;
    return nullptr;
}
QTreeWidgetItem* group(QTreeWidget* tree, const QString& key) {
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, Qt::UserRole + 1) == key)
            return *it;
    return nullptr;
}

void scanFixture(DeviceService& service, const fake::Fixture& fixture) {
    {
        std::lock_guard lock(fake::mutex);
        fake::state = fixture;
    }
    bool refreshed = false;
    const auto connection =
        QObject::connect(&service, &DeviceService::refreshed, &service, [&] { refreshed = true; });
    service.refresh(true);
    check(waitUntil([&] { return refreshed; }), "selector/scroll fixture receives actual service scan");
    QObject::disconnect(connection);
    settle();
}

void mouseBindingChecks() {
    const auto endpoint = [](const char* id, TransportKind transport) {
        Endpoint value;
        value.endpointId = id;
        value.kind = DeviceKind::Mouse;
        value.transport = transport;
        return value;
    };
    DeviceSnapshot receiver;
    receiver.logicalId = "hid:receiver";
    receiver.kind = DeviceKind::Mouse;
    receiver.receiverPresent = true;
    receiver.endpoints = {endpoint("receiver-path", TransportKind::Receiver24G)};
    DeviceSnapshot wired;
    wired.logicalId = "hid:wired";
    wired.kind = DeviceKind::Mouse;
    wired.wiredPresent = true;
    wired.endpoints = {endpoint("wired-path", TransportKind::WiredUsb)};

    const auto both = resolveMouseBinding({receiver, wired}, "hid:receiver", "hid:wired");
    check(both && both.endpointPaths == QStringList{"receiver-path", "wired-path"},
          "receiver and wired channels produce both binding paths");

    const auto removed = resolveMouseBinding({wired}, "hid:receiver", "hid:wired");
    check(!removed && removed.error.contains("移除"),
          "receiver removed while the dialog is open rejects the binding instead of saving one channel");

    const auto wrongChannel = resolveMouseBinding({receiver, wired}, "hid:wired", "hid:receiver");
    check(!wrongChannel && wrongChannel.error.contains("接收器"),
          "each selection must still be the channel it was chosen for");

    const auto same = resolveMouseBinding({receiver, wired}, "hid:receiver", "hid:receiver");
    check(!same && same.error.contains("同一项"), "the same record cannot fill both channels");
}

void selectionChecks(const QString& output) {
    fake::Fixture base;
    base.hid = {hid(L"selection-mouse-A", L"selection-mouse-A", L"模拟 SELECT M1", 2),
                hid(L"selection-mouse-B", L"selection-mouse-B", L"模拟 SELECT M2", 2),
                hid(L"selection-keyboard-A", L"selection-keyboard-A", L"模拟 SELECT K1", 6),
                hid(L"selection-keyboard-B", L"selection-keyboard-B", L"模拟 SELECT K2", 6)};
    auto vgn = hid(L"selection-vgn-A", L"selection-vgn-A", L"模拟 F2 标准鼠标接口", 2);
    vgn.vid = 0x3554;
    vgn.pid = 0xFB56;
    base.hid.push_back(vgn);
    DeviceRegistry identities;
    identities.setEndpoints(discoverHidEndpoints(base.hid));
    std::map<std::wstring, QString> ids;
    for (const auto& device : identities.snapshots(MonotonicClock::now(), std::chrono::seconds(45)))
        for (const auto& endpoint : device.endpoints)
            ids[QString::fromStdString(endpoint.endpointId).toStdWString()] =
                QString::fromStdString(device.logicalId);
    const auto mouseA = ids[L"selection-mouse-A"];
    const auto mouseB = ids[L"selection-mouse-B"];
    const auto keyboardA = ids[L"selection-keyboard-A"];
    const auto keyboardB = ids[L"selection-keyboard-B"];
    const auto verified = ids[L"selection-vgn-A"];
    check(!mouseA.isEmpty() && !mouseB.isEmpty() && !keyboardB.isEmpty() && !verified.isEmpty(),
          "selector fixtures have grounded logical IDs from discovery/registry");
    for (int scenario = 0; scenario < 7; ++scenario) {
        QTemporaryDir directory(output + "/selection-data-XXXXXX");
        check(directory.isValid(), "selection scenario uses fresh independent settings directory");
        fake::Fixture fixture = base;
        QString savedMouse, binding, savedKeyboard;
        QString expectedMouse = verified, expectedKeyboard;
        if (scenario == 1) {
            savedMouse = expectedMouse = mouseA;
            binding = mouseB;
            savedKeyboard = expectedKeyboard = keyboardB;
        } else if (scenario == 2) {
            savedMouse = "removed-mouse";
            binding = expectedMouse = mouseB;
        } else if (scenario == 3) {
            savedMouse = binding = "removed-mouse";
        } else if (scenario == 4) {
            auto another = vgn;
            another.path = another.containerId = L"selection-vgn-B";
            fixture.hid.push_back(another);
            expectedMouse.clear();
        } else if (scenario == 5) {
            fixture.hid.pop_back();
            expectedMouse.clear();
        } else if (scenario == 6) {
            fixture.hid = {base.hid[0], base.hid[2]};
            expectedMouse = mouseA;
            expectedKeyboard = keyboardA;
        }
        {
            QSettings settings(directory.path() + "/settings.ini", QSettings::IniFormat);
            settings.setValue("mouse/selectedId", savedMouse);
            settings.setValue("mouse/bindingId", binding);
            settings.setValue("keyboard/selectedId", savedKeyboard);
            settings.sync();
        }
        DeviceService service(false);
        MainWindow window(&service, directory.path());
        DeviceCard *mouse = nullptr, *keyboard = nullptr;
        for (auto* card : window.findChildren<DeviceCard*>()) {
            if (card->cardId() == "mouse")
                mouse = card;
            if (card->cardId() == "keyboard")
                keyboard = card;
        }
        check(mouse && keyboard, "selection scenario finds mouse and keyboard card");
        if (!mouse || !keyboard)
            continue;
        auto* mouseSelector = selector(mouse);
        auto* keyboardSelector = selector(keyboard);
        int mouseChanges = 0, keyboardChanges = 0;
        QObject::connect(mouseSelector, &QComboBox::currentIndexChanged, &window, [&] { ++mouseChanges; });
        QObject::connect(keyboardSelector, &QComboBox::currentIndexChanged, &window,
                         [&] { ++keyboardChanges; });
        scanFixture(service, fixture);
        check(mouseSelector->currentData().toString() == expectedMouse,
              "mouse selection follows current/saved/binding/unique-known/single preference");
        check(keyboardSelector->currentData().toString() == expectedKeyboard,
              "keyboard restores saved choice or selects a single device without model guesses");
        check(mouseChanges == 0 && keyboardChanges == 0, "programmatic population blocks selection signals");
        if (expectedMouse.isEmpty())
            check(mouseSelector->currentIndex() == -1 &&
                      mouseSelector->placeholderText() ==
                          QString("已发现 %1 台，请选择鼠标").arg(mouseSelector->count()),
                  "ambiguous mouse list explains discovered count instead of waiting for discovery");
        if (expectedKeyboard.isEmpty())
            check(keyboardSelector->currentIndex() == -1 &&
                      keyboardSelector->placeholderText() ==
                          QString("已发现 %1 台，请选择键盘").arg(keyboardSelector->count()),
                  "ambiguous keyboard list explains discovered count without guessing model");
        if (scenario == 0) {
            mouseSelector->setCurrentIndex(mouseSelector->findData(mouseA));
            keyboardSelector->setCurrentIndex(keyboardSelector->findData(keyboardB));
            QSettings disk(directory.path() + "/settings.ini", QSettings::IniFormat);
            disk.sync();
            check(disk.value("mouse/selectedId") == mouseA && disk.value("keyboard/selectedId") == keyboardB,
                  "manual mouse and keyboard selections are saved immediately to settings.ini");
            check(mouseChanges == 1 && keyboardChanges == 1, "only explicit choices emit selection signals");
            std::reverse(fixture.hid.begin(), fixture.hid.end());
            scanFixture(service, fixture);
            check(mouseSelector->currentData() == mouseA && keyboardSelector->currentData() == keyboardB,
                  "current explicit choices survive reordered scans even when unique F2 is present");
            check(mouseChanges == 1 && keyboardChanges == 1, "refresh never emits another selection change");
            auto failed = fixture;
            failed.failHid = true;
            scanFixture(service, failed);
            check(mouseSelector->currentData() == mouseA && keyboardSelector->currentData() == keyboardB,
                  "failed HID enumeration never lets automatic preference steal current selection");
            check(mouseChanges == 1 && keyboardChanges == 1, "failed enumeration emits no selection changes");
            scanFixture(service, fixture);
            check(mouseSelector->currentData() == mouseA && keyboardSelector->currentData() == keyboardB,
                  "recovered HID enumeration retains current choices");
            check(mouseChanges == 1 && keyboardChanges == 1,
                  "recovered enumeration keeps population signals blocked");
        }
        check(fake::writeCount() == 0, "selection preferences and settings persistence never write hardware");
    }
}

void savedScrollChecks(const QString& output) {
    QTemporaryDir directory(output + "/scroll-data-XXXXXX");
    check(directory.isValid(), "scroll test uses fresh independent profile directory");
    fake::Fixture fixture;
    for (int index = 0; index < 14; ++index) {
        const auto identity = QString("scroll-fixture-%1").arg(index, 2, 10, QChar('0')).toStdWString();
        const auto name = QString("模拟档案行 %1").arg(index, 2, 10, QChar('0')).toStdWString();
        fixture.hid.push_back(hid(identity.c_str(), identity.c_str(), name.c_str(), 2));
    }
    DeviceService service(false);
    MainWindow window(&service, directory.path());
    window.resize(1030, 760);
    window.statusBar()->showMessage("模拟验证：14 条设备档案，用于检查刷新时的浏览位置。");
    window.show();
    scanFixture(service, fixture);
    button(window, "已保存设备")->click();
    settle();
    auto* list = window.findChild<QTreeWidget*>();
    auto* detail = window.findChild<QPlainTextEdit*>();
    check(list && detail && leaves(list).size() == 14,
          "saved tree displays 14 independent simulated profiles");
    if (!list || !detail || leaves(list).size() != 14)
        return;
    const auto items = leaves(list);
    list->setCurrentItem(items.front());
    // 收窄详情高度，覆盖同一设备详情发生更新时的实际纵向滚动恢复。
    detail->setFixedHeight(110);
    settle();
    check(list->verticalScrollBar()->maximum() > 0 && detail->verticalScrollBar()->maximum() > 0,
          "14-row list and constrained detail both require vertical scrolling");
    list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum());
    detail->verticalScrollBar()->setValue(detail->verticalScrollBar()->maximum());
    const auto selected = list->currentItem()->data(0, Qt::UserRole);
    const int listScroll = list->verticalScrollBar()->value();
    const int detailScroll = detail->verticalScrollBar()->value();
    fixture.hid[0].product = L"模拟档案更新 00";
    scanFixture(service, fixture);
    for (int row = 0; row < leaves(list).size(); ++row)
        check(leaves(list)[row] == items[row], "normal profile refresh reuses each tree leaf identity");
    check(list->currentItem()->data(0, Qt::UserRole) == selected,
          "refresh preserves selected stable profile ID");
    check(list->verticalScrollBar()->value() == listScroll,
          "14-row list preserves vertical scroll across scan");
    check(detail->toPlainText().contains("模拟档案更新 00"), "same-profile detail text is actually updated");
    check(detail->verticalScrollBar()->value() == detailScroll,
          "same-profile detail preserves vertical scroll");
    auto failed = fixture;
    failed.failHid = true;
    scanFixture(service, failed);
    check(list->verticalScrollBar()->value() == listScroll &&
              detail->verticalScrollBar()->value() == detailScroll,
          "unavailable-status update also preserves list and detail scrolling");
    fixture.hid.push_back(hid(L"scroll-fixture-99", L"scroll-fixture-99", L"模拟新档案 99", 2));
    scanFixture(service, fixture);
    check(leaves(list).size() == 15 && list->currentItem()->data(0, Qt::UserRole) == selected,
          "profile-set change retains selected stable profile");
    check(list->verticalScrollBar()->value() == listScroll, "profile-set rebuild restores previous scroll");
    check(window.grab().save(output + "/saved-scroll-fixture.png"),
          "save explicitly simulated scrolling fixture");
    check(fake::writeCount() == 0, "saved-profile scrolling and refresh never issue hardware writes");
}

void clickGroup(QTreeWidget* tree, QTreeWidgetItem* item, bool arrow = false) {
    tree->scrollToItem(item);
    settle();
    const auto rect = tree->visualItemRect(item);
    const QPoint point(arrow ? rect.left() - tree->indentation() / 2
                             : rect.left() + std::min(55, rect.width() / 2),
                       rect.center().y());
    const QPoint global = tree->viewport()->mapToGlobal(point);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(global), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &press);
    QApplication::sendEvent(tree->viewport(), &release);
    settle();
}

void classificationChecks(const QString& output) {
    QTemporaryDir directory(output + "/category-data-XXXXXX");
    check(directory.isValid(), "category fixture uses fresh independent archive directory");
    QJsonArray records;
    const auto append = [&records](const QString& id, const QString& name, const QString& category,
                                   const QString& direction = QString{}) {
        QJsonObject record{{"id", id},
                           {"name", name},
                           {"category", category},
                           {"lastSeen", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                           {"settings", QJsonObject{}},
                           {"confirmedSettings", QJsonArray{}}};
        if (!direction.isNull())
            record["direction"] = direction;
        records.append(record);
    };
    append("legacy-mouse", "模拟旧鼠标", "mouse");
    append("legacy-keyboard", "模拟旧键盘", "keyboard");
    append("legacy-monitor", "模拟旧显示器", "monitor");
    append("audio:legacy-untyped", "模拟 麦克风 名称不代表方向", "audio");
    append("audio:opaque-output", "模拟 麦克风字样 输出端点", "audio", "output");
    append("audio:opaque-input", "模拟 扬声器字样 输入端点", "audio", "input");
    append("legacy-other", "模拟其他设备", "controller");
    QFile file(directory.path() + "/devices.json");
    check(file.open(QIODevice::WriteOnly), "create schema-1 legacy metadata fixture");
    file.write(QJsonDocument(QJsonObject{{"schema", 1}, {"profiles", records}}).toJson());
    file.close();
    DeviceService service(false);
    MainWindow window(&service, directory.path());
    window.statusBar()->showMessage("模拟验证：分类与音频方向仅根据已保存的系统元数据。");
    window.show();
    button(window, "已保存设备")->click();
    settle();
    auto* tree = window.findChild<QTreeWidget*>("已保存设备分类树");
    auto* detail = window.findChild<QPlainTextEdit*>();
    check(tree && detail && tree->topLevelItemCount() == 5 && leaves(tree).size() == 7,
          "saved records are grouped under five nonempty device categories");
    if (!tree || !detail)
        return;
    for (const auto& key :
         {QString("mouse"), QString("keyboard"), QString("monitor"), QString("audio"), QString("other"),
          QString("audio/output"), QString("audio/input"), QString("audio/unknown")}) {
        auto* node = group(tree, key);
        check(node && node->isExpanded(), "initial nonempty categories and audio directions are expanded");
    }
    const QMap<QString, QString> expected{{"legacy-mouse", "mouse"},
                                          {"legacy-keyboard", "keyboard"},
                                          {"legacy-monitor", "monitor"},
                                          {"legacy-other", "other"},
                                          {"audio:legacy-untyped", "audio/unknown"},
                                          {"audio:opaque-output", "audio/output"},
                                          {"audio:opaque-input", "audio/input"}};
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        auto* item = leaf(tree, it.key());
        check(item && item->parent() && item->parent()->data(0, Qt::UserRole + 1) == it.value(),
              "profile appears under category and authoritative direction rather than device name");
    }
    tree->setCurrentItem(leaf(tree, "legacy-monitor"));
    const auto monitorDetail = detail->toPlainText();
    int mouseToggles = 0;
    const auto countMouseToggle = [&](QTreeWidgetItem* item) {
        if (item == group(tree, "mouse"))
            ++mouseToggles;
    };
    QObject::connect(tree, &QTreeWidget::itemExpanded, &window, countMouseToggle);
    QObject::connect(tree, &QTreeWidget::itemCollapsed, &window, countMouseToggle);
    clickGroup(tree, group(tree, "mouse"));
    check(!group(tree, "mouse")->isExpanded() && mouseToggles == 1,
          "clicking category text toggles exactly once");
    clickGroup(tree, group(tree, "mouse"), true);
    check(group(tree, "mouse")->isExpanded() && mouseToggles == 2,
          "clicking category arrow expands exactly once without itemClicked double toggle");
    clickGroup(tree, group(tree, "mouse"), true);
    check(!group(tree, "mouse")->isExpanded() && mouseToggles == 3,
          "clicking category arrow again collapses exactly once");
    check(detail->toPlainText() == monitorDetail, "category click does not replace selected device detail");
    clickGroup(tree, group(tree, "audio/input"));
    check(!group(tree, "audio/input")->isExpanded(), "audio subgroup supports independent collapse");
    auto* unknown = leaf(tree, "audio:legacy-untyped");
    scanFixture(service, {});
    check(!group(tree, "mouse")->isExpanded() && !group(tree, "audio/input")->isExpanded(),
          "category and direction collapse persist across timer-style refresh");
    check(leaf(tree, "audio:legacy-untyped") == unknown && leaves(tree).size() == 7,
          "offline legacy records keep the same tree leaf instances");
    tree->setCurrentItem(unknown);
    check(detail->toPlainText().contains("方向待识别"),
          "old audio archive displays unknown direction explicitly");
    fake::Fixture reconnected;
    reconnected.audio = {audio(L"legacy-untyped", L"模拟 重连后按capture确认输入", true)};
    scanFixture(service, reconnected);
    check(leaf(tree, "audio:legacy-untyped") == unknown && unknown->parent() == group(tree, "audio/input"),
          "reconnection moves existing unknown audio leaf to actual capture direction");
    check(!group(tree, "audio/unknown"), "empty audio direction group disappears after metadata is learned");
    check(!group(tree, "audio/input")->isExpanded(),
          "direction metadata update preserves user's collapsed group");
    scanFixture(service, {});
    check(leaves(tree).size() == 7 && unknown->parent() == group(tree, "audio/input") &&
              unknown->text(0).contains("未接入"),
          "detached device stays under its last confirmed category and direction");
    check(fake::writeCount() == 0,
          "grouping, folding, selection and metadata classification cause zero writes");
    check(window.grab().save(output + "/categories-fixture.png"),
          "save explicitly simulated category-tree view");
}

void sendWheel(QWidget* target, int vertical, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
               const QPoint& pixel = {}, Qt::ScrollPhase phase = Qt::NoScrollPhase) {
    const auto position = QPointF(target->rect().center());
    QWheelEvent event(position, target->mapToGlobal(position), pixel, QPoint(0, vertical), Qt::NoButton,
                      modifiers, phase, false);
    QApplication::sendEvent(target, &event);
    settle();
}

void sendKey(QWidget* target, int key, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QApplication::sendEvent(target, &press);
    QApplication::sendEvent(target, &release);
    settle();
}

void wheelMainWindowChecks(const QString& output) {
    QTemporaryDir directory(output + "/wheel-window-data-XXXXXX");
    DeviceService service(false);
    MainWindow window(&service, directory.path());
    fake::Fixture fixture;
    fixture.hid = {hid(L"wheel-window-mouse", L"wheel-window-mouse", L"模拟滚轮鼠标", 2),
                   hid(L"wheel-window-keyboard", L"wheel-window-keyboard", L"模拟滚轮键盘", 6)};
    fixture.monitors = {monitor(L"wheel-window-monitor", L"模拟滚轮显示器", 47)};
    fixture.audio = {audio(L"wheel-window-output", L"ATK N9 模拟滚轮输出"),
                     audio(L"wheel-window-input", L"ATK N9 模拟滚轮输入", true)};
    window.show();
    scanFixture(service, fixture);
    for (auto* card : window.findChildren<DeviceCard*>())
        card->setExpanded(true);
    window.findChild<QPushButton*>("monitorControlsRefresh")->click();
    check(waitUntil([&] { return window.findChild<QSpinBox*>("monitorContrastEditor")->isEnabled(); }),
          "wheel integration reads simulated native monitor controls without native backend");
    service.monitorFeatureProgress("模拟请求已发送，正在确认");
    check(window.findChild<QLabel*>("monitorControlsStatus")->text() == "模拟请求已发送，正在确认",
          "native monitor progress appears without claiming final success");
    service.monitorFeatureFinished("模拟最终确认");
    check(window.findChild<QLabel*>("monitorControlsStatus")->text() == "模拟最终确认",
          "native monitor final result replaces its progress message");
    QList<QComboBox*> combos;
    QList<QSlider*> sliders;
    for (auto* card : window.findChildren<DeviceCard*>()) {
        combos.append(card->findChildren<QComboBox*>());
        sliders.append(card->findChildren<QSlider*>());
    }
    const auto spins = window.findChildren<QSpinBox*>();
    // 8 个选择器 + 显示器 3 个高级选项组合框 + 鼠标写入区 2 个组合框；
    // 4 个数值编辑器 + 鼠标写入区的 DPI 输入。
    check(combos.size() == 10 && spins.size() == 5 && sliders.size() == 3,
          "actual MainWindow inventory covers every selector native option precision editor and live slider");
    QList<QWidget*> inputs;
    int changes = 0;
    for (auto* input : combos) {
        inputs.append(input);
        QObject::connect(input, &QComboBox::currentIndexChanged, &window, [&](int) { ++changes; });
    }
    for (auto* input : spins) {
        inputs.append(input);
        inputs.append(input->findChild<QLineEdit*>());
        QObject::connect(input, &QSpinBox::valueChanged, &window, [&](int) { ++changes; });
    }
    for (auto* input : sliders) {
        inputs.append(input);
        QObject::connect(input, &QSlider::valueChanged, &window, [&](int) { ++changes; });
    }
    for (auto* input : inputs) {
        if (!input->isEnabled())
            continue;
        for (auto* parent = input->parentWidget(); parent; parent = parent->parentWidget())
            if (auto* card = qobject_cast<DeviceCard*>(parent)) {
                card->setExpanded(!card->quickLayout()->parentWidget()->isAncestorOf(input));
                break;
            }
        settle();
        QScrollArea* scroll = nullptr;
        for (auto* parent = input->parentWidget(); parent; parent = parent->parentWidget())
            if ((scroll = qobject_cast<QScrollArea*>(parent)))
                break;
        check(scroll, "every active production input is inside its card scrolling page");
        if (!scroll)
            continue;
        // 仅模拟夹具增加内容高度，确保常用页与详情页都实际具有可滚动范围。
        if (!scroll->property("wheelFixtureSpacing").toBool()) {
            qobject_cast<QVBoxLayout*>(scroll->widget()->layout())->addSpacing(400);
            scroll->setProperty("wheelFixtureSpacing", true);
        }
        settle();
        scroll->verticalScrollBar()->setValue(0);
        sendWheel(input, -120);
        check(scroll->verticalScrollBar()->value() > 0,
              "wheel on actual card input scrolls its own page instead of editing");
        sendWheel(input, 120);
        check(changes == 0 && fake::writeCount() == 0,
              "actual live-connected fake controls emit no value changes or device writes on wheel");
    }
}

void wheelGuardChecks() {
    // 仅控件事件验证，不连接 DeviceService；明确点击、键盘与拖动也不可能访问设备。
    QScrollArea area;
    area.resize(520, 390);
    area.setWidgetResizable(true);
    area.setStyleSheet(comboBoxStyleSheet());
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    auto* combo = createThemedComboBox();
    for (int row = 0; row < 100; ++row)
        combo->addItem(QString("模拟滚轮选项 %1").arg(row), row);
    combo->setCurrentIndex(50);
    auto* spin = createThemedSpinBox();
    spin->setRange(0, 100);
    spin->setKeyboardTracking(false);
    spin->setValue(50);
    auto* slider = createThemedSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setValue(50);
    auto* percent = new PercentDetails("模拟百分比", {20, 50, 80});
    percent->setAvailable(true);
    percent->setValue(50);
    auto* native = new MonitorControls;
    platform::MonitorControlsInfo info;
    info.id = L"wheel-only-simulated-monitor";
    for (unsigned code : {0x12, 0x60, 0xDC, 0x14}) {
        platform::MonitorFeatureInfo feature;
        feature.code = static_cast<std::uint8_t>(code);
        feature.advertised = feature.readable = feature.writable = true;
        feature.maximum = 100;
        feature.current = 50;
        feature.allowedValues = {20, 50, 80};
        info.features.push_back(feature);
    }
    native->setInfo(info);
    for (auto* widget :
         {static_cast<QWidget*>(combo), static_cast<QWidget*>(spin), static_cast<QWidget*>(slider),
          static_cast<QWidget*>(percent), static_cast<QWidget*>(native)})
        layout->addWidget(widget);
    layout->addSpacing(900);
    area.setWidget(content);
    area.show();
    settle();
    int changed = 0, activated = 0, requested = 0;
    for (auto* input : content->findChildren<QComboBox*>()) {
        QObject::connect(input, &QComboBox::currentIndexChanged, &area, [&](int) { ++changed; });
        QObject::connect(input, &QComboBox::activated, &area, [&](int) { ++activated; });
    }
    for (auto* input : content->findChildren<QSpinBox*>())
        QObject::connect(input, &QSpinBox::valueChanged, &area, [&](int) { ++changed; });
    QObject::connect(slider, &QSlider::valueChanged, &area, [&](int) { ++changed; });
    QObject::connect(percent, &PercentDetails::valueRequested, &area, [&](int) { ++requested; });
    QObject::connect(native, &MonitorControls::valueRequested, &area, [&](int, int) { ++requested; });

    QList<QWidget*> inputs;
    for (auto* input : content->findChildren<QComboBox*>())
        inputs.append(input);
    for (auto* input : content->findChildren<QSpinBox*>()) {
        inputs.append(input);
        inputs.append(input->findChild<QLineEdit*>());
    }
    inputs.append(slider);
    for (auto* input : inputs) {
        check(input && input->isEnabled(), "wheel fixture includes enabled selector number editor or slider");
        if (!input)
            continue;
        check(input->focusPolicy() == Qt::StrongFocus,
              "input retains click and Tab focus without wheel focus stealing");
        for (bool focused : {false, true}) {
            area.ensureWidgetVisible(input);
            focused ? input->setFocus() : input->clearFocus();
            settle();
            auto* focus = QApplication::focusWidget();
            const int before = area.verticalScrollBar()->value();
            sendWheel(input, -120);
            const int down = area.verticalScrollBar()->value();
            check(down > before, "wheel over focused or unfocused input scrolls surrounding details down");
            sendWheel(input, 120);
            check(area.verticalScrollBar()->value() < down,
                  "wheel over focused or unfocused input scrolls surrounding details up");
            check(QApplication::focusWidget() == focus, "wheel keeps editing focus unchanged");
            sendWheel(input, -120, Qt::ControlModifier);
            sendWheel(input, 120, Qt::ShiftModifier);
            sendWheel(input, 0, Qt::NoModifier, QPoint(0, -25), Qt::ScrollBegin);
            sendWheel(input, 0, Qt::NoModifier, QPoint(0, -25), Qt::ScrollUpdate);
            sendWheel(input, 0, Qt::NoModifier, {}, Qt::ScrollEnd);
            check(changed == 0 && activated == 0 && requested == 0,
                  "angle pixel modifiers and scroll phases never change inputs or submit control requests");
        }
    }
    area.ensureWidgetVisible(combo);
    settle();
    const int closedIndex = combo->currentIndex();
    const int outerScroll = area.verticalScrollBar()->value();
    combo->showPopup();
    settle();
    auto* view = combo->view();
    auto* popupScroll = view->verticalScrollBar();
    check(popupScroll->maximum() > 0, "expanded selector fixture has a scrollable option list");
    popupScroll->setValue(0);
    sendWheel(view->viewport(), -120);
    check(popupScroll->value() > 0, "wheel inside expanded selector scrolls its option list");
    sendWheel(popupScroll, -120);
    check(combo->currentIndex() == closedIndex && changed == 0 && activated == 0,
          "popup list and scrollbar wheel never commit a device or feature selection");
    check(area.verticalScrollBar()->value() == outerScroll,
          "expanded selector wheel does not scroll the background card");

    const auto chosen = combo->model()->index(5, 0);
    view->scrollTo(chosen, QAbstractItemView::PositionAtCenter);
    settle();
    QElapsedTimer releaseDelay;
    releaseDelay.start();
    while (releaseDelay.elapsed() < 200)
        QApplication::processEvents();
    const auto click = view->visualRect(chosen).center();
    const auto global = view->viewport()->mapToGlobal(click);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(click), QPointF(global), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(click), QPointF(global), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &press);
    QApplication::sendEvent(view->viewport(), &release);
    settle();
    check(combo->currentIndex() == 5 && activated == 1,
          "explicit popup option click still commits the selected item once");
    combo->hidePopup();
    sendKey(combo, Qt::Key_Down);
    check(combo->currentIndex() == 6, "closed selector keyboard navigation remains available");
    combo->showPopup();
    settle();
    sendKey(combo, Qt::Key_Down);
    sendKey(combo, Qt::Key_Return);
    check(combo->currentIndex() == 7, "expanded selector keyboard navigation and Enter still commit");
    combo->hidePopup();

    area.ensureWidgetVisible(spin);
    auto* edit = spin->findChild<QLineEdit*>();
    edit->setFocus();
    edit->selectAll();
    sendKey(edit, Qt::Key_6, "6");
    sendKey(edit, Qt::Key_7, "7");
    check(spin->value() == 50 && edit->text() == "67", "numeric editor keeps uncommitted typed text");
    const int beforePendingWheel = changed;
    sendWheel(edit, -120);
    check(spin->value() == 50 && edit->text() == "67" && changed == beforePendingWheel && edit->hasFocus(),
          "wheel on spin inner text preserves pending edit without committing or stealing focus");
    sendKey(edit, Qt::Key_Return);
    check(spin->value() == 67, "numeric text and Enter still commit after wheel navigation");
    sendKey(spin, Qt::Key_Up);
    check(spin->value() == 68, "numeric keyboard arrow still adjusts the value");

    area.ensureWidgetVisible(slider);
    slider->setValue(50);
    sendKey(slider, Qt::Key_Right);
    check(slider->value() == 51, "slider keyboard arrow still adjusts the value");
    slider->setValue(50);
    QStyleOptionSlider option;
    option.initFrom(slider);
    option.orientation = slider->orientation();
    option.minimum = slider->minimum();
    option.maximum = slider->maximum();
    option.sliderPosition = slider->sliderPosition();
    option.sliderValue = slider->value();
    const auto handle =
        slider->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider);
    const auto start = handle.center();
    const auto finish = start + QPoint(45, 0);
    QMouseEvent dragPress(QEvent::MouseButtonPress, QPointF(start), slider->mapToGlobal(QPointF(start)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent dragMove(QEvent::MouseMove, QPointF(finish), slider->mapToGlobal(QPointF(finish)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent dragRelease(QEvent::MouseButtonRelease, QPointF(finish), slider->mapToGlobal(QPointF(finish)),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(slider, &dragPress);
    QApplication::sendEvent(slider, &dragMove);
    QApplication::sendEvent(slider, &dragRelease);
    check(slider->value() > 50, "slider pointer dragging remains available");
    auto* nativeStatus = native->findChild<QLabel*>("monitorControlsStatus");
    native->setBusy(true);
    native->setOperationStatus("模拟请求已发送，正在确认");
    native->setInfo(info);
    native->setStatus("模拟普通读取提示");
    check(nativeStatus->text() == "模拟请求已发送，正在确认",
          "same-target background info cannot erase pending operation progress");
    native->setOperationStatus("模拟目标单项已回读确认", true);
    native->setInfo(info);
    check(nativeStatus->text().contains("模拟目标单项已回读确认") &&
              nativeStatus->text().contains("正在后台补读其他功能"),
          "final feedback remains visible while background full read is busy");
    native->setBusy(false);
    native->setInfo(info);
    check(nativeStatus->text() == "模拟目标单项已回读确认",
          "completed background read leaves final operation result visible without timer");
    auto otherInfo = info;
    otherInfo.id = L"wheel-only-other-monitor";
    native->setInfo(otherInfo);
    check(!nativeStatus->text().contains("模拟目标单项"),
          "changing target clears old feedback immediately without delayed timer callback");
    native->setOperationStatus("模拟旧操作反馈", true);
    native->findChild<QPushButton*>("monitorControlsRefresh")->click();
    check(!nativeStatus->text().contains("模拟旧操作反馈"),
          "explicit advanced refresh clears prior operation feedback");
    native->setOperationStatus("模拟再次旧操作反馈", true);
    native->findChild<QPushButton*>("monitorFeature18Apply")->click();
    check(!nativeStatus->text().contains("模拟再次旧操作反馈"),
          "explicit native value apply clears prior feedback before emitting next request");
    native->setOperationStatus("模拟断开前反馈", true);
    native->setBusy(true);
    native->clear();
    check(nativeStatus->text() == "尚未读取显示器内置功能。",
          "detaching target clears prior feedback and preserves initial state hint");
    check(native->findChild<QPushButton*>("monitorControlsRefresh")->isEnabled(),
          "detaching target also resets old busy state for future native refresh");
    check(fake::writeCount() == 0, "all wheel and explicit interaction checks use isolated UI only");
}

void savedInitialCollapseChecks(const QString& output) {
    for (bool allCollapsed : {false, true}) {
        QTemporaryDir directory(output + "/initial-collapse-data-XXXXXX");
        check(directory.isValid(), "initial saved-page collapse uses fresh independent data directory");
        QJsonArray records;
        const auto append = [&records](const QString& id, const QString& name, const QString& category,
                                       const QString& direction = QString{}) {
            records.append(
                QJsonObject{{"id", id},
                            {"name", name},
                            {"category", category},
                            {"direction", direction},
                            {"lastSeen", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                            {"settings", QJsonObject{}},
                            {"confirmedSettings", QJsonArray{}}});
        };
        append("initial-mouse", "模拟首次进入鼠标", "mouse");
        append("initial-keyboard", "模拟首次进入键盘", "keyboard");
        append("initial-monitor", "模拟首次进入显示器", "monitor");
        append("audio:initial-output", "模拟首次进入输出", "audio", "output");
        append("audio:initial-input", "模拟首次进入输入", "audio", "input");
        QFile file(directory.path() + "/devices.json");
        check(file.open(QIODevice::WriteOnly), "seed saved records before constructing new window");
        file.write(QJsonDocument(QJsonObject{{"schema", 1}, {"profiles", records}}).toJson());
        file.close();
        {
            QSettings preferences(directory.path() + "/settings.ini", QSettings::IniFormat);
            for (const auto& key : {QString("mouse"), QString("keyboard"), QString("audio/output")})
                preferences.setValue("saved/groups/" + key, false);
            for (const auto& key : {QString("monitor"), QString("audio"), QString("audio/input")})
                preferences.setValue("saved/groups/" + key, !allCollapsed);
            preferences.sync();
        }
        DeviceService service(false);
        MainWindow window(&service, directory.path());
        window.statusBar()->showMessage("模拟验证：首次进入保存页尊重重启前分类折叠偏好。");
        window.show();
        settle();
        button(window, "已保存设备")->click();
        settle();
        auto* tree = window.findChild<QTreeWidget*>("已保存设备分类树");
        auto* detail = window.findChild<QPlainTextEdit*>();
        check(tree && detail && leaves(tree).size() == 5,
              "new window first saved-page entry loads pre-existing saved records");
        if (!tree || !detail)
            continue;
        for (const auto& key : {QString("mouse"), QString("keyboard"), QString("audio/output")}) {
            auto* item = group(tree, key);
            check(item && !item->isExpanded(),
                  "first saved-page show preserves saved mouse keyboard and output collapse");
        }
        if (allCollapsed) {
            for (const auto& key : {QString("monitor"), QString("audio"), QString("audio/input")}) {
                auto* item = group(tree, key);
                check(item && !item->isExpanded(), "all-collapsed first entry never expands a category");
            }
            check(!tree->currentItem() || tree->currentItem()->data(0, Qt::UserRole).toString().isEmpty(),
                  "all-collapsed first entry does not select a hidden device leaf");
            check(detail->toPlainText().contains("接入设备后，将在这里保留它的最后设置"),
                  "all-collapsed initial view retains empty detail hint");
        } else {
            check(tree->currentItem() == leaf(tree, "initial-monitor") &&
                      detail->toPlainText().contains("模拟首次进入显示器"),
                  "first entry chooses visible monitor rather than hidden first mouse leaf");
        }
        scanFixture(service, {});
        button(window, "设备总览")->click();
        button(window, "已保存设备")->click();
        settle();
        for (const auto& key : {QString("mouse"), QString("keyboard"), QString("audio/output")})
            check(!group(tree, key)->isExpanded(), "saved collapse survives refresh and page re-entry");
        check(fake::writeCount() == 0, "initial archived-profile fallback never writes hardware");
    }
}

void manualRefreshChecks(const QString& output) {
    QTemporaryDir directory(output + "/refresh-data-XXXXXX");
    DeviceService service(false);
    MainWindow window(&service, directory.path());
    auto* refresh = window.findChild<QPushButton*>("primary");
    auto* notice = window.findChild<QLabel*>("noticeText");
    check(refresh && notice, "manual refresh has named button and visible feedback region");
    if (!refresh || !notice)
        return;
    {
        std::lock_guard lock(fake::mutex);
        fake::state = {};
    }
    const int enumerationsBefore = fake::enumerations;
    fake::hidEntered = fake::releaseHid = false;
    fake::holdNextHid = true;
    refresh->click();
    check(!refresh->isEnabled() && refresh->text() == "正在刷新…" && notice->text().contains("正在重新读取"),
          "manual refresh immediately disables button and displays working feedback");
    check(waitUntil([] { return fake::hidEntered.load(); }), "new full scan enters fake backend gate");
    service.refreshed();
    service.refreshFinished(999999, true, "wrong-ticket-result");
    check(!refresh->isEnabled() && notice->text().contains("正在重新读取"),
          "partial refreshed and unrelated completion ticket cannot finish manual feedback");
    fake::releaseHid = true;
    fake::condition.notify_all();
    check(waitUntil([&] { return refresh->isEnabled(); }),
          "matching full scan ticket restores refresh button");
    check(notice->text().contains("状态已刷新") && notice->text().contains("采样时间") &&
              fake::enumerations >= enumerationsBefore + 3,
          "manual completion displays actual full-scan result and sampled time");
    const auto result = notice->text();
    service.refreshed();
    check(notice->text() == result, "ordinary refreshed does not immediately erase manual completion result");
    {
        std::lock_guard lock(fake::mutex);
        fake::state.failHid = fake::state.failAudio = fake::state.failMonitor = true;
    }
    refresh->click();
    check(waitUntil([&] { return refresh->isEnabled(); }),
          "failed full scan also releases manual refresh button");
    check(notice->text().contains("读取失败") && notice->text().contains("采样时间"),
          "manual failure feedback reports actual errors instead of unconditional success");
    check(fake::writeCount() == 0, "manual refresh performs only fake reads");
}

void longNameChecks(const QString& output) {
    QTemporaryDir directory(output + "/long-name-data-XXXXXX");
    fake::Fixture fixture;
    auto mouse = hid(L"long-name-mouse", L"long-name-mouse", L"F2 Master+", 2);
    mouse.vid = 0x3554;
    mouse.pid = 0xFB56;
    mouse.serial = L"TEST0001";
    fixture.hid.push_back(mouse);
    fixture.hid.push_back(hid(L"long-name-keyboard", L"long-name-keyboard",
                              L"模拟 LANGTU L98 超长键盘名称：有线与无线多模设备・完整型号后缀", 6));
    const QString unicode = QString("模拟中文 العربية 🔊 Unicode 长名称・完整设备描述 ").repeated(7);
    fixture.audio = {audio(L"long-name-output", ("ATK 输出 " + unicode).toStdWString().c_str()),
                     audio(L"long-name-input", ("ATK 输入 " + unicode).toStdWString().c_str(), true)};
    fixture.audio[1].volumePercent = 71;
    fixture.monitors = {monitor(
        L"long-name-monitor",
        (QString("模拟 KTC H24T7 DisplayPort 超长显示器型号・完整后缀 ").repeated(4)).toStdWString().c_str(),
        47)};
    DeviceService service(false);
    MainWindow window(&service, directory.path());
    window.statusBar()->showMessage(
        "模拟验证：实机同长度鼠标条目与更长 Unicode 名称，不读取或写入真实设备。");
    window.show();
    scanFixture(service, fixture);
    check(waitUntil([&] { return !service.monitorControlsBusy(); }),
          "long-name monitor capability read completes");
    DeviceCard* mouseCard = nullptr;
    for (auto* card : window.findChildren<DeviceCard*>())
        if (card->cardId() == "mouse")
            mouseCard = card;
    check(mouseCard &&
              selector(mouseCard)->currentText() == "VGN Dragonfly F2 Master+ · 2.4G 接收器 · TEST0001",
          "long-name fixture reproduces exact physical mouse entry length");
    for (const QSize size : {QSize(1210, 830), QSize(1030, 760)}) {
        window.resize(size);
        settle();
        for (auto* card : window.findChildren<DeviceCard*>()) {
            auto* page = card->quickLayout()->parentWidget();
            auto* scroll = qobject_cast<QScrollArea*>(page->parentWidget()->parentWidget());
            check(scroll && page->width() <= scroll->viewport()->width(),
                  "long names never widen card content beyond horizontal viewport");
            if (!scroll)
                continue;
            for (auto* combo : page->findChildren<QComboBox*>()) {
                scroll->ensureWidgetVisible(combo, 0, 0);
                settle();
                check(combo->sizeAdjustPolicy() == QComboBox::AdjustToMinimumContentsLengthWithIcon &&
                          combo->minimumContentsLength() == 12 &&
                          combo->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding,
                      "every quick selector uses centrally configured compressible sizing");
                const QRect inViewport(combo->mapTo(scroll->viewport(), QPoint{}), combo->size());
                check(
                    scroll->viewport()->rect().contains(inViewport),
                    "closed selector is fully reachable inside viewport at default and minimum window size");
                QStyleOptionComboBox option;
                option.initFrom(combo);
                option.rect = combo->rect();
                const auto arrow = combo->style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                                  QStyle::SC_ComboBoxArrow, combo);
                const QRect arrowInViewport(combo->mapTo(scroll->viewport(), arrow.topLeft()), arrow.size());
                check(arrow.width() > 0 && combo->rect().contains(arrow) &&
                          scroll->viewport()->rect().contains(arrowInViewport),
                      "right-side dropdown arrow stays geometrically visible and unclipped");
                const auto full = combo->currentText();
                QHelpEvent help(QEvent::ToolTip, combo->rect().center(),
                                combo->mapToGlobal(combo->rect().center()));
                QApplication::sendEvent(combo, &help);
                check(QToolTip::text() == full, "closed selector tooltip retains full unmodified model text");
                QToolTip::hideText();
                combo->showPopup();
                settle();
                const int required = combo->fontMetrics().horizontalAdvance(full);
                std::cout << "LONG_POPUP " << combo->objectName().toStdString()
                          << " text=" << full.left(42).toStdString() << " required=" << required
                          << " view=" << combo->view()->viewport()->width()
                          << " elide=" << combo->view()->textElideMode()
                          << " class=" << combo->view()->metaObject()->className()
                          << " column=" << combo->view()->sizeHintForColumn(0)
                          << " policy=" << combo->view()->horizontalScrollBarPolicy()
                          << " horizontal=" << combo->view()->horizontalScrollBar()->maximum() << '\n';
                check(combo->view()->textElideMode() == Qt::ElideNone &&
                          (combo->view()->viewport()->width() >= required ||
                           combo->view()->horizontalScrollBar()->maximum() > 0),
                      "popup exposes full long text using independent width or horizontal scrolling");
                combo->hidePopup();
                settle();
                check(combo->currentText() == full && page->width() <= scroll->viewport()->width(),
                      "popup sizing never mutates selected text or expands closed card content");
            }
            for (auto* status : page->findChildren<QLabel*>()) {
                if (status->property("role") != "status")
                    continue;
                scroll->ensureWidgetVisible(status, 0, 0);
                settle();
                const QRect inViewport(status->mapTo(scroll->viewport(), QPoint{}), status->size());
                check(scroll->viewport()->rect().contains(inViewport),
                      "status labels remain inside card and reachable after long device names");
            }
            scroll->verticalScrollBar()->setValue(0);
        }
        settle();
        const auto name = size.width() == 1210 ? "/quick-fixture.png" : "/quick-long-minimum-fixture.png";
        check(window.grab().save(output + name), "save simulated long-name card layout proof");
    }
    for (auto* combo : window.findChildren<QComboBox*>())
        check(combo->sizeAdjustPolicy() == QComboBox::AdjustToMinimumContentsLengthWithIcon &&
                  combo->minimumContentsLength() == 12,
              "advanced monitor dropdowns also use shared combo sizing factory");
    check(fake::writeCount() == 0, "long-name layout, tooltip and popup checks issue zero device writes");
}

void profileDeletionChecks(const QString& output) {
    QTemporaryDir directory(output + "/deletion-data-XXXXXX");
    const auto path = directory.path() + "/devices.json";
    {
        DeviceProfiles seed(path);
        seed.observe("monitor:delete-a", "模拟删除显示器 A", "monitor", {{"brightness", 37}});
        seed.observe("monitor:delete-b", "模拟删除显示器 B", "monitor", {{"brightness", 63}});
        seed.observe("audio:delete-output", "模拟耳机输出", "audio", {{"volume", 20}}, "output");
        seed.observe("audio:delete-input", "模拟耳机输入", "audio", {{"volume", 55}}, "input");
        check(seed.save(), "seed independent deletion fixture");
    }
    const int writesBefore = fake::writeCount();
    DeviceService service(false);
    MainWindow window(&service, directory.path());
    window.setWindowTitle("删除档案检查 · 仅模拟数据");
    window.show();
    button(window, "已保存设备")->click();
    auto* tree = window.findChild<QTreeWidget*>();
    auto* remove = window.findChild<QPushButton*>("deleteDeviceProfile");
    auto* result = window.findChild<QLabel*>("deleteProfileResult");
    tree->setCurrentItem(leaf(tree, "monitor:delete-a"));
    check(!remove->isEnabled(), "loaded offline flag alone cannot authorize deletion before enumeration");
    scanFixture(service, {});
    check(remove->isEnabled(), "successful scan confirms offline profile can be removed");
    tree->setCurrentItem(group(tree, "monitor"));
    check(!remove->isEnabled(), "category group cannot delete a previously selected leaf");
    tree->setCurrentItem(leaf(tree, "monitor:delete-a"));

    const auto confirm = [&](const std::function<void()>& duringDialog = {}, bool accept = true) {
        QTimer::singleShot(0, &window, [&, duringDialog, accept] {
            auto* dialog = window.findChild<QDialog*>("confirmProfileDeletion");
            check(dialog, "delete action opens themed explicit confirmation");
            if (!dialog)
                return;
            if (duringDialog)
                duringDialog();
            if (accept)
                dialog->findChild<QPushButton*>("confirmDeleteDeviceProfile")->click();
            else
                dialog->reject();
        });
        remove->click();
    };
    confirm({}, false);
    check(!savedRow(path, "monitor:delete-a").isEmpty(), "cancel preserves selected archive");
    check(remove->isEnabled(), "cancel restores usable offline delete button");

    fake::Fixture attached;
    attached.monitors = {monitor(L"delete-a", L"模拟删除显示器 A", 37)};
    scanFixture(service, attached);
    check(!remove->isEnabled(), "connected profile cannot be deleted");
    scanFixture(service, {});
    confirm([&] { std::lock_guard lock(fake::mutex); fake::state = attached; });
    check(!remove->isEnabled() && remove->text().contains("确认"),
          "accepted delete waits for a new full scan");
    check(waitUntil([&] { return !remove->text().contains("确认"); }), "reconnection scan finishes");
    check(result->text().contains("未删除") && !savedRow(path, "monitor:delete-a").isEmpty(),
          "reconnecting during confirmation cancels deletion");

    scanFixture(service, {});
    confirm([&] { std::lock_guard lock(fake::mutex); fake::state.failMonitor = true; });
    check(waitUntil([&] { return !remove->text().contains("确认"); }), "failed confirmation scan finishes");
    check(result->text().contains("未删除") && !savedRow(path, "monitor:delete-a").isEmpty(),
          "enumeration failure is not interpreted as physical disconnection");
    check(!remove->isEnabled(), "unknown connection state blocks another deletion");
    scanFixture(service, {});
    fake::hidEntered = fake::releaseHid = false;
    fake::holdNextHid = true;
    confirm();
    check(waitUntil([] { return fake::hidEntered.load(); }), "deletion waits at controlled fake scan gate");
    service.refreshed();
    service.refreshFinished(999999, true, "unrelated ticket");
    check(!savedRow(path, "monitor:delete-a").isEmpty(), "unrelated refresh event cannot commit deletion");
    tree->setCurrentItem(leaf(tree, "monitor:delete-b"));
    check(!remove->isEnabled(), "selection changes cannot enable concurrent delete");
    auto* refresh = window.findChild<QPushButton*>("primary");
    refresh->click(); // Replace pending scan with a newer known ticket.
    fake::releaseHid = true;
    fake::condition.notify_all();
    check(waitUntil([&] { return result->text().contains("已删除"); }), "newer requested full scan completes deletion");
    check(refresh->isEnabled(), "shared newer scan also completes manual refresh feedback");
    check(savedRow(path, "monitor:delete-a").isEmpty(), "confirmed ID is deleted despite selection change");
    check(!savedRow(path, "monitor:delete-b").isEmpty(), "newly selected different device is preserved");
    check(!leaf(tree, "monitor:delete-a") && leaf(tree, "monitor:delete-b"),
          "tree removes only deleted device and keeps its category");
    scanFixture(service, {});
    check(savedRow(path, "monitor:delete-a").isEmpty(), "background scan does not recreate absent deleted device");
    attached.monitors[0].brightnessPercent = attached.monitors[0].currentBrightness = 71;
    scanFixture(service, attached);
    check(savedRow(path, "monitor:delete-a").value("settings").toObject().value("brightness") == 71,
          "reconnect stores freshly read value instead of deleted brightness");
    check(fake::writeCount() == writesBefore, "delete and first reconnect do not write hardware settings");

    scanFixture(service, {});
    for (const auto& id : {QString("audio:delete-input"), QString("audio:delete-output"),
                           QString("monitor:delete-a"), QString("monitor:delete-b")}) {
        tree->setCurrentItem(leaf(tree, id));
        check(remove->isEnabled(), "each offline device category shares the same deletion entry");
        confirm();
        check(waitUntil([&] { return !leaf(tree, id); }), "selected leaf disappears after successful deletion");
    }
    check(leaves(tree).isEmpty() && tree->topLevelItemCount() == 0 && !remove->isEnabled(),
          "deleting last device removes empty nested categories and disables action");
    DeviceProfiles restarted(path);
    check(restarted.load() && restarted.profiles().isEmpty(), "all deletions survive reload");
    check(fake::writeCount() == writesBefore, "complete deletion scenarios perform zero fake hardware writes");
    check(window.grab().save(output + "/deleted-empty-fixture.png"), "save simulated empty archive UI");
}

void trayMenuChecks(const QString& output) {
    QTemporaryDir directory(output + "/tray-data-XXXXXX");
    check(directory.isValid(), "tray fixture uses an isolated data directory");
    DeviceService service(false);
    MainWindow window(&service, directory.path());
    window.show();
    fake::Fixture fixture;
    fixture.monitors = {monitor(L"tray-monitor", L"模拟托盘显示器", 33)};
    fixture.audio = {audio(L"tray-output", L"模拟托盘输出")};
    scanFixture(service, fixture);
    auto* menu = window.findChild<QMenu*>("trayMenu");
    check(menu, "tray menu is constructed even when the platform tray is unavailable");
    if (!menu)
        return;
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    menu->popup(QPoint(40, 40));
    settle();
    check(menu->grab().save(output + "/tray-menu.png"), "save simulated tray menu");
    menu->hide();
    auto* brightnessSlider = menu->findChild<QSlider*>("trayBrightnessSlider");
    auto* brightnessValue = menu->findChild<QLabel*>("trayBrightnessValue");
    check(brightnessSlider && brightnessValue && brightnessSlider->isEnabled() &&
              brightnessSlider->value() == 33 && brightnessValue->text() == "33%",
          "tray brightness slider reflects the selected monitor's current value");
    const int beforeBrightness = fake::writeCount();
    brightnessSlider->setValue(50);
    check(brightnessValue->text() == "50%", "tray brightness label follows the slider");
    check(waitUntil([&] { return fake::writeCount() > beforeBrightness; }),
          "tray brightness slider dispatches a write");
    {
        std::lock_guard lock(fake::mutex);
        check(!fake::writes.empty() && fake::writes.back().kind == "brightness" &&
                  fake::writes.back().value == 50,
              "tray brightness slider writes the dragged percentage");
    }

    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    auto* outputSlider = menu->findChild<QSlider*>("trayOutputSlider");
    auto* outputValue = menu->findChild<QLabel*>("trayOutputValue");
    check(outputSlider && outputValue && outputSlider->isEnabled() && outputSlider->value() == 20 &&
              outputValue->text() == "20%",
          "tray output slider reflects the selected endpoint's current volume");
    const int beforeVolume = fake::writeCount();
    outputSlider->setValue(25);
    check(outputValue->text() == "25%", "tray output label follows the slider");
    check(waitUntil([&] { return fake::writeCount() > beforeVolume; }),
          "tray output slider dispatches a write");
    {
        std::lock_guard lock(fake::mutex);
        check(!fake::writes.empty() && fake::writes.back().kind == "volume" &&
                  fake::writes.back().value == 25,
              "tray output slider writes the dragged percentage");
    }

    auto* outputMuteButton = menu->findChild<QToolButton*>("trayOutputMute");
    check(outputMuteButton && outputMuteButton->isEnabled(), "tray output mute button is clickable");
    const int beforeMute = fake::writeCount();
    outputMuteButton->click();
    check(waitUntil([&] { return fake::writeCount() >= beforeMute + 1; }),
          "first mute click dispatches a write");
    outputMuteButton->click();
    check(waitUntil([&] { return fake::writeCount() >= beforeMute + 2; }),
          "second mute click dispatches the opposite state without waiting for readback");
    {
        std::lock_guard lock(fake::mutex);
        int muted = 0, unmuted = 0;
        for (const auto& write : fake::writes) {
            if (write.kind != "mute" || write.target != L"tray-output")
                continue;
            if (write.value == 1)
                ++muted;
            else
                ++unmuted;
        }
        check(muted >= 1 && unmuted >= 1, "rapid mute clicks alternate between mute and unmute");
    }
}

void lowBatteryNotificationChecks(const QString& output) {
    QTemporaryDir directory(output + "/low-battery-data-XXXXXX");
    check(directory.isValid(), "low battery fixture uses an isolated data directory");
    DeviceService service(false);
    MainWindow window(&service, directory.path());
    window.show();
    int alerts = 0;
    QObject::connect(&window, &MainWindow::lowBatteryAlert, &window,
                     [&alerts](const QString&) { ++alerts; });
    fake::Fixture fixture;
    fixture.hid = {n9Headset(L"low-n9-path", L"low-n9-container")};
    auto endpoint = audio(L"low-audio", L"模拟低电量耳机");
    endpoint.containerId = L"low-n9-container";
    fixture.audio = {endpoint};
    fixture.n9BatteryPercent = 5;
    scanFixture(service, fixture);
    auto* notice = window.findChild<QLabel*>("noticeText");
    check(notice && notice->text().contains("电量 5%") && notice->text().contains("模拟低电量耳机"),
          "headset below 10% produces an in-app low-battery notice without a tray");
    check(alerts == 1, "low battery alert is emitted once for the first low reading");
    scanFixture(service, fixture);
    check(alerts == 1, "unchanged low battery is not notified twice");

    QTemporaryDir quietDirectory(output + "/low-battery-off-data-XXXXXX");
    QString error;
    check(AppSettings(quietDirectory.path() + "/settings.ini").save({true, true, 3, false}, error),
          "seed disabled low battery preference");
    DeviceService quietService(false);
    MainWindow quietWindow(&quietService, quietDirectory.path());
    quietWindow.show();
    int quietAlerts = 0;
    QObject::connect(&quietWindow, &MainWindow::lowBatteryAlert, &quietWindow,
                     [&quietAlerts](const QString&) { ++quietAlerts; });
    scanFixture(quietService, fixture);
    auto* quietNotice = quietWindow.findChild<QLabel*>("noticeText");
    check(quietAlerts == 0 && quietNotice && !quietNotice->text().contains("请及时充电"),
          "disabled low battery preference suppresses the notice");
}

void applicationSettingsChecks(const QString& output) {
    QTemporaryDir directory(output + "/preferences-data-XXXXXX");
    const auto settingsPath = directory.path() + "/settings.ini";
    QString error;
    check(AppSettings(settingsPath).save({true, false, 10}, error), "seed disabled restore preferences");
    {
        DeviceProfiles seed(directory.path() + "/devices.json");
        seed.observe("monitor:settings-monitor", "模拟设置显示器", "monitor", {{"brightness", 80}});
        check(seed.save(), "seed old settings value for disabled-restore scenario");
        QSettings legacy(settingsPath, QSettings::IniFormat);
        legacy.setValue("saved/groups/audio/input", false);
        legacy.setValue("mouse/boundEndpoints", QStringList{"retain-receiver", "retain-wired"});
    }
    const int writesBefore = fake::writeCount();
    DeviceService service(false);
    MainWindow window(&service, directory.path());
    window.setWindowTitle("设置模块检查 · 仅模拟数据");
    window.resize(1030, 760);
    window.show();
    fake::Fixture fixture;
    fixture.monitors = {monitor(L"settings-monitor", L"模拟设置显示器", 33)};
    scanFixture(service, fixture);
    check(fake::writeCount() == writesBefore, "startup restore preference prevents old brightness writes");
    check(savedRow(directory.path() + "/devices.json", "monitor:settings-monitor")
                  .value("settings").toObject().value("brightness") == 33,
          "disabled restore observes actual current value");
    auto* navigation = window.findChild<QPushButton*>("settingsNavigation");
    auto* page = window.findChild<SettingsPage*>();
    navigation->click();
    check(page && !page->isHidden() && navigation->isChecked(), "settings has independent selected navigation");
    auto* close = page->findChild<QComboBox*>("settingsCloseBehavior");
    auto* restore = page->findChild<QComboBox*>("settingsRestoreBehavior");
    auto* interval = page->findChild<QComboBox*>("settingsRefreshInterval");
    auto* lowBattery = page->findChild<QComboBox*>("settingsLowBatteryNotification");
    check(close && restore && interval && lowBattery,
          "all four program preferences use themed selectors");
    check(close->currentData().toBool() && !restore->currentData().toBool() &&
              interval->currentData().toInt() == 10 && lowBattery->currentData().toBool() &&
              service.refreshIntervalSeconds() == 10,
          "saved preferences initialize actual runtime and controls");
    for (auto* control : {close, restore, interval, lowBattery}) {
        const int before = control->currentIndex();
        control->setFocus();
        sendWheel(control, -120);
        check(control->currentIndex() == before, "settings wheel scroll never changes focused selection");
    }
    close->setCurrentIndex(close->findData(false));
    interval->setCurrentIndex(interval->findData(30));
    check(AppSettings(settingsPath).load() == AppPreferences{false, false, 30},
          "individual changes persist immediately");
    lowBattery->setCurrentIndex(lowBattery->findData(false));
    check(AppSettings(settingsPath).load() == AppPreferences{false, false, 30, false},
          "low battery notification toggle persists immediately");
    check(service.refreshIntervalSeconds() == 30, "poll interval changes actual service timer");
    check(!service.setRefreshIntervalSeconds(0) && service.refreshIntervalSeconds() == 30,
          "invalid direct service interval never enables a busy loop");
    restore->setCurrentIndex(restore->findData(true));
    settle();
    check(fake::writeCount() == writesBefore, "enabling restore does not replay settings to current devices");
    auto* defaults = page->findChild<QPushButton*>("settingsRestoreDefaults");
    defaults->click();
    check(AppSettings(settingsPath).load() == AppPreferences{} && service.refreshIntervalSeconds() == 3,
          "restore defaults applies and persists all program preferences");
    check(close->currentData().toBool() && restore->currentData().toBool() && interval->currentData() == 3 &&
              lowBattery->currentData().toBool(),
          "default controls synchronize without recursive writes");
    check(savedRows(directory.path() + "/devices.json").size() == 1,
          "program defaults preserve device profiles");
    QSettings retained(settingsPath, QSettings::IniFormat);
    check(retained.value("mouse/boundEndpoints").toStringList() == QStringList{"retain-receiver", "retain-wired"} &&
              !retained.value("saved/groups/audio/input").toBool(),
          "program defaults preserve connection bindings and category expansion");
    check(page->findChild<QLabel*>("settingsSaveStatus")->property("saveSucceeded").toBool(),
          "preference save result is visible");
    check(page->findChild<QPushButton*>("settingsOpenDataDirectory") &&
              page->findChild<QPushButton*>("settingsOpenLog"),
          "local data actions are provided without opening external apps during tests");
    settle();
    check(window.grab().save(output + "/settings-minimum-fixture.png"), "save simulated minimum-size settings page");
    window.resize(1440, 940);
    settle();
    check(window.grab().save(output + "/settings-wide-fixture.png"), "save simulated wide settings page");
    check(fake::writeCount() == writesBefore, "program preference operations perform zero hardware writes");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // offscreen 插件不自动枚举 Windows 字体；仅给验证窗口注册系统字体。
    const int testFont = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    check(testFont >= 0, "offscreen fixture loads actual Windows CJK font");
    app.setFont(QFont("Microsoft YaHei UI", 10));
    app.setQuitOnLastWindowClosed(false);
    const QString output = QDir::currentPath();
    QTemporaryDir directory(output + "/fresh-data-XXXXXX");
    if (!directory.isValid())
        return 2;
    const auto profilePath = directory.path() + "/devices.json";
    check(!QFile::exists(profilePath) && !QFile::exists(directory.path() + "/settings.ini"),
          "fresh fixture has no pre-existing device or selection profile");
    DeviceService service(false); // Real scan flow with private-query eligibility enabled; no periodic timer.
    MainWindow window(&service, directory.path());
    if (const auto* display = window.screen()) {
        const auto available = display->availableGeometry().size();
        const QSize expectedMaximum = available.expandedTo(window.minimumSize());
        check(window.width() <= expectedMaximum.width() && window.height() <= expectedMaximum.height(),
              "initial window is bounded by available work area when it can satisfy the design minimum");
        check(window.minimumSize().width() <= available.width() &&
                  window.minimumSize().height() <= available.height(),
              "design minimum never exceeds the available work area on small or high-DPI screens");
    }
    {
        const QString literal = "模拟设备 <b>PRO & Max</b>";
        PercentDetails panel("模拟音量", {25, 50});
        panel.setDescription(literal);
        const auto descriptions = panel.findChildren<QLabel*>();
        const auto found = std::find_if(descriptions.begin(), descriptions.end(),
                                       [&](const auto* label) { return label->text() == literal; });
        check(found != descriptions.end() && (*found)->textFormat() == Qt::PlainText,
              "device descriptions display literal name text instead of interpreting HTML tags");
        auto* combo = createThemedComboBox();
        combo->addItem(literal);
        combo->resize(300, 40);
        combo->show();
        settle();
        QHelpEvent help(QEvent::ToolTip, QPoint(12, 12), combo->mapToGlobal(QPoint(12, 12)));
        QApplication::sendEvent(combo, &help);
        QTextDocument renderedTooltip;
        renderedTooltip.setHtml(QToolTip::text());
        check(renderedTooltip.toPlainText() == literal,
              "selector tooltip preserves device names containing literal HTML punctuation");
        service.operationFinished(literal, true);
        auto* statusText = window.findChild<QLabel*>("noticeText");
        check(statusText && statusText->text() == literal && statusText->textFormat() == Qt::PlainText,
              "backend operation feedback is displayed literally without HTML interpretation");
        if (statusText) {
            renderedTooltip.setHtml(statusText->toolTip());
            check(renderedTooltip.toPlainText() == literal,
                  "backend feedback tooltip preserves the same literal diagnostic text");
        }
        DeviceCard literalCard("模拟", literal);
        QLabel* cardTitle = nullptr;
        for (auto* candidate : literalCard.findChildren<QLabel*>())
            if (candidate->property("role") == "cardTitle")
                cardTitle = candidate;
        renderedTooltip.setHtml(cardTitle ? cardTitle->toolTip() : QString{});
        check(cardTitle && cardTitle->accessibleName() == literal && renderedTooltip.toPlainText() == literal,
              "card title accessibility and tooltip retain the original literal device name");
        QToolTip::hideText();
        delete combo;
    }
    window.setWindowTitle("动态设备集成检查 · 全部设备均为模拟数据");
    window.statusBar()->showMessage("模拟验证：本窗口所有设备、读数与保存档案均为测试数据。");
    std::map<QString, DeviceCard*> cards;
    for (auto* card : window.findChildren<DeviceCard*>())
        cards[card->cardId()] = card;
    check(cards.size() == 4 && cards.contains("mouse") && cards.contains("keyboard") &&
              cards.contains("monitor") && cards.contains("audio"),
          "exactly one reusable card per category");
    if (cards.size() != 4)
        return 3;
    auto* gridWidget = window.findChild<QWidget*>("deviceGrid");
    auto* grid = static_cast<DeviceGrid*>(gridWidget);
    auto* overview = button(window, "设备总览");
    auto* saved = button(window, "已保存设备");
    auto* savedList = window.findChild<QTreeWidget*>();
    auto* savedDetail = window.findChild<QPlainTextEdit*>();
    check(grid && overview && saved && savedList && savedDetail, "overview and saved-device views exist");
    if (!grid || !overview || !saved || !savedList || !savedDetail)
        return 4;
    int scans = 0;
    QObject::connect(&service, &DeviceService::refreshed, &window, [&] { ++scans; });
    auto scan = [&](const fake::Fixture& fixture) {
        {
            std::lock_guard lock(fake::mutex);
            fake::state = fixture;
        }
        const int before = scans;
        service.refresh(true);
        check(waitUntil([&] { return scans > before; }), "real service emits fresh scan result");
        settle();
    };
    window.show();
    settle();
    {
        // 唤醒处理走同一条公开路径：测试直接调用，避免依赖真实窗口句柄
        // （offscreen 平台没有原生 HWND，SendMessage 到不了窗口）。
        const int before = scans;
        window.handleSystemResume();
        check(waitUntil([&] { return scans > before; }, 5000),
              "system resume forces a fresh device scan");
    }
    const int previewWritesBefore = fake::writeCount();
    for (const auto& limit : {QSize(192, 120), QSize(360, 220), QSize(512, 256)}) {
        const auto compact = window.taskbarPreviewImage(limit);
        check(!compact.isNull() && compact.width() <= limit.width() && compact.height() <= limit.height(),
              "compact taskbar bitmap fits every requested system thumbnail bound");
        check(compact.hasAlphaChannel(), "compact taskbar bitmap keeps a 32-bit alpha channel");
    }
    check(window.taskbarPreviewImage({}).isNull(), "empty taskbar bounds do not allocate a bitmap");
    check(fake::writeCount() == previewWritesBefore, "rendering taskbar summaries does not write device settings");
    {
        FrostedDialog dialog(&window);
        dialog.setWindowTitle("模拟设备详情");
        auto* message = new QLabel("模拟验证：无真实设备写入。", &dialog);
        dialog.contentLayout()->addWidget(message);
        dialog.show();
        settle();
        check(dialog.property("glassDialog").toBool() && dialog.isVisible(),
              "secondary windows use a visible shared frosted dialog surface");
        check(dialog.findChild<QPushButton*>("windowClose") &&
                  !dialog.findChild<QPushButton*>("windowMinimize") &&
                  !dialog.findChild<QPushButton*>("windowMaximize"),
              "modal dialog caption offers close without unrelated minimize or maximize actions");
        const QRect contentBounds(message->mapTo(&dialog, QPoint{}), message->size());
        check(dialog.rect().contains(contentBounds) && contentBounds.top() >= 34,
              "dialog content is below its integrated caption and stays inside the window");
        int rejected = 0;
        QObject::connect(&dialog, &QDialog::rejected, [&] { ++rejected; });
        dialog.findChild<QPushButton*>("windowClose")->click();
        settle();
        check(rejected == 1 && !dialog.isVisible(), "dialog close rejects rather than accepting an action");
    }
    auto* chrome = window.findChild<QWidget*>("windowChrome");
    auto* minimize = window.findChild<QPushButton*>("windowMinimize");
    auto* maximize = window.findChild<QPushButton*>("windowMaximize");
    auto* close = window.findChild<QPushButton*>("windowClose");
    check(chrome && minimize && maximize && close, "integrated title bar exposes all three window actions");
    if (chrome && minimize && maximize && close) {
        check(chrome->height() == 34 && chrome->mapTo(&window, QPoint{}).y() == 0,
              "integrated title bar starts at the top without a separate caption gap");
        check(minimize->width() == 46 && maximize->width() == 46 && close->width() == 46 &&
                  minimize->geometry().right() < maximize->geometry().left() &&
                  maximize->geometry().right() < close->geometry().left(),
              "caption actions have consistent sizes and standard order");
        maximize->click();
        settle();
        check(window.isMaximized(), "maximize caption action maximizes the actual main window");
        maximize->click();
        settle();
        check(!window.isMaximized() && !window.isMinimized(), "restore caption action returns to normal state");
        minimize->click();
        settle();
        check(window.isMinimized(), "minimize caption action minimizes the actual main window");
        window.showNormal();
        settle();
    }
    scan({});
    check(service.snapshot().hidEnumerationSucceeded && service.snapshot().audioEnumerationSucceeded &&
              service.snapshot().monitorEnumerationSucceeded,
          "successful empty enumerations remain distinguishable from failures");
    check(service.snapshot().sampledMs > 0 && service.snapshot().audioSampledMs > 0 &&
              service.snapshot().monitorSampledMs > 0,
          "fixture scan carries actual sampled timestamps for every category");
    check(grid->visibleCardCount() == 0, "empty scan hides every control card");
    for (const auto& [id, card] : cards)
        check(card->isHidden(), "disconnected category card is hidden");
    auto* empty = grid->findChild<QLabel*>("deviceGridEmpty");
    check(empty && empty->isVisible() && empty->text().contains("接入后会自动显示"),
          "empty state is visible");
    check(window.grab().save(output + "/empty-fixture.png"), "save simulated empty overview screenshot");

    fake::Fixture first;
    first.hid = {hid(L"fixture-mouse-A", L"fixture-mouse-A", L"模拟 ORBIT M1", 2),
                 hid(L"fixture-keyboard-A", L"fixture-keyboard-A", L"模拟 NOVA K98", 6)};
    first.audio = {audio(L"fixture-audio-A", L"模拟 ORBIT Headset X")};
    first.monitors = {monitor(L"fixture-monitor-A", L"模拟 NOVA VIEW 27Q", 47)};
    fake::Fixture displayOnly;
    displayOnly.monitors = first.monitors;
    scan(displayOnly);
    check(grid->visibleCardCount() == 1 && cards["monitor"]->isVisible(),
          "single connected monitor reveals only its own class card");
    check(cards["mouse"]->isHidden() && cards["keyboard"]->isHidden() && cards["audio"]->isHidden(),
          "unconnected peripheral classes never show placeholder cards");
    check(cards["monitor"]->height() <= 310, "single connected card keeps compact height");
    check(title(cards["monitor"]) == "模拟 NOVA VIEW 27Q",
          "first-seen single monitor gets actual model title");
    check(waitUntil([&] { return !service.monitorControlsBusy(); }),
          "single monitor capability read completes");
    scan(first);
    check(grid->visibleCardCount() == 4, "first-seen unknown devices reveal their category cards");
    for (const auto& [id, card] : cards)
        check(card->isVisible(), "all four connected category cards appear");
    {
        // 缩到设计下限以下时应停在下限，而不是被内部布局的最小高度反向撑大。
        const auto designMinimum = window.minimumSize();
        window.resize(designMinimum.width() - 120, designMinimum.height() - 80);
        settle();
        check(window.size() == designMinimum,
              "shrinking below the design minimum snaps to the minimum instead of a larger layout size");
        window.resize(1440, 940);
        settle();
    }
    check(title(cards["mouse"]) == "ACME 模拟 ORBIT M1", "unknown mouse uses HID-provided product name");
    check(title(cards["keyboard"]) == "ACME 模拟 NOVA K98",
          "unknown keyboard classified by standard HID usage");
    check(title(cards["monitor"]) == "模拟 NOVA VIEW 27Q",
          "arbitrary new monitor uses enumerated system name");
    auto* audioSelector = cards["audio"]->findChild<QComboBox*>("audioOutputSelector");
    auto* audioInputSelector = cards["audio"]->findChild<QComboBox*>("audioInputSelector");
    check(audioSelector && audioInputSelector && audioSelector != audioInputSelector,
          "output and input use independent clearly named selectors");
    check(audioSelector->count() == 1 && audioSelector->itemText(0).contains("模拟 ORBIT Headset X"),
          "unknown audio endpoint appears under its actual system name");
    check(audioSelector->currentIndex() == 0,
          "single unambiguous audio endpoint is selected without guessing identity");
    auto* singleOutputVolume = cards["audio"]->findChild<QSlider*>("audioOutputVolume");
    check(singleOutputVolume && singleOutputVolume->isEnabled(),
          "single audio endpoint exposes its volume control on the first scan");
    audioSelector->setCurrentIndex(0);
    check(title(cards["audio"]) == "音频设备" && audioSelector->currentText() == "模拟 ORBIT Headset X",
          "audio card groups controls while output selector retains actual endpoint model name");
    check(waitUntil([&] { return !service.monitorControlsBusy() && fake::advancedQueries > 0; }),
          "new arbitrary monitor receives capability query without a model table");
    check(waitUntil([&] { return savedRows(profilePath).size() == 4; }),
          "first-seen devices are persisted automatically");
    check(
        savedRow(profilePath, "monitor:fixture-monitor-A").value("settings").toObject().value("brightness") ==
            47,
        "persisted monitor profile records actual brightness");
    check(savedRow(profilePath, "audio:fixture-audio-A").value("settings").toObject().value("volume") == 20,
          "persisted audio profile records actual volume");
    check(fake::writeCount() == 0, "new device detection, selection and profile creation issue zero writes");
    check(fake::batteryQueries == 0, "unknown HID never receives private battery queries");

    fake::Fixture many = first;
    many.hid.push_back(hid(L"fixture-mouse-B", L"fixture-mouse-B", L"模拟 ORBIT M2", 2));
    many.hid.push_back(hid(L"fixture-keyboard-B", L"fixture-keyboard-B", L"USB Keyboard", 6));
    auto inputEndpoint = audio(L"fixture-audio-B", L"模拟 ORBIT Microphone X", true);
    inputEndpoint.volumePercent = 71;
    inputEndpoint.muted = true;
    many.audio.push_back(inputEndpoint);
    many.monitors.push_back(monitor(L"fixture-monitor-B", L"模拟 HORIZON 32U", 63));
    scan(many);
    check(window.findChildren<DeviceCard*>().size() == 4 && grid->visibleCardCount() == 4,
          "two devices of every category still share four cards");
    for (const auto& [id, card] : cards)
        if (id != "audio")
            check(selector(card)->count() == 2, "same-category selector contains both devices");
    check(audioSelector->count() == 1 && audioInputSelector->count() == 1,
          "audio menus partition output and input endpoints without mixing directions");
    auto* mouseSelector = selector(cards["mouse"]);
    for (int index = 0; index < mouseSelector->count(); ++index)
        if (mouseSelector->itemText(index).contains("M2"))
            mouseSelector->setCurrentIndex(index);
    check(title(cards["mouse"]) == "ACME 模拟 ORBIT M2",
          "second mouse selection updates only its card model");
    auto* keyboardSelector = selector(cards["keyboard"]);
    for (int index = 0; index < keyboardSelector->count(); ++index)
        if (keyboardSelector->itemText(index).contains("型号未提供"))
            keyboardSelector->setCurrentIndex(index);
    check(title(cards["keyboard"]) == "USB 键盘（型号未提供）",
          "generic HID name never invents a product model");
    auto* monitorSelector = selector(cards["monitor"]);
    monitorSelector->setCurrentIndex(monitorSelector->findData("fixture-monitor-B"));
    check(title(cards["monitor"]) == "模拟 HORIZON 32U", "second monitor selection updates its model title");
    audioInputSelector->setCurrentIndex(audioInputSelector->findData("fixture-audio-B"));
    check(audioInputSelector->currentText() == "模拟 ORBIT Microphone X" &&
              audioSelector->findData("fixture-audio-B") == -1 &&
              audioInputSelector->findData("fixture-audio-A") == -1,
          "capture and output endpoints appear only in their own direction selector");
    auto* outputVolume = cards["audio"]->findChild<QSlider*>("audioOutputVolume");
    auto* inputVolume = cards["audio"]->findChild<QSlider*>("audioInputVolume");
    check(outputVolume && inputVolume && outputVolume != inputVolume && outputVolume->isEnabled() &&
              inputVolume->isEnabled() && outputVolume->value() == 20 && inputVolume->value() == 71,
          "output and input sliders independently display their own endpoint values");
    check(cards["audio"]->findChild<QPushButton*>("audioOutputMute")->text() == "静音" &&
              cards["audio"]->findChild<QPushButton*>("audioInputMute")->text() == "取消静音",
          "output and input mute buttons independently display endpoint mute state");
    {
        QSettings disk(directory.path() + "/settings.ini", QSettings::IniFormat);
        disk.sync();
        check(disk.value("audio/outputSelectedId") == "fixture-audio-A" &&
                  disk.value("audio/inputSelectedId") == "fixture-audio-B",
              "output and input selections use independent persisted preference keys");
    }
    std::reverse(many.hid.begin(), many.hid.end());
    std::reverse(many.audio.begin(), many.audio.end());
    std::reverse(many.monitors.begin(), many.monitors.end());
    const auto selectedMouse = mouseSelector->currentData();
    scan(many);
    check(mouseSelector->currentData() == selectedMouse &&
              monitorSelector->currentData() == "fixture-monitor-B" &&
              audioSelector->currentData() == "fixture-audio-A" &&
              audioInputSelector->currentData() == "fixture-audio-B",
          "enumeration order changes retain selected stable targets");
    check(waitUntil([&] { return !service.monitorControlsBusy(); }),
          "second selected monitor capability query completes");
    auto withoutMice = many;
    std::erase_if(withoutMice.hid, [](const auto& device) { return device.usage == 2; });
    cards["mouse"]->setExpanded(true);
    scan(withoutMice);
    check(grid->visibleCardCount() == 3 && cards["mouse"]->isHidden(),
          "removing one entire class hides only that class card");
    check(cards["keyboard"]->isVisible() && cards["monitor"]->isVisible() && cards["audio"]->isVisible(),
          "other connected classes remain visible after mouse removal");
    scan(many);
    check(grid->visibleCardCount() == 4 && cards["mouse"]->isVisible() && cards["mouse"]->isExpanded(),
          "reconnected mouse class reuses its existing expanded card");
    for (const QSize size : {QSize(1210, 830), QSize(1030, 760)}) {
        window.resize(size);
        settle();
        std::map<QString, QRect> geometry;
        for (const auto& [id, card] : cards)
            geometry[id] = QRect(card->mapTo(&window, QPoint{}), card->size());
        for (int mask = 0; mask < 16; ++mask) {
            int index = 0;
            for (const auto& [id, card] : cards)
                card->setExpanded((mask & (1 << index++)) != 0);
            settle();
            for (const auto& [id, card] : cards)
                check(QRect(card->mapTo(&window, QPoint{}), card->size()) == geometry[id],
                      "expansion preserves every other card geometry at default and minimum window size");
        }
    }
    check(window.grab().save(output + "/connected-fixture.png"),
          "save simulated connected overview screenshot");
    for (const auto& [id, card] : cards)
        card->setExpanded(false);
    window.resize(1210, 830);
    settle();
    check(window.grab().save(output + "/quick-fixture.png"),
          "save simulated quick view of split audio controls");
    const int layoutWritesBefore = fake::writeCount();
    cards["monitor"]->setExpanded(true);
    window.resize(1440, 940);
    settle();
    auto* sidebar = window.findChild<QFrame*>("sidebar");
    auto* brightnessControl = window.findChild<QWidget*>("monitorBrightnessControl");
    check(sidebar && sidebar->width() == 230 && sidebar->mapTo(&window, QPoint{}).y() == 0 &&
              sidebar->height() == window.centralWidget()->height(),
          "reference layout sidebar spans the complete glass surface at 230px");
    auto* sidebarToggle = window.findChild<QToolButton*>("sidebarToggle");
    auto* overviewNavigation = window.findChild<QPushButton*>("overviewNavigation");
    check(sidebarToggle && overviewNavigation && sidebar->width() == 230 &&
              overviewNavigation->text() == "设备总览",
          "sidebar starts expanded and exposes a collapse control");
    sidebarToggle->click();
    settle();
    check(sidebar->width() == 64 && sidebarToggle->toolTip() == "展开侧栏" &&
              qAbs(sidebarToggle->geometry().center().x() - sidebar->width() / 2) <= 1 &&
              overviewNavigation->text().isEmpty() && overviewNavigation->toolTip() == "设备总览" &&
              cards["mouse"]->findChild<QLabel*>("deviceCategoryIcon")->width() == 64,
          "sidebar collapses to a centered icon rail without changing card icon scale");
    check(window.grab().save(output + "/sidebar-collapsed.png"), "save collapsed sidebar fixture");
    {
        QSettings disk(directory.path() + "/settings.ini", QSettings::IniFormat);
        disk.sync();
        check(disk.value("ui/sidebarCollapsed").toBool(), "collapsed sidebar state is saved");
    }
    sidebarToggle->click();
    settle();
    {
        QSettings disk(directory.path() + "/settings.ini", QSettings::IniFormat);
        disk.sync();
        check(sidebar->width() == 230 && overviewNavigation->text() == "设备总览" &&
                  !disk.value("ui/sidebarCollapsed").toBool(),
              "sidebar expands and saves the restored state");
    }
    check(cards["mouse"]->findChild<QLabel*>("deviceCategoryIcon")->width() == 64,
          "reference size uses the fixed 64px category icon");
    check(brightnessControl && brightnessControl->isVisible() &&
              cards["monitor"]->detailLayout()->parentWidget()->isAncestorOf(brightnessControl),
          "expanded monitor retains the single live brightness slider inside native controls");
    check(monitorSelector->isVisible(), "multiple monitors remain selectable in expanded reference layout");
    check(window.grab().save(output + "/reference-fixture.png"),
          "save actual reference-sized application layout using explicitly simulated device data");
    window.resize(1030, 760);
    settle();
    auto* overviewScroll = window.findChild<QScrollArea*>("overviewScroll");
    check(sidebar->width() == 230 && overviewScroll &&
              overviewScroll->widget() == window.findChild<QWidget*>("overviewPage") &&
              cards["mouse"]->findChild<QLabel*>("deviceCategoryIcon")->width() == 64,
          "minimum window keeps the same icon scale and scrolls the overview page instead of switching density");
    check(window.grab().save(output + "/reference-minimum-fixture.png"),
          "save minimum layout with page scrolling and expanded native monitor controls");
    for (int count = 0; count < 4; ++count)
        cards["monitor"]->setExpanded(count % 2 == 0);
    cards["monitor"]->setExpanded(true);
    settle();
    check(brightnessControl->isVisible() &&
              window.findChild<DeviceGrid*>()->findChildren<QSlider*>().size() == 3 &&
              fake::writeCount() == layoutWritesBefore,
          "density and repeated monitor page changes preserve one slider per endpoint and never write devices");
    check(waitUntil([&] { return savedRows(profilePath).size() == 8; }),
          "all new devices get separate saved profiles");

    auto failed = many;
    failed.failHid = failed.failAudio = failed.failMonitor = true;
    scan(failed);
    check(!service.snapshot().hidEnumerationSucceeded && !service.snapshot().audioEnumerationSucceeded &&
              !service.snapshot().monitorEnumerationSucceeded,
          "failed enumeration flags survive actual service flow");
    check(grid->visibleCardCount() == 4, "enumeration failure is not treated as physical disconnection");
    saved->click();
    settle();
    check(leaves(savedList).size() == 8, "saved tree contains all known device profiles");
    for (auto* item : leaves(savedList)) {
        check(item->text(0).contains("读取暂不可用") && !item->text(0).contains("未接入"),
              "failed category reports unavailable reading without declaring a disconnect");
        savedList->setCurrentItem(item);
        check(savedDetail->toPlainText().contains("读取暂不可用，保留上次记录"),
              "failed category details preserve previous profile state explicitly");
    }
    overview->click();
    scan(many);
    check(grid->visibleCardCount() == 4, "successful scan after transient failure keeps cards visible");
    check(fake::writeCount() == 0, "recovering enumeration does not falsely trigger reconnect restore");

    scan({});
    check(grid->visibleCardCount() == 0, "successful unplug scan hides every category card");
    check(empty->isVisible(), "empty overview returns after all devices detach");
    for (const auto& [id, card] : cards)
        check(card->isHidden(), "detached card is hidden without deletion");
    saved->click();
    settle();
    check(leaves(savedList).size() == 8, "detached devices remain in the separate saved tree");
    for (auto* item : leaves(savedList)) {
        check(item->text(0).contains("未接入"), "saved profile shows detached state");
        savedList->setCurrentItem(item);
        check(savedDetail->toPlainText().contains("状态：未接入"),
              "saved detail reports disconnection accurately");
    }
    savedList->setCurrentItem(leaf(savedList, "monitor:fixture-monitor-A"));
    check(savedDetail->toPlainText().contains("47%"), "saved column retains last known monitor brightness");
    check(window.grab().save(output + "/saved-fixture.png"), "save simulated saved-profile screenshot");
    check(savedRows(profilePath).size() == 8, "detach preserves persisted device records");
    QFile::remove(output + "/fixture-devices.json");
    check(QFile::copy(profilePath, output + "/fixture-devices.json"), "preserve simulated profile artifact");
    check(fake::writeCount() == 0,
          "all integrated discovery and view operations perform zero hardware writes");
    selectionChecks(output);
    mouseBindingChecks();
    savedScrollChecks(output);
    classificationChecks(output);
    savedInitialCollapseChecks(output);
    manualRefreshChecks(output);
    profileDeletionChecks(output);
    longNameChecks(output);
    wheelMainWindowChecks(output);
    wheelGuardChecks();
    applicationSettingsChecks(output);
    trayMenuChecks(output);
    lowBatteryNotificationChecks(output);
    {
        std::lock_guard lock(fake::mutex);
        for (const auto& write : fake::writes) {
            check(write.code != 0x60, "automatic flow never switches input source");
            std::cout << "FAKE_WRITE target=" << QString::fromStdWString(write.target).toStdString()
                      << " kind=" << write.kind << " code=" << write.code << " value=" << write.value << '\n';
        }
    }
    std::cout << "CHECKS=" << checks << " FAILURES=" << failures << " FAKE_WRITES=" << fake::writeCount()
              << " FAKE_ENUMERATIONS=" << fake::enumerations
              << " FAKE_ADVANCED_QUERIES=" << fake::advancedQueries
              << " FAKE_PRIVATE_HID_QUERIES=" << fake::batteryQueries << '\n';
    std::cout
        << "Only fake WindowsBackend symbols linked. All models, devices and screenshots are simulated.\n";
    return failures == 0 ? 0 : 1;
}
