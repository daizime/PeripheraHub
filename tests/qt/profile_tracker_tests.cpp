#include "app/DeviceProfiles.h"
#include "app/DeviceService.h"
#include "app/ProfileTracker.h"
#include "../fixtures/vgn_f2_capture.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

namespace fake {
struct Restore {
    QString profile;
    QString target;
    bool monitor = false;
    QMap<QString, int> values;
};
QList<Restore> restores;
QStringList selections;
int queries = 0;
bool brightnessPending = false;
bool volumePending = false;
} // namespace fake

// 测试只实现这些服务入口，不编译 DeviceService.cpp 或原生后端，无法写入硬件。
namespace peripheral::app {
struct DeviceService::LiveControl {};
DeviceService::DeviceService(bool, QObject* parent) : QObject(parent) {}
DeviceService::~DeviceService() = default;
bool DeviceService::brightnessUpdatePending(const QString&) const {
    return fake::brightnessPending;
}
bool DeviceService::volumeUpdatePending(const QString&) const {
    return fake::volumePending;
}
void DeviceService::restoreSettings(const QString& profile, const QString& target, bool monitor,
                                    const QMap<QString, int>& values) {
    fake::restores.append({profile, target, monitor, values});
}
void DeviceService::selectMonitorControlsTarget(const QString& id) {
    fake::selections.append(id);
    monitorControls_ = {};
    monitorControls_.id = id.toStdWString();
    monitorControlsReadAttempted_ = false;
    monitorControlsBusy_ = false;
    emit monitorControlsChanged();
}
void DeviceService::requestMonitorControls(const QString&) {
    ++fake::queries;
    monitorControlsBusy_ = true;
    monitorControlsReadAttempted_ = true;
    emit monitorControlsChanged();
}
void DeviceService::setMonitorFeature(const QString&, int, int) {
    // 仅用于完成伪造的查询，测试不会调用任何硬件函数。
    monitorControlsBusy_ = false;
    monitorControlsReadAttempted_ = true;
    emit monitorControlsChanged();
}
} // namespace peripheral::app

using namespace peripheral;
using namespace peripheral::app;

