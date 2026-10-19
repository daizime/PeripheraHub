#include "platform/WindowsBackend.h"
#include "core/device_features.h"
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>
#include <QMap>
#include <QSet>
#include <functional>
#include <map>
#include <memory>
#include <vector>
// 只在夹具中模拟缓存采样时间，避免真实等待五分钟；产品没有测试时钟或环境开关。
#define private public
#include "app/DeviceService.h"
#undef private
#include "app/SettingsRestore.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fake {
using namespace std::chrono_literals;
struct Gate {
    bool armed = false;
    bool entered = false;
    bool open = false;
};
struct Write {
    std::wstring id;
    int value;
};
std::mutex mutex;
std::condition_variable condition;
Gate writeGate, audioGate, hidGate, monitorGate, advancedGate;
std::map<std::wstring, int> volumes{{L"out-A", 10}, {L"in-A", 20}, {L"in-B", 30}};
std::vector<Write> writes;
std::vector<std::string> events;
int monitorReads = 0;
bool failMonitor = false;
bool failAdvanced = false;
int freshWrites = 0;
int cachedWrites = 0;
int mismatchedTargetReads = 0;
int advancedReads = 0;
int targetReads = 0;
int featureWrites = 0;
int activeDdc = 0;
int maxDdc = 0;
std::map<std::uint8_t, int> features{{0x12, 50}, {0x14, 3}, {0x60, 15}, {0xDC, 0}};
struct DdcCall {
    DdcCall() { maxDdc = std::max(maxDdc, ++activeDdc); }
    ~DdcCall() { --activeDdc; }
};

void arm(Gate& gate) {
    std::lock_guard lock(mutex);
    gate = {true, false, false};
}
void release(Gate& gate) {
    std::lock_guard lock(mutex);
    gate.open = true;
    condition.notify_all();
}
bool entered(Gate& gate) {
    std::lock_guard lock(mutex);
    return gate.entered;
}
void wait(Gate& gate, std::unique_lock<std::mutex>& lock) {
    if (!gate.armed)
        return;
    gate.armed = false;
    gate.entered = true;
    condition.notify_all();
    if (!condition.wait_for(lock, 5s, [&gate] { return gate.open; }))
        throw std::runtime_error("Fake gate timed out");
}
int volume(const std::wstring& id) {
    std::lock_guard lock(mutex);
    return volumes.at(id);
}
int countWrites(const std::wstring& id, int value) {
    std::lock_guard lock(mutex);
    return static_cast<int>(std::count_if(writes.begin(), writes.end(), [&](const auto& write) {
        return write.id == id && write.value == value;
    }));
}
int monitorCount() {
    std::lock_guard lock(mutex);
    return monitorReads;
}
// 实验性写入替身：只计数，永远报告"回读不一致"，因为测试不模拟设备落盘。
std::vector<peripheral::platform::HidDeviceInfo> hidDevices;
int privateWrites = 0;
int privateWriteCount() {
    std::lock_guard lock(mutex);
    return privateWrites;
}
void notePrivateWrite() {
    std::lock_guard lock(mutex);
    ++privateWrites;
}
} // namespace fake

// 每个原生后端入口都是替身；此可执行文件不能调用真实 HID、音频或 DDC 接口。
namespace peripheral::platform {
std::vector<HidDeviceInfo> WindowsBackend::enumerateHidDevices() const {
    std::unique_lock lock(fake::mutex);
    fake::events.push_back("hid");
    fake::wait(fake::hidGate, lock);
    return fake::hidDevices;
}
std::vector<AudioEndpointInfo> WindowsBackend::enumerateAudioEndpoints() const {
    std::unique_lock lock(fake::mutex);
    fake::events.push_back("audio");
    fake::wait(fake::audioGate, lock);
    std::vector<AudioEndpointInfo> result;
    for (const auto& [id, volume] : fake::volumes) {
        AudioEndpointInfo audio;
        audio.id = id;
        audio.name = id;
        audio.capture = id != L"out-A";
        audio.volumeReadable = true;
        audio.volumePercent = static_cast<float>(volume);
        result.push_back(audio);
    }
    return result;
}
std::vector<MonitorInfo> WindowsBackend::enumerateMonitors() const {
    std::unique_lock lock(fake::mutex);
    fake::DdcCall call;
    ++fake::monitorReads;
    fake::wait(fake::monitorGate, lock);
    if (fake::failMonitor)
        throw std::runtime_error("Fake monitor unavailable");
    return {};
}
void WindowsBackend::setAudioVolume(const std::wstring& id, float value) const {
    std::unique_lock lock(fake::mutex);
    fake::wait(fake::writeGate, lock);
    fake::writes.push_back({id, static_cast<int>(value)});
    fake::events.push_back("audio-write");
    fake::volumes.at(id) = static_cast<int>(value);
}
void WindowsBackend::setAudioMute(const std::wstring&, bool) const {}
void WindowsBackend::setMonitorBrightness(const std::wstring&, int) const {}
void WindowsBackend::setMonitorFeature(const std::wstring&, std::uint8_t code, std::uint16_t value) const {
    std::lock_guard lock(fake::mutex);
    fake::DdcCall call;
    if (!fake::features.contains(code))
        throw std::runtime_error("Unsupported fake VCP feature");
    ++fake::featureWrites;
    fake::events.push_back("feature-set");
    fake::features[code] = value;
}
MonitorWriteTimings WindowsBackend::setMonitorFeatureTimed(const std::wstring& id, std::uint8_t code,
                                                           std::uint16_t value) const {
    {
        std::lock_guard lock(fake::mutex);
        ++fake::freshWrites;
    }
    setMonitorFeature(id, code, value);
    return {11, 7}; // 明确的模拟计时，不是硬件耗时。
}
MonitorWriteTimings WindowsBackend::setMonitorFeatureFromCapabilities(
    const std::wstring& id, std::uint8_t code, std::uint16_t value, const MonitorControlsInfo& declaration) const {
    if (declaration.id != id || declaration.capabilities.empty())
        throw std::runtime_error("Fake cached declaration has wrong identity or no capabilities");
    {
        std::lock_guard lock(fake::mutex);
        ++fake::cachedWrites;
    }
    setMonitorFeature(id, code, value);
    return {3, 7}; // 模拟快路径，原生 API 次数由独立 WinAPI 夹具验证。
}
MonitorFeatureInfo WindowsBackend::queryMonitorFeature(const std::wstring&, std::uint8_t code) const {
    std::lock_guard lock(fake::mutex);
    fake::DdcCall call;
    ++fake::targetReads;
    fake::events.push_back("target-read");
    MonitorFeatureInfo result;
    result.code = code;
    result.readable = true;
    result.current = fake::features.at(code);
    if (fake::mismatchedTargetReads > 0) {
        --fake::mismatchedTargetReads;
        ++result.current;
    }
    return result;
}
MonitorControlsInfo WindowsBackend::queryMonitorControls(const std::wstring& id) const {
    std::unique_lock lock(fake::mutex);
    fake::DdcCall call;
    ++fake::advancedReads;
    fake::events.push_back("full-controls");
    fake::wait(fake::advancedGate, lock);
    MonitorControlsInfo result;
    result.id = id;
    if (fake::failAdvanced) {
        result.capabilitiesError = "Fake capabilities read failed";
        return result;
    }
    result.capabilities = "(mccs_ver(2.1)vcp(12 14(03 05)60(0F 11)DC(00 03)))";
    for (const auto& [code, value] : fake::features) {
        MonitorFeatureInfo feature;
        feature.code = code;
        feature.advertised = feature.readable = feature.writable = true;
        feature.current = value;
        feature.maximum = 100;
        feature.allowedValues = {0, 3, 5, 15, 17};
        result.features.push_back(feature);
    }
    return result;
}
VgnBatteryInfo WindowsBackend::queryVgnBattery(const HidDeviceInfo&) const {
    return {};
}
VgnFlashWriteResult WindowsBackend::writeVgnDpiStage(const HidDeviceInfo&, int, int, int) const {
    fake::notePrivateWrite();
    return {};
}
VgnScalarWriteResult WindowsBackend::writeVgnPollingRate(const HidDeviceInfo&, int) const {
    fake::notePrivateWrite();
    return {};
}
VgnScalarWriteResult WindowsBackend::writeVgnActiveStage(const HidDeviceInfo&, int) const {
    fake::notePrivateWrite();
    return {};
}
N9ControlsInfo WindowsBackend::queryN9Controls(const HidDeviceInfo&, bool) const {
    return {};
}
} // namespace peripheral::platform
namespace peripheral::app {
RestoreResult restoreDeviceSettings(const platform::WindowsBackend&, const std::wstring&, bool,
                                    const std::map<std::string, int>&) {
    return {false, "Unused fake restoration"};
}
} // namespace peripheral::app