namespace {
int checks = 0;
int failures = 0;
qint64 clockMs = 0;

void check(bool passed, const char* description) {
    ++checks;
    if (!passed) {
        ++failures;
        std::cerr << "FAILED: " << description << '\n';
    }
}

qint64 monotonicNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

qint64 tick() {
    clockMs = std::max(clockMs + 2, monotonicNowMs() + 2);
    return clockMs;
}

RuntimeSnapshot& snapshot(DeviceService& service) {
    return const_cast<RuntimeSnapshot&>(service.snapshot());
}

void fresh(RuntimeSnapshot& state) {
    const auto now = tick();
    state.sampledMs = state.audioSampledMs = state.monitorSampledMs = now;
    state.sampledMonotonicMs = state.audioSampledMonotonicMs = state.monitorSampledMonotonicMs = now;
}

platform::MonitorInfo monitor(int brightness = 47) {
    platform::MonitorInfo result;
    result.id = L"native/monitor-A";
    result.name = L"Real model from system";
    result.brightnessReadable = true;
    result.brightnessPercent = brightness;
    return result;
}

platform::AudioEndpointInfo audio(float volume = 20) {
    platform::AudioEndpointInfo result;
    result.id = L"native/audio-A";
    result.name = L"Playback endpoint";
    result.volumeReadable = true;
    result.volumePercent = volume;
    return result;
}

DeviceSnapshot hid(const std::string& serial = "serial-A") {
    DeviceSnapshot result;
    result.kind = DeviceKind::Mouse;
    result.logicalId = "same-container";
    result.name = "HID product";
    Endpoint endpoint;
    endpoint.endpointId = "path";
    endpoint.kind = DeviceKind::Mouse;
    endpoint.vid = 0x3554;
    endpoint.serial = serial;
    result.endpoints.push_back(endpoint);
    return result;
}

void completeControls(DeviceService& service, ProfileTracker& tracker, int scene, qint64 sampled,
                      bool validScene = true) {
    auto& info = const_cast<platform::MonitorControlsInfo&>(service.monitorControls());
    info = {};
    info.id = L"native/monitor-A";
    info.sampledMs = sampled;
    info.sampledMonotonicMs = sampled;
    platform::MonitorFeatureInfo feature;
    feature.code = 0xDC;
    feature.readable = true;
    feature.current = scene;
    feature.allowedValues =
        validScene ? std::vector<std::uint16_t>{0, 3, 5} : std::vector<std::uint16_t>{0, 3};
    info.features.push_back(feature);
    feature.code = 0x60;
    feature.current = 15;
    feature.allowedValues = {15, 17};
    info.features.push_back(feature);
    feature.code = 0x14;
    feature.current = 7;
    feature.allowedValues = {3, 5}; // 当前值不在声明列表，不能保存为确认值。
    info.features.push_back(feature);
    feature.code = 0x12;
    feature.current = 70;
    feature.maximum = 100;
    feature.allowedValues.clear();
    info.features.push_back(feature);
    service.setMonitorFeature({}, 0, 0);
    tracker.updateMonitorControls();
}

void waitFor(int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QCoreApplication::processEvents();
}

void initialDiscovery(const QString& path) {
    fake::restores.clear();
    DeviceProfiles profiles(path);
    check(profiles.load(), "fresh store loads");
    DeviceService service(false);
    ProfileTracker tracker(&service, &profiles);
    auto& state = snapshot(service);
    state.hidEnumerationSucceeded = state.audioEnumerationSucceeded = state.monitorEnumerationSucceeded =
        true;
    state.monitors = {monitor()};
    state.audio = {audio()};
    auto capture = audio();
    capture.id = L"native/audio-capture";
    capture.capture = true;
    state.audio.push_back(capture);
    fresh(state);
    auto mouse = hid();
    mouse.stale = false;
    mouse.battery.reportedPercent = 0;
    mouse.battery.charging = false;
    auto keyboard = mouse;
    keyboard.kind = DeviceKind::Keyboard;
    keyboard.logicalId = "keyboard";
    keyboard.battery = {};
    auto historical = mouse;
    historical.logicalId = "old-cache";
    historical.endpoints.clear();
    tracker.update({mouse, keyboard, historical});
    check(fake::restores.isEmpty(), "first discovery never writes hardware settings");
    check(profiles.profiles().size() == 5, "only current endpoints create profiles");
    check(profiles.find("monitor:native/monitor-A")->settings.value("brightness") == 47,
          "new monitor seeds valid brightness");
    check(profiles.find("audio:native/audio-A")->settings.value("volume") == 20,
          "new audio seeds valid volume");
    check(profiles.find("audio:native/audio-A")->direction == QStringLiteral("output") &&
              profiles.find("audio:native/audio-capture")->direction == QStringLiteral("input"),
          "audio capture flag determines direction even when input and output share the same name");
    check(profiles.find("monitor:native/monitor-A")->confirmedSettings.contains("brightness"),
          "initial actual read is confirmed");
    check(profiles.find(ProfileTracker::hidProfileId(mouse))->connected, "HID endpoint is marked connected");
    check(profiles.find(ProfileTracker::hidProfileId(mouse))->settings.value("battery", -1) == 0 &&
              profiles.find(ProfileTracker::hidProfileId(mouse))->settings.value("charging", -1) == 0,
          "fresh zero battery and explicit not-charging remain real values");
    check(!profiles.find(ProfileTracker::hidProfileId(keyboard))->settings.contains("battery") &&
              !profiles.find(ProfileTracker::hidProfileId(keyboard))->settings.contains("charging"),
          "unknown HID battery and charging stay absent");
    check(!profiles.find("hid:old-cache"), "historical battery-only device does not reconnect");
    auto other = mouse;
    other.endpoints[0].serial = "serial-B";
    check(ProfileTracker::hidProfileId(mouse) != ProfileTracker::hidProfileId(other),
          "same logical ID with different serial cannot share saved settings");
    other = mouse;
    other.endpoints[0].vid = 0x1234;
    check(ProfileTracker::hidProfileId(mouse) != ProfileTracker::hidProfileId(other),
          "same serial with different VID remains distinct");
    other = mouse;
    other.endpoints.push_back(other.endpoints[0]);
    check(ProfileTracker::hidProfileId(mouse) == ProfileTracker::hidProfileId(other),
          "duplicate HID collections do not change identity fingerprint");
    state.monitors[0].brightnessReadable = false;
    state.audio[0].volumeReadable = false;
    mouse.stale = true;
    mouse.battery.reportedPercent = 100;
    mouse.battery.charging = true;
    fresh(state);
    tracker.update({mouse, keyboard});
    check(profiles.find("monitor:native/monitor-A")->settings.value("brightness") == 47,
          "unreadable brightness never replaces known value with zero");
    check(profiles.find("audio:native/audio-A")->settings.value("volume") == 20,
          "unreadable volume never replaces known value with zero");
    check(profiles.find(ProfileTracker::hidProfileId(mouse))->settings.value("battery", -1) == 0 &&
              profiles.find(ProfileTracker::hidProfileId(mouse))->settings.value("charging", -1) == 0,
          "stale HID reading never replaces last confirmed battery and charging");
    check(QFile::exists(path), "new connection is saved immediately");
    tracker.remember("audio:native/audio-A", "volume", 33);
    DeviceProfiles beforeDebounce(path);
    check(beforeDebounce.load() &&
              beforeDebounce.find("audio:native/audio-A")->settings.value("volume") == 20,
          "ordinary user edit remains deferred before debounce interval");
    waitFor(650);
    DeviceProfiles afterDebounce(path);
    check(afterDebounce.load() && afterDebounce.find("audio:native/audio-A")->settings.value("volume") == 33,
          "debounced save persists user intent without manual action");
    tracker.remember("audio:native/audio-A", "volume", 44);
    tracker.flush();
    DeviceProfiles afterFlush(path);
    check(afterFlush.load() && afterFlush.find("audio:native/audio-A")->settings.value("volume") == 44,
          "flush saves user intent synchronously without waiting for timer");
    tracker.remember("audio:native/audio-A", "volume", 45);
    state.audio.clear();
    tracker.update({mouse, keyboard});
    DeviceProfiles afterDisconnect(path);
    check(afterDisconnect.load() &&
              afterDisconnect.find("audio:native/audio-A")->settings.value("volume") == 45,
          "disconnect immediately persists last target");
    check(afterDisconnect.find("audio:native/audio-A")->name == "Playback endpoint",
          "disconnect preserves last reported device name");
}

void reconnectAndFreshness(const QString& path) {
    fake::restores.clear();
    fake::selections.clear();
    fake::queries = 0;
    DeviceProfiles profiles(path);
    profiles.observe("monitor:native/monitor-A", "Old monitor name", "monitor");
    profiles.remember("monitor:native/monitor-A", "brightness", 55, true);
    profiles.remember("monitor:native/monitor-A", "vcp_dc", 3, true);
    profiles.remember("monitor:native/monitor-A", "vcp_60", 15, true);
    profiles.remember("monitor:native/monitor-A", "vcp_12", 20, true);
    profiles.remember("monitor:native/monitor-A", "vcp_14", 5, true);
    profiles.observe("audio:native/audio-A", "Old audio name", "audio");
    profiles.remember("audio:native/audio-A", "volume", 65, true);
    profiles.remember("audio:native/audio-A", "mute", 1, true);
    DeviceService service(false);
    ProfileTracker tracker(&service, &profiles);
    int statusSignals = 0;
    QObject::connect(&tracker, &ProfileTracker::statusMessage, [&statusSignals] { ++statusSignals; });
    auto& state = snapshot(service);
    state.hidEnumerationSucceeded = state.audioEnumerationSucceeded = state.monitorEnumerationSucceeded =
        true;
    state.monitors = {monitor(10)};
    state.audio = {audio(20)};
    const auto cachedAt = monotonicNowMs() - 1000;
    state.sampledMs = state.audioSampledMs = state.monitorSampledMs = cachedAt;
    state.sampledMonotonicMs = state.audioSampledMonotonicMs = state.monitorSampledMonotonicMs = cachedAt;
    service.selectMonitorControlsTarget("native/monitor-A");
    tracker.update({});
    check(fake::restores.size() == 2, "existing profiles restore once per new connection session");
    check(profiles.find("audio:native/audio-A")->direction == QStringLiteral("output"),
          "legacy audio profile learns reliable direction when reconnected");
    for (const auto& request : fake::restores) {
        if (request.monitor) {
            check(request.profile == "monitor:native/monitor-A" && request.target == "native/monitor-A",
                  "monitor profile namespace does not leak into native target ID");
            check(request.values.size() == 3 && request.values.value("brightness") == 55 &&
                      request.values.value("vcp_dc") == 3,
                  "only confirmed brightness and native scene are eligible for restore");
            check(request.values.value("last_input", -1) == 15 && !request.values.contains("vcp_60"),
                  "confirmed last input is read-only matching context, never a writable input target");
            check(!request.values.contains("vcp_60") && !request.values.contains("vcp_12") &&
                      !request.values.contains("vcp_14"),
                  "input, contrast and color are never automatically restored");
        } else {
            check(request.values.size() == 1 && request.values.value("volume") == 65,
                  "audio restores volume while excluding mute");
        }
    }
    check(profiles.find("monitor:native/monitor-A")->settings.value("brightness") == 55,
          "reconnect defaults never overwrite saved monitor target");
    check(profiles.find("audio:native/audio-A")->settings.value("volume") == 65,
          "reconnect defaults never overwrite saved audio target");
    tracker.update({});
    completeControls(service, tracker, 0, state.sampledMonotonicMs);
    check(fake::restores.size() == 2, "repeated scans do not duplicate a pending restore");
    check(profiles.find("monitor:native/monitor-A")->settings.value("vcp_dc") == 3,
          "advanced old defaults cannot overwrite while restore is pending");
    service.settingsRestored("monitor:native/monitor-A", true, "monitor restored");
    check(fake::selections.size() >= 3 && fake::selections[fake::selections.size() - 2].isEmpty() &&
              fake::selections.back() == "native/monitor-A",
          "restore completion clears and reselects current advanced target");
    check(fake::queries == 1, "restore completion requests a fresh advanced query");
    check(tracker.messageFor("monitor:native/monitor-A") == "monitor restored", "restore result is retained");
    state.sampledMs = tick(); // 仅全局时间较新，类别缓存仍旧。
    state.sampledMonotonicMs = state.sampledMs;
    tracker.update({});
    check(profiles.find("monitor:native/monitor-A")->settings.value("brightness") == 55,
          "new global timestamp cannot make old monitor category cache fresh");
    completeControls(service, tracker, 0, state.monitorSampledMonotonicMs);
    check(profiles.find("monitor:native/monitor-A")->settings.value("vcp_dc") == 3,
          "restore completion still rejects a late stale advanced query");
    completeControls(service, tracker, 3, tick());
    check(profiles.find("monitor:native/monitor-A")->settings.value("vcp_dc") == 3 &&
              profiles.find("monitor:native/monitor-A")->confirmedSettings.contains("vcp_dc"),
          "fresh actual scene can confirm saved scene");
    check(profiles.find("monitor:native/monitor-A")->settings.value("vcp_12") == 70,
          "actual supported contrast is recorded");
    check(profiles.find("monitor:native/monitor-A")->settings.value("vcp_14") == 5,
          "color outside declared enumeration cannot be accepted");
    state.monitors[0].brightnessPercent = 55;
    state.monitorSampledMs = tick();
    state.monitorSampledMonotonicMs = state.monitorSampledMs;
    tracker.update({});
    service.settingsRestored("audio:native/audio-A", false, "audio restore failed");
    check(statusSignals == 2, "restore success and failure are non-modal status signals");
    check(tracker.messageFor("audio:native/audio-A") == "audio restore failed",
          "restore failure is retained");
    state.sampledMs = tick();
    state.sampledMonotonicMs = state.sampledMs;
    tracker.update({});
    check(profiles.find("audio:native/audio-A")->settings.value("volume") == 65,
          "stale audio sample after restore failure cannot overwrite saved target");
    state.audio[0].volumePercent = 30;
    state.audioSampledMs = tick();
    state.audioSampledMonotonicMs = state.audioSampledMs;
    tracker.update({});
    check(profiles.find("audio:native/audio-A")->settings.value("volume") == 30,
          "fresh actual externally changed volume becomes last used value");

    tracker.remember("monitor:native/monitor-A", "brightness", 70);
    check(!profiles.find("monitor:native/monitor-A")->confirmedSettings.contains("brightness"),
          "user request is stored as unconfirmed");
    state.monitors[0].brightnessPercent = 55;
    fresh(state);
    fake::brightnessPending = true;
    tracker.update({});
    check(profiles.find("monitor:native/monitor-A")->settings.value("brightness") == 70,
          "pending live brightness write shields user target from old actual read");
    fake::brightnessPending = false;
    state.monitorSampledMs = 1;
    state.monitorSampledMonotonicMs = 1;
    tracker.update({});
    check(profiles.find("monitor:native/monitor-A")->settings.value("brightness") == 70,
          "old category sample stays rejected after pending flag clears");
    state.monitors[0].brightnessPercent = 70;
    state.monitorSampledMs = tick();
    state.monitorSampledMonotonicMs = state.monitorSampledMs;
    tracker.update({});
    check(profiles.find("monitor:native/monitor-A")->confirmedSettings.contains("brightness"),
          "new actual brightness readback confirms user setting");
    tracker.remember("monitor:native/monitor-A", "vcp_dc", 5);
    completeControls(service, tracker, 3, 1);
    check(profiles.find("monitor:native/monitor-A")->settings.value("vcp_dc") == 5 &&
              !profiles.find("monitor:native/monitor-A")->confirmedSettings.contains("vcp_dc"),
          "stale advanced cache cannot erase new requested scene");
    completeControls(service, tracker, 5, tick(), false);
    check(!profiles.find("monitor:native/monitor-A")->confirmedSettings.contains("vcp_dc"),
          "scene outside declared allowed values cannot become confirmed");
    completeControls(service, tracker, 5, tick());
    check(profiles.find("monitor:native/monitor-A")->confirmedSettings.contains("vcp_dc"),
          "fresh valid scene readback confirms requested scene");
    const int beforeFailure = fake::restores.size();
    const auto lastMonitorSeen = profiles.find("monitor:native/monitor-A")->lastSeen;
    waitFor(4);
    state.audio.clear();
    state.monitors.clear();
    state.audioEnumerationSucceeded = state.monitorEnumerationSucceeded = false;
    fresh(state);
    tracker.update({});
    check(profiles.find("monitor:native/monitor-A")->connected &&
              profiles.find("audio:native/audio-A")->connected,
          "enumeration failure does not prove prior connected devices were unplugged");
    check(profiles.find("monitor:native/monitor-A")->lastSeen == lastMonitorSeen,
          "failed enumeration preserves last actual discovery time");
    state.monitors = {monitor(70)};
    state.audio = {audio(30)};
    state.audioEnumerationSucceeded = state.monitorEnumerationSucceeded = true;
    fresh(state);
    tracker.update({});
    check(fake::restores.size() == beforeFailure, "recovery from failed enumeration does not restore twice");
    state.monitors.clear();
    fresh(state);
    tracker.update({});
    check(!profiles.find("monitor:native/monitor-A")->connected,
          "successful empty scan marks device disconnected");
    state.monitors = {monitor(10)};
    fresh(state);
    tracker.update({});
    check(fake::restores.size() == beforeFailure + 1, "real reconnect starts exactly one new restore");
    check(fake::restores.back().values.value("brightness") == 70 &&
              fake::restores.back().values.value("vcp_dc") == 5,
          "reconnect restores latest confirmed settings for that device only");
    check(!fake::restores.back().values.contains("vcp_60"),
          "recorded input is still excluded on later reconnect");
    tracker.remember("monitor:native/monitor-A", "brightness", 88);
    service.settingsRestored("monitor:native/monitor-A", true, "done");
    state.monitors.clear();
    fresh(state);
    tracker.update({});
    const int beforeUnconfirmed = fake::restores.size();
    state.monitors = {monitor(10)};
    fresh(state);
    tracker.update({});
    check(fake::restores.size() == beforeUnconfirmed + 1 &&
              !fake::restores.back().values.contains("brightness"),
          "unconfirmed brightness is not automatically restored");
    waitFor(650);
    DeviceProfiles saved(path);
    check(saved.load() && saved.find("monitor:native/monitor-A").has_value(),
          "tracked settings persist through debounce");
}

void inputContextOnly(const QString& path) {
    fake::restores.clear();
    DeviceProfiles profiles(path);
    profiles.observe("monitor:native/monitor-A", "Input context only", "monitor");
    profiles.remember("monitor:native/monitor-A", "vcp_60", 15, true);
    DeviceService service(false);
    ProfileTracker tracker(&service, &profiles);
    auto& state = snapshot(service);
    state.hidEnumerationSucceeded = state.audioEnumerationSucceeded = state.monitorEnumerationSucceeded =
        true;
    auto device = monitor();
    device.brightnessReadable = false;
    state.monitors = {device};
    fresh(state);
    tracker.update({});
    check(fake::restores.isEmpty(), "last input context alone never starts a restore");
}
} // namespace