using namespace peripheral;
using namespace peripheral::app;
namespace {
int checks = 0;
int failures = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}
bool until(const std::function<bool()>& predicate, int timeout = 4000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QCoreApplication::processEvents();
    return predicate();
}
struct RefreshResult {
    quint64 ticket;
    bool success;
    QString message;
};
bool hasResult(const QList<RefreshResult>& results, quint64 ticket) {
    return std::any_of(results.begin(), results.end(),
                       [ticket](const auto& result) { return result.ticket == ticket; });
}
bool settled(DeviceService& service) {
    return !service.volumeUpdatePending("out-A") && !service.volumeUpdatePending("in-A") &&
           !service.volumeUpdatePending("in-B");
}
void dualChannels(DeviceService& service) {
    service.selectVolumeTarget("out-A");
    service.selectVolumeTarget("in-A", true);
    for (int offset = 0; offset <= 20; ++offset) {
        service.setFeature("out-A", DeviceFeature::Volume, 25 + offset);
        service.setFeature("in-A", DeviceFeature::Volume, 45 + offset);
    }
    check(service.volumeUpdatePending("out-A") && service.volumeUpdatePending("in-A"),
          "output and input retain independent pending work");
    service.finishVolume("out-A");
    service.finishVolume("in-A");
    check(until([&] { return settled(service); }), "both channels finish writes and readbacks");
    check(fake::volume(L"out-A") == 45 && fake::volume(L"in-A") == 65,
          "same-round volume requests reach both final target values");
    check(fake::countWrites(L"out-A", 45) == 1 && fake::countWrites(L"in-A", 65) == 1,
          "unified setFeature routes each final write once to correct native endpoint");
    check(service.snapshot().audioSampledMs > 0 && service.snapshot().audioEnumerationSucceeded,
          "audio readback records a real sampled timestamp");

    fake::arm(fake::audioGate);
    service.setFeature("in-A", DeviceFeature::Volume, 66);
    service.finishVolume("in-A");
    check(until([] { return fake::entered(fake::audioGate); }), "input readback can be held independently");
    service.setFeature("out-A", DeviceFeature::Volume, 55);
    service.finishVolume(); // 兼容原来只有输出通道的无参数调用。
    fake::release(fake::audioGate);
    check(until([&] { return !service.volumeUpdatePending("in-A") && service.volumeUpdatePending("out-A"); }),
          "input readback completes without clearing output pending/readback state");
    check(until([&] { return settled(service); }), "output separately finishes its later readback");
    check(fake::volume(L"out-A") == 55 && fake::volume(L"in-A") == 66,
          "independent readback preserves values");

    fake::arm(fake::writeGate);
    service.setFeature("out-A", DeviceFeature::Volume, 56);
    check(until([] { return fake::entered(fake::writeGate); }),
          "output write holds worker for generation test");
    service.setFeature("in-A", DeviceFeature::Volume, 70);
    service.selectVolumeTarget("in-B", true);
    service.setFeature("in-B", DeviceFeature::Volume, 80);
    service.finishVolume("in-B");
    service.setFeature("out-A", DeviceFeature::Volume, 60);
    service.finishVolume("out-A");
    fake::release(fake::writeGate);
    check(until([&] { return settled(service); }), "switching input completes all current work");
    check(fake::countWrites(L"in-A", 70) == 0, "old input generation is canceled before backend dispatch");
    check(fake::volume(L"in-B") == 80 && fake::volume(L"out-A") == 60,
          "new input target and existing output remain independent");

    fake::arm(fake::writeGate);
    service.setFeature("out-A", DeviceFeature::Volume, 61);
    check(until([] { return fake::entered(fake::writeGate); }), "output write holds worker for clear test");
    service.setFeature("in-B", DeviceFeature::Volume, 81);
    service.selectVolumeTarget({}, true);
    check(!service.volumeUpdatePending({}), "cleared input is not an empty-ID device with pending settings");
    service.setFeature("out-A", DeviceFeature::Volume, 62);
    service.finishVolume("out-A");
    service.finishVolume("in-B"); // 旧 sliderReleased 不得误 flush 当前其他目标。
    fake::release(fake::writeGate);
    check(until([&] { return settled(service); }), "clearing input does not strand output pending work");
    check(fake::countWrites(L"in-B", 81) == 0 && fake::volume(L"out-A") == 62,
          "cleared input request is canceled while output reaches final target");
    service.selectVolumeTarget("in-A", true);

    fake::arm(fake::writeGate);
    service.setFeature("in-A", DeviceFeature::Volume, 72);
    check(until([] { return fake::entered(fake::writeGate); }),
          "old input write has already entered backend");
    service.selectVolumeTarget("in-B", true);
    service.setFeature("in-B", DeviceFeature::Volume, 83);
    service.setFeature("out-A", DeviceFeature::Volume, 63);
    fake::release(fake::writeGate);
    check(until([&] { return settled(service); }),
          "in-flight old input completion cannot strand new generation");
    check(fake::volume(L"in-A") == 72 && fake::volume(L"in-B") == 83 && fake::volume(L"out-A") == 63,
          "already-started request stays on old ID while new input/output finish independently");

    service.selectVolumeTarget("in-A", true);
    fake::arm(fake::audioGate);
    service.setFeature("in-A", DeviceFeature::Volume, 73);
    check(until([] { return fake::entered(fake::audioGate); }),
          "old input readback is held before target switch");
    service.selectVolumeTarget("in-B", true);
    service.setFeature("in-B", DeviceFeature::Volume, 84);
    {
        std::lock_guard lock(fake::mutex);
        fake::volumes[L"in-B"] = 11; // 旧查询稍后返回该值，不得回填到新目标。
    }
    fake::arm(fake::writeGate);
    fake::release(fake::audioGate);
    check(until([] { return fake::entered(fake::writeGate); }),
          "new generation write waits behind old readback");
    check(service.volumeUpdatePending("in-B"), "old readback does not clear new input pending state");
    const auto& audio = service.snapshot().audio;
    const auto saved =
        std::find_if(audio.begin(), audio.end(), [](const auto& endpoint) { return endpoint.id == L"in-B"; });
    check(saved != audio.end() && saved->volumePercent != 11,
          "stale prior-generation readback cannot replace current snapshot");
    fake::release(fake::writeGate);
    check(until([&] { return settled(service); }) && fake::volume(L"in-B") == 84,
          "fresh new input write and readback still complete after old result is discarded");
    service.selectVolumeTarget("in-A", true);
}

void refreshTickets(DeviceService& service, QList<RefreshResult>& results, int& refreshed) {
    const int monitorBefore = fake::monitorCount();
    const auto forced = service.requestRefresh();
    check(forced > 0 && until([&] { return hasResult(results, forced); }),
          "manual refresh reports its ticket");
    check(fake::monitorCount() == monitorBefore + 1, "manual refresh bypasses the 15-second monitor cache");
    check(results.back().success && results.back().message.contains(QStringLiteral("采样时间")),
          "successful manual feedback includes actual sampled time");

    fake::arm(fake::hidGate);
    service.refresh(false); // 先于用户点击开始的旧扫描。
    check(until([] { return fake::entered(fake::hidGate); }), "older full scan is in flight");
    fake::arm(fake::monitorGate); // 旧扫描使用热缓存，新手动扫描必须再次实际枚举。
    const auto queued = service.requestRefresh();
    fake::release(fake::hidGate);
    check(until([] { return fake::entered(fake::monitorGate); }),
          "busy refresh request queues a new forced scan");
    check(!hasResult(results, queued), "older full scan cannot finish the newer manual ticket");
    fake::release(fake::monitorGate);
    check(until([&] { return hasResult(results, queued); }),
          "queued manual ticket finishes after its actual new scan");

    fake::arm(fake::hidGate);
    const auto superseded = service.requestRefresh();
    check(until([] { return fake::entered(fake::hidGate); }), "first manual request starts scan");
    const auto latest = service.requestRefresh();
    fake::release(fake::hidGate);
    check(until([&] { return hasResult(results, latest); }), "latest manual request eventually completes");
    check(latest > superseded && !hasResult(results, superseded),
          "superseded ticket cannot clear latest request");

    for (const QString id : {QStringLiteral("out-A"), QStringLiteral("in-A")}) {
        fake::arm(fake::writeGate);
        service.setFeature(id, DeviceFeature::Volume, id == QStringLiteral("out-A") ? 69 : 79);
        check(until([] { return fake::entered(fake::writeGate); }),
              "audio control is pending before manual refresh");
        {
            std::lock_guard lock(fake::mutex);
            fake::events.clear();
        }
        fake::arm(fake::hidGate);
        const auto ticket = service.requestRefresh();
        const int beforePartial = refreshed;
        fake::release(fake::writeGate);
        check(until([] { return fake::entered(fake::hidGate); }), "manual scan resumes after audio settles");
        check(refreshed > beforePartial && !hasResult(results, ticket),
              "partial audio readback does not complete queued manual ticket");
        {
            std::lock_guard lock(fake::mutex);
            const auto audio = std::find(fake::events.begin(), fake::events.end(), "audio");
            const auto hid = std::find(fake::events.begin(), fake::events.end(), "hid");
            check(audio != fake::events.end() && hid != fake::events.end() && audio < hid,
                  "full scan waits for input/output readback before native enumeration");
        }
        fake::release(fake::hidGate);
        check(until([&] { return hasResult(results, ticket); }),
              "deferred control refresh completes correct ticket");
    }
    {
        std::lock_guard lock(fake::mutex);
        fake::failMonitor = true;
    }
    const auto failed = service.requestRefresh();
    check(until([&] { return hasResult(results, failed); }),
          "failed actual scan still completes manual feedback");
    check(!results.back().success && results.back().message.contains("Fake monitor unavailable"),
          "failed refresh includes real enumeration error summary");
    check(!service.snapshot().monitorEnumerationSucceeded, "failed monitor scan is not reported successful");
    {
        std::lock_guard lock(fake::mutex);
        fake::failMonitor = false;
    }
    const auto recovered = service.requestRefresh();
    check(until([&] { return hasResult(results, recovered); }) && results.back().success,
          "next manual refresh recovers after backend error");
}