void restorePreference(const QString& path) {
    fake::restores.clear();
    DeviceProfiles profiles(path);
    profiles.observe("monitor:native/monitor-A", "Saved monitor", "monitor", {{"brightness", 70}});
    DeviceService service(false);
    ProfileTracker tracker(&service, &profiles);
    tracker.setRestoreOnReconnect(false);
    auto& state = snapshot(service);
    state.hidEnumerationSucceeded = state.audioEnumerationSucceeded = state.monitorEnumerationSucceeded = true;
    state.monitors = {monitor(33)};
    fresh(state);
    tracker.update({});
    check(fake::restores.isEmpty(), "disabled restore reads current connected device without writing old target");
    check(profiles.find("monitor:native/monitor-A")->settings.value("brightness") == 33,
          "disabled restore records current actual value");
    tracker.setRestoreOnReconnect(true);
    tracker.update({});
    check(fake::restores.isEmpty(), "enabling restore does not affect existing connection session");
    state.monitors.clear();
    fresh(state);
    tracker.update({});
    state.monitors = {monitor(10)};
    fresh(state);
    tracker.update({});
    check(fake::restores.size() == 1 && fake::restores.back().values.value("brightness") == 33,
          "next reconnect restores most recent confirmed settings when enabled");
}

void headsetReadings(const QString& path) {
    fake::restores.clear();
    DeviceProfiles profiles(path);
    DeviceService service(false);
    ProfileTracker tracker(&service, &profiles);
    tracker.setRestoreOnReconnect(false);
    auto& state = snapshot(service);
    state.hidEnumerationSucceeded = state.audioEnumerationSucceeded = state.monitorEnumerationSucceeded = true;
    state.audio = {audio()};
    state.audio[0].containerId = L"physical-headset";
    RuntimeSnapshot::HeadsetReading reading;
    reading.containerId = L"physical-headset";
    reading.value.status = n9::Status{};
    reading.value.status->batteryPercent = 80;
    reading.value.peq[0] = n9::PeqPoint{64,-20,9,1,false};
    state.headsets[L"control-path"] = reading;
    check(!state.headsetForContainer({}) && !state.headsetForContainer(L"other"),
          "unrelated or unidentified audio cannot inherit headset readings");
    fresh(state);
    tracker.update({});
    tracker.flush();
    const auto id = QStringLiteral("audio:native/audio-A");
    check(profiles.find(id)->settings.value("battery") == 80, "headset battery joins only by container");
    check(profiles.find(id)->settings.value("n9_peq_0_gain10") == -20, "captured negative PEQ gain is retained");
    DeviceProfiles reloaded(path);
    check(reloaded.load(), "headset archive reloads with signed gains");
    check(reloaded.find(id)->settings.value("n9_peq_0_gain10") == -20, "negative gain survives disk round trip");
    state.headsets[L"ambiguous-second-path"] = reading;
    check(!state.headsetForContainer(L"physical-headset"), "ambiguous HID controls are not attached to audio");
    check(fake::restores.isEmpty(), "private EQ readback never writes or restores hardware");
}

void mouseReadings(const QString& path) {
    fake::restores.clear();
    DeviceProfiles profiles(path);
    DeviceService service(false);
    ProfileTracker tracker(&service, &profiles);
    auto& state = snapshot(service);
    state.hidEnumerationSucceeded = state.audioEnumerationSucceeded = state.monitorEnumerationSucceeded = true;
    auto mouse = hid();
    mouse.stale = false;
    mouse.battery.reportedPercent = 25;
    mouse.battery.charging = false;
    fresh(state);
    RuntimeSnapshot::BatteryReading reading;
    reading.value.profile = fixture::f2Capture().decode();
    reading.profileSampledMs = reading.sampledMs = state.sampledMs;
    reading.profileSampledMonotonicMs = reading.sampledMonotonicMs = state.sampledMonotonicMs;
    state.batteries[L"path"] = reading;
    tracker.update({mouse});
    tracker.flush();
    const auto id = ProfileTracker::hidProfileId(mouse);
    check(profiles.find(id)->settings.value("vgn_dpi_5_x") == 42000, "high DPI extension persists");
    check(profiles.find(id)->settings.value("vgn_active_stage") == 3, "zero based stage persists without reinterpretation");
    check(profiles.find(id)->settings.value("vgn_key_5_param") == 0x100, "button parameter survives profile storage");
    DeviceProfiles reload(path);
    check(reload.load() && reload.find(id)->settings.value("vgn_dpi_5_y") == 42000,
          "device archive roundtrips captured X/Y stages and raw buttons");
    state.batteries[L"path"].value.profileError = "timeout";
    state.batteries[L"path"].value.profile->activeStage = 1;
    fresh(state);
    tracker.update({mouse});
    check(profiles.find(id)->settings.value("vgn_active_stage") == 3, "failed reads cannot replace last confirmed configuration");
    state.batteries[L"path"].value.profileError.clear();
    state.batteries[L"path"].profileSampledMs = state.sampledMs - 45001;
    state.batteries[L"path"].profileSampledMonotonicMs = state.sampledMonotonicMs - 45001;
    tracker.update({mouse});
    check(profiles.find(id)->settings.value("vgn_active_stage") == 3, "expired profile is not saved as new readback");
    auto other = hid("serial-B");
    other.endpoints[0].endpointId = "other-path";
    check(!state.mouseForDevice(other), "matching model or name cannot join another mouse's configuration");
    tracker.update({mouse,other});
    check(!profiles.find(ProfileTracker::hidProfileId(other))->settings.contains("vgn_rate_hz"),
          "new mouse does not inherit previous mouse settings");
    check(fake::restores.isEmpty(), "configuration reads do not authorize mouse writes or restores");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary(QCoreApplication::applicationDirPath() + QStringLiteral("/tracker-XXXXXX"));
    check(temporary.isValid(), "test directory is available");
    initialDiscovery(temporary.filePath("new.json"));
    reconnectAndFreshness(temporary.filePath("existing.json"));
    inputContextOnly(temporary.filePath("input-only.json"));
    restorePreference(temporary.filePath("restore-preference.json"));
    headsetReadings(temporary.filePath("headset.json"));
    mouseReadings(temporary.filePath("mouse.json"));
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