void monitorScheduling(DeviceService& service, QList<RefreshResult>& refreshResults) {
    QObject receiver;
    QStringList progress;
    QStringList finished;
    QObject::connect(&service, &DeviceService::monitorFeatureProgress, &receiver,
                     [&](const QString& message) { progress.append(message); });
    QObject::connect(&service, &DeviceService::monitorFeatureFinished, &receiver,
                     [&](const QString& message) { finished.append(message); });
    service.selectMonitorControlsTarget("mon-A");
    service.requestMonitorControls("mon-A");
    check(until([&] { return !service.monitorControlsBusy(); }), "initial advanced fixture query completes");
    const auto initialSample = service.monitorControls().sampledMs;
    const int initialMonitors = fake::monitorCount();
    int initialFull = 0;
    {
        std::lock_guard lock(fake::mutex);
        initialFull = fake::advancedReads;
    }
    fake::arm(fake::hidGate);
    const auto ticket = service.requestRefresh();
    check(until([] { return fake::entered(fake::hidGate); }),
          "background full scan is held before DDC stage");
    fake::arm(fake::advancedGate);
    service.setFeature("mon-A", DeviceFeature::ColorPreset, 5);
    fake::release(fake::hidGate);
    check(until([&] { return progress.size() == 1; }),
          "Set completion reports progress before readback finishes");
    check(finished.isEmpty(), "request-sent progress is not final verification");
    check(fake::monitorCount() == initialMonitors,
          "user feature write bypasses deferred background DDC enumeration");
    {
        std::lock_guard lock(fake::mutex);
        check(fake::features.at(0x14) == 5,
              "selected color command is already dispatched before settle timer");
    }
    service.refresh(false);
    service.setFeature("out-A", DeviceFeature::Volume, 64);
    service.finishVolume("out-A");
    check(until([] { return fake::volume(L"out-A") == 64; }),
          "settle timer does not occupy worker or block audio write");
    check(until([] { return fake::entered(fake::advancedGate); }),
          "full related query is deferred until target confirmation");
    check(until([&] { return finished.size() == 1; }) &&
              finished.back().contains(QStringLiteral("已回读确认")),
          "single target is verified before slower related-query completion");
    check(!hasResult(refreshResults, ticket),
          "deferred full scan cannot prematurely finish manual refresh ticket");
    check(service.monitorControls().features.size() == 4 &&
              service.monitorControls().sampledMs == initialSample,
          "single-item readback never masquerades as a newly sampled full capabilities snapshot");
    {
        std::lock_guard lock(fake::mutex);
        const auto audio = std::find(fake::events.rbegin(), fake::events.rend(), "audio-write");
        const auto target = std::find(fake::events.rbegin(), fake::events.rend(), "target-read");
        check(audio != fake::events.rend() && target != fake::events.rend() && target < audio,
              "audio write runs during settle period before targeted DDC readback");
        check(fake::targetReads == 1 && fake::advancedReads == initialFull + 1,
              "one target verification precedes one asynchronous related query");
    }
    fake::release(fake::advancedGate);
    check(until([&] { return !service.monitorControlsBusy() && hasResult(refreshResults, ticket); }),
          "related query and deferred full scan both eventually finish");
    check(fake::monitorCount() == initialMonitors + 1,
          "deferred manual scan executes once after feature finishes");

    const int beforeQuietWrite = fake::monitorCount();
    service.setFeature("mon-A", DeviceFeature::Contrast, 51);
    check(until([&] { return finished.size() == 2 && !service.monitorControlsBusy(); }),
          "contrast verifies without preset settle delay");
    check(fake::monitorCount() == beforeQuietWrite,
          "feature completion does not force a new USB/audio/brightness full scan");
    int readsBeforeFailure = 0;
    {
        std::lock_guard lock(fake::mutex);
        readsBeforeFailure = fake::targetReads;
        check(fake::featureWrites == 2 && fake::targetReads == 2,
              "each successful request writes once and verifies only its original code");
        check(fake::maxDdc == 1, "all DDC stages remain serialized on one worker");
    }
    service.setMonitorFeature("mon-A", 0x99, 7);
    check(until([&] { return finished.size() == 3; }) && finished.back().contains(QStringLiteral("失败")),
          "failed validation reports failure without claiming sent or verified");
    check(progress.size() == 2 && !service.monitorControlsBusy(),
          "failed preflight does not enter settle/progress phase");
    {
        std::lock_guard lock(fake::mutex);
        check(fake::targetReads == readsBeforeFailure,
              "failed write validation is not followed by target readback");
    }
    service.setFeature("mon-A", DeviceFeature::ColorPreset, 3);
    check(until([&] { return progress.size() == 3; }),
          "second preset command enters asynchronous settle phase");
    service.selectMonitorControlsTarget("mon-B");
    service.requestMonitorControls("mon-B");
    QElapsedTimer cancelled;
    cancelled.start();
    until([&] { return cancelled.elapsed() >= 300 && !service.monitorControlsBusy(); });
    {
        std::lock_guard lock(fake::mutex);
        check(fake::targetReads == readsBeforeFailure,
              "target change cancels old settle timer before stale verification query");
    }
    check(finished.size() == 3 && service.monitorControls().id == L"mon-B",
          "old operation cannot publish completion or capabilities into new target");
}

void capabilityCache(DeviceService& service) {
    QObject receiver;
    int finished = 0;
    QString lastResult;
    QObject::connect(&service, &DeviceService::monitorFeatureFinished, &receiver,
                     [&](const QString& message) { ++finished; lastResult = message; });
    const auto now = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const auto load = [&] {
        service.requestMonitorControls("mon-B");
        check(until([&] { return !service.monitorControlsBusy(); }), "capabilities query settles");
        check(!service.monitorCapabilities_.capabilities.empty(), "successful full query creates current lease");
    };
    const auto counts = [] {
        std::lock_guard lock(fake::mutex);
        return std::pair{fake::freshWrites, fake::cachedWrites};
    };
    const auto apply = [&](int value) {
        const int before = finished;
        service.setFeature("mon-B", DeviceFeature::Contrast, value);
        check(until([&] { return finished == before + 1 && !service.monitorControlsBusy(); }),
              "contrast write verification and related query settle");
    };

    load();
    auto before = counts();
    apply(52);
    check(counts() == std::pair{before.first, before.second + 1},
          "same-connection validated capabilities use fast path");

    load();
    before = counts();
    fake::arm(fake::hidGate);
    service.refresh(false);
    check(until([] { return fake::entered(fake::hidGate); }), "worker is held before cached write");
    const int beforeForced = finished;
    service.setFeature("mon-B", DeviceFeature::Contrast, 53);
    service.refresh(true); // 入队后强制刷新必须使已经复制的声明失效。
    fake::release(fake::hidGate);
    check(until([&] { return finished == beforeForced + 1 && !service.monitorControlsBusy() && !service.busy_; }),
          "force invalidation after enqueue completes without losing background refresh");
    check(counts() == std::pair{before.first + 1, before.second},
          "worker revision check rejects queued declaration invalidated by force refresh");

    load();
    before = counts();
    service.monitorCapabilitiesAt_ = now() - 300001;
    apply(54);
    check(counts() == std::pair{before.first + 1, before.second}, "expired declaration uses fresh validation");

    load();
    before = counts();
    service.monitorCapabilitiesAt_ = now() - 299900;
    fake::arm(fake::hidGate);
    service.refresh(false);
    check(until([] { return fake::entered(fake::hidGate); }), "worker is gated at TTL boundary");
    const int beforeExpiry = finished;
    service.setFeature("mon-B", DeviceFeature::Contrast, 55);
    QElapsedTimer elapsed;
    elapsed.start();
    until([&] { return elapsed.elapsed() >= 160; });
    fake::release(fake::hidGate);
    check(until([&] { return finished == beforeExpiry + 1 && !service.monitorControlsBusy() && !service.busy_; }),
          "write queued across TTL boundary completes");
    check(counts() == std::pair{before.first + 1, before.second},
          "worker rechecks monotonic TTL at execution rather than only at enqueue");

    load();
    {
        std::lock_guard lock(fake::mutex);
        fake::failAdvanced = true;
    }
    service.requestMonitorControls("mon-B");
    check(until([&] { return !service.monitorControlsBusy(); }), "failed full query settles");
    check(service.monitorCapabilities_.capabilities.empty(), "new failed full query cannot leave old lease active");
    before = counts();
    apply(56);
    check(counts() == std::pair{before.first + 1, before.second},
          "write after failed query falls back to fresh validation");
    {
        std::lock_guard lock(fake::mutex);
        fake::failAdvanced = false;
    }

    load();
    fake::arm(fake::advancedGate);
    service.requestMonitorControls("mon-B");
    check(until([] { return fake::entered(fake::advancedGate); }), "older capability query is held");
    service.refresh(true);
    fake::release(fake::advancedGate);
    check(until([&] { return !service.monitorControlsBusy() && !service.busy_; }),
          "capability result and newer forced scan settle");
    check(service.monitorCapabilities_.capabilities.empty(), "stale full-query result cannot reactivate lease");
    before = counts();
    apply(57);
    check(counts() == std::pair{before.first + 1, before.second}, "stale query does not authorize fast write");

    load();
    int writesBeforeRetry = 0, readsBeforeRetry = 0;
    {
        std::lock_guard lock(fake::mutex);
        writesBeforeRetry = fake::featureWrites;
        readsBeforeRetry = fake::targetReads;
        fake::mismatchedTargetReads = 1;
    }
    apply(59);
    check(lastResult.contains(QStringLiteral("已回读确认")), "delayed device result is confirmed by bounded follow-up read");
    {
        std::lock_guard lock(fake::mutex);
        check(fake::featureWrites == writesBeforeRetry + 1 && fake::targetReads == readsBeforeRetry + 2,
              "transient read mismatch adds one read and never repeats Set");
        writesBeforeRetry = fake::featureWrites;
        readsBeforeRetry = fake::targetReads;
        fake::mismatchedTargetReads = 2;
    }
    apply(60);
    check(lastResult.contains(QStringLiteral("未确认")), "persistent mismatch remains explicitly unconfirmed");
    {
        std::lock_guard lock(fake::mutex);
        check(fake::featureWrites == writesBeforeRetry + 1 && fake::targetReads == readsBeforeRetry + 2,
              "failed confirmation is bounded to two reads with no rewrite or rollback");
    }

    load();
    fake::arm(fake::advancedGate);
    const int beforeMode = finished;
    before = counts();
    service.setFeature("mon-B", DeviceFeature::ColorPreset, 5);
    check(service.monitorCapabilities_.capabilities.empty(), "preset invalidates declaration for later commands immediately");
    check(until([] { return fake::entered(fake::advancedGate); }), "mode target confirms before full query finishes");
    check(finished == beforeMode + 1 && service.monitorCapabilities_.capabilities.empty(),
          "mode change cannot reuse pre-change lease during related query");
    check(counts() == std::pair{before.first, before.second + 1},
          "mode command itself may use its current validated declaration once");
    fake::release(fake::advancedGate);
    check(until([&] { return !service.monitorControlsBusy(); }), "new post-mode declaration finishes");
    check(!service.monitorCapabilities_.capabilities.empty(), "successful related query builds new post-mode lease");

    service.selectMonitorControlsTarget("mon-C");
    check(service.monitorCapabilities_.capabilities.empty(), "target change immediately clears declaration");
    before = counts();
    const int beforeSwitch = finished;
    service.setFeature("mon-C", DeviceFeature::Contrast, 58);
    check(until([&] { return finished == beforeSwitch + 1 && !service.monitorControlsBusy(); }),
          "new target write settles without borrowing previous target declaration");
    check(counts() == std::pair{before.first + 1, before.second}, "new target uses fresh validation");
}

// 实验性写入的安全属性：开关关闭时，连后端都不会被调用。
void experimentalWriteGate() {
    {
        std::lock_guard lock(fake::mutex);
        peripheral::platform::HidDeviceInfo device;
        device.path = L"vgn-test";
        device.vid = 0x3554;
        device.pid = 0xFB56;
        device.vgnBatteryCandidate = true;
        fake::hidDevices = {device};
    }
    DeviceService service(false);
    int refreshed = 0;
    std::vector<QString> messages;
    bool lastSuccess = true;
    QObject::connect(&service, &DeviceService::refreshed, [&] { ++refreshed; });
    QObject::connect(&service, &DeviceService::operationFinished,
                     [&](const QString& message, bool success) {
                         messages.push_back(message);
                         lastSuccess = success;
                     });
    service.refresh(true);
    check(until([&] { return refreshed > 0; }), "write fixture scan completes");
    check(service.snapshot().hid.size() == 1, "write fixture exposes one VGN interface");

    const int baseline = fake::privateWriteCount();
    service.writeVgnDpiStage("vgn-test", 0, 800, 800);
    check(!messages.empty() && messages.back().contains(QStringLiteral("未开启")),
          "closed switch refuses the write with a visible reason");
    check(!lastSuccess, "refused write reports failure");
    check(fake::privateWriteCount() == baseline, "closed switch never reaches the backend");

    service.setExperimentalWritesEnabled(true);
    messages.clear();
    service.writeVgnDpiStage("vgn-test", 0, 800, 800);
    check(until([&] { return fake::privateWriteCount() == baseline + 1; }),
          "enabled switch sends exactly one backend write");
    check(until([&] { return !messages.empty(); }), "enabled write reports a result");
    check(!lastSuccess, "an unverified backend result is reported as failure, not success");

    service.setExperimentalWritesEnabled(false);
    service.writeVgnDpiStage("vgn-test", 0, 800, 800);
    check(fake::privateWriteCount() == baseline + 1, "closing the switch stops further writes");

    {
        DeviceService passive(true);
        passive.setExperimentalWritesEnabled(true);
        std::vector<QString> passiveMessages;
        QObject::connect(&passive, &DeviceService::operationFinished,
                         [&](const QString& message, bool) { passiveMessages.push_back(message); });
        passive.writeVgnDpiStage("vgn-test", 0, 800, 800);
        check(!passiveMessages.empty() && passiveMessages.back().contains(QStringLiteral("未开启")),
              "passive mode refuses writes even with the switch on");
        check(fake::privateWriteCount() == baseline + 1, "passive mode never reaches the backend");
    }
    {
        std::lock_guard lock(fake::mutex);
        fake::hidDevices.clear();
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    DeviceService service(false);
    int refreshed = 0;
    QList<RefreshResult> results;
    QObject::connect(&service, &DeviceService::refreshed, [&] { ++refreshed; });
    QObject::connect(&service, &DeviceService::refreshFinished,
                     [&](quint64 ticket, bool success, const QString& message) {
                         results.append({ticket, success, message});
                     });
    service.refresh(true);
    check(until([&] { return refreshed > 0; }), "initial fake scan completes");
    dualChannels(service);
    refreshTickets(service, results, refreshed);
    monitorScheduling(service, results);
    capabilityCache(service);
    experimentalWriteGate();
    fake::release(fake::writeGate);
    fake::release(fake::audioGate);
    fake::release(fake::hidGate);
    fake::release(fake::monitorGate);
    fake::release(fake::advancedGate);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
