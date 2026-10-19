#include "app/SettingsRestore.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

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

using namespace peripheral::platform;
using peripheral::app::restoreDeviceSettings;

struct Write {
    std::wstring id;
    std::string type;
    int code = 0;
    int value = 0;
};
struct FakeState {
    std::vector<MonitorInfo> monitors;
    std::vector<AudioEndpointInfo> audio;
    MonitorControlsInfo controls;
    std::vector<Write> writes;
    int monitorReads = 0;
    int audioReads = 0;
    int controlReads = 0;
    int otherCalls = 0;
    bool enumerateThrows = false;
    bool writeThrows = false;
    bool disappearAfterWrite = false;
    int brightnessDelta = 0;
    float volumeDelta = 0;
    int scenarioDelta = 0;
} fake;

void reset() {
    fake = {};
    MonitorInfo monitor;
    monitor.id = L"monitor-A";
    monitor.name = L"Same monitor name";
    monitor.brightnessReadable = true;
    monitor.brightnessPercent = 50;
    fake.monitors.push_back(monitor);
    AudioEndpointInfo audio;
    audio.id = L"audio-A";
    audio.name = L"Same audio name";
    audio.volumeReadable = true;
    audio.volumePercent = 50;
    audio.muted = true;
    fake.audio.push_back(audio);
    fake.controls.id = monitor.id;
    MonitorFeatureInfo scenario;
    scenario.code = 0xDC;
    scenario.advertised = true;
    scenario.readable = true;
    scenario.writable = true;
    scenario.current = 2;
    scenario.allowedValues = {1, 2, 3};
    fake.controls.features.push_back(scenario);
    auto source = scenario;
    source.code = 0x60;
    source.current = 15;
    source.allowedValues = {15, 17, 18};
    fake.controls.features.push_back(source);
}

void noOtherCalls() {
    CHECK(fake.otherCalls == 0);
}
} // namespace

// 链接时完整替代 WindowsBackend，无真实 HID、音频或 DDC 调用。
namespace peripheral::platform {
std::vector<HidDeviceInfo> WindowsBackend::enumerateHidDevices() const {
    ++fake.otherCalls;
    throw std::runtime_error("Unexpected HID call");
}
std::vector<AudioEndpointInfo> WindowsBackend::enumerateAudioEndpoints() const {
    ++fake.audioReads;
    if (fake.enumerateThrows)
        throw std::runtime_error("Fake enumeration failed");
    if (fake.disappearAfterWrite && !fake.writes.empty())
        return {};
    return fake.audio;
}
std::vector<MonitorInfo> WindowsBackend::enumerateMonitors() const {
    ++fake.monitorReads;
    if (fake.enumerateThrows)
        throw std::runtime_error("Fake enumeration failed");
    if (fake.disappearAfterWrite && !fake.writes.empty())
        return {};
    return fake.monitors;
}
void WindowsBackend::setAudioVolume(const std::wstring& id, float percent) const {
    fake.writes.push_back({id, "volume", 0, static_cast<int>(percent)});
    if (fake.writeThrows)
        throw std::runtime_error("Fake write failed");
    for (auto& endpoint : fake.audio) {
        if (endpoint.id == id)
            endpoint.volumePercent = percent + fake.volumeDelta;
    }
}
void WindowsBackend::setAudioMute(const std::wstring&, bool) const {
    ++fake.otherCalls;
    throw std::runtime_error("Mute must remain unchanged");
}
void WindowsBackend::setMonitorBrightness(const std::wstring& id, int percent) const {
    fake.writes.push_back({id, "brightness", 0, percent});
    if (fake.writeThrows)
        throw std::runtime_error("Fake write failed");
    for (auto& monitor : fake.monitors) {
        if (monitor.id == id)
            monitor.brightnessPercent = percent + fake.brightnessDelta;
    }
}
MonitorControlsInfo WindowsBackend::queryMonitorControls(const std::wstring&) const {
    ++fake.controlReads;
    return fake.controls;
}
void WindowsBackend::setMonitorFeature(const std::wstring& id, std::uint8_t code, std::uint16_t value) const {
    fake.writes.push_back({id, "feature", code, value});
    if (code != 0xDC)
        ++fake.otherCalls;
    if (fake.writeThrows)
        throw std::runtime_error("Fake write failed");
    for (auto& feature : fake.controls.features) {
        if (feature.code == code)
            feature.current = static_cast<std::uint32_t>(value + fake.scenarioDelta);
    }
}
VgnBatteryInfo WindowsBackend::queryVgnBattery(const HidDeviceInfo&) const {
    ++fake.otherCalls;
    throw std::runtime_error("Unexpected battery call");
}
} // namespace peripheral::platform

int main() {
    const WindowsBackend backend;
    reset();
    auto result = restoreDeviceSettings(backend, L"missing", true,
                                        {{"vcp_60", 17}, {"vcp_12", 50}, {"vcp_14", 5}, {"mute", 0}});
    CHECK(result.success);
    CHECK(fake.writes.empty());
    CHECK(fake.monitorReads == 0);
    CHECK(fake.controlReads == 0);
    result = restoreDeviceSettings(backend, L"missing", false, {{"brightness", 90}, {"vcp_dc", 1}});
    CHECK(result.success);
    CHECK(fake.audioReads == 0);
    noOtherCalls();

    reset();
    result = restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", -1}, {"vcp_dc", 65536}});
    CHECK(!result.success);
    CHECK(fake.monitorReads == 0);
    CHECK(fake.writes.empty());
    result = restoreDeviceSettings(backend, L"audio-A", false, {{"volume", 101}});
    CHECK(!result.success);
    CHECK(fake.audioReads == 0);

    reset();
    result = restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", 50}, {"vcp_dc", 2}});
    CHECK(result.success);
    CHECK(fake.writes.empty());
    CHECK(fake.monitorReads == 1);
    CHECK(fake.controlReads == 1);

    reset();
    fake.brightnessDelta = -1; // 百分比允许一次读回有 1 点误差。
    result = restoreDeviceSettings(backend, L"monitor-A", true,
                                   {{"brightness", 70}, {"vcp_dc", 3}, {"vcp_60", 17}, {"vcp_14", 5}});
    CHECK(result.success);
    CHECK(fake.writes.size() == 2);
    CHECK(fake.writes[0].id == L"monitor-A");
    CHECK(fake.writes[0].type == "brightness");
    CHECK(fake.writes[1].code == 0xDC);
    CHECK(fake.writes[1].value == 3);
    CHECK(fake.monitorReads == 2);
    CHECK(fake.controlReads == 2);
    CHECK(fake.controls.features[1].current == 15); // 输入源保持原值。
    noOtherCalls();

    reset();
    fake.controls.features[0].current = 0; // H24T7 已观察到的声明外 current，不得猜测。
    result = restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", 60}, {"vcp_dc", 3}});
    CHECK(!result.success);
    CHECK(result.message.find("当前回报值不在") != std::string::npos);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.writes[0].type == "brightness");
    CHECK(fake.monitorReads == 2);
    CHECK(fake.controlReads == 1);

    reset();
    result = restoreDeviceSettings(backend, L"monitor-A", true, {{"vcp_dc", 4}});
    CHECK(!result.success);
    CHECK(fake.writes.empty());
    CHECK(fake.controlReads == 1);
    for (int flag = 0; flag < 3; ++flag) {
        reset();
        auto& feature = fake.controls.features[0];
        if (flag == 0)
            feature.advertised = false;
        else if (flag == 1)
            feature.readable = false;
        else
            feature.writable = false;
        CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"vcp_dc", 3}}).success);
        CHECK(fake.writes.empty());
    }
    reset();
    fake.controls.id = L"monitor-B";
    CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"vcp_dc", 3}}).success);
    CHECK(fake.writes.empty());
    reset();
    fake.controls.features.push_back(fake.controls.features[0]);
    CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"vcp_dc", 3}}).success);
    CHECK(fake.writes.empty());
    reset();
    fake.controls.features[0].current = 65536;
    CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"vcp_dc", 3}}).success);
    CHECK(fake.writes.empty());

    reset();
    fake.monitors[0].brightnessReadable = false;
    result = restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", 70}, {"vcp_dc", 3}});
    CHECK(!result.success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.writes[0].code == 0xDC);
    CHECK(fake.monitorReads == 1);
    CHECK(fake.controlReads == 2);

    reset();
    CHECK(!restoreDeviceSettings(backend, L"monitor-B", true, {{"brightness", 60}}).success);
    CHECK(fake.writes.empty());
    fake.monitors.push_back(fake.monitors[0]);
    CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", 60}}).success);
    CHECK(fake.writes.empty());
    reset();
    fake.monitors.push_back(fake.monitors[0]);
    fake.monitors[1].id = L"monitor-B";
    result = restoreDeviceSettings(backend, L"monitor-B", true, {{"brightness", 60}});
    CHECK(result.success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.writes[0].id == L"monitor-B");
    CHECK(fake.monitors[0].brightnessPercent == 50);

    reset();
    fake.brightnessDelta = 2;
    CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", 70}}).success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.monitorReads == 2);
    reset();
    fake.scenarioDelta = -1;
    CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"vcp_dc", 3}}).success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.controlReads == 2);
    reset();
    fake.disappearAfterWrite = true;
    CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", 70}}).success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.monitorReads == 2);
    reset();
    fake.writeThrows = true;
    CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", 70}}).success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.monitorReads == 2);

    reset();
    CHECK(restoreDeviceSettings(backend, L"audio-A", false, {{"volume", 50}, {"mute", 0}}).success);
    CHECK(fake.writes.empty());
    CHECK(fake.audioReads == 1);
    CHECK(fake.audio[0].muted);
    noOtherCalls();
    // Core Audio 的小数与档案整数采用相同显示精度，不能因舍入误差反复写回。
    for (const float actual : {49.51F, 49.999F, 50.001F, 50.49F}) {
        reset();
        fake.audio[0].volumePercent = actual;
        CHECK(restoreDeviceSettings(backend, L"audio-A", false, {{"volume", 50}}).success);
        CHECK(fake.writes.empty());
        CHECK(fake.audioReads == 1);
    }
    reset();
    fake.audio[0].volumePercent = 50.5F;
    CHECK(restoreDeviceSettings(backend, L"audio-A", false, {{"volume", 50}}).success);
    CHECK(fake.writes.size() == 1);
    reset();
    fake.volumeDelta = 0.75F;
    result = restoreDeviceSettings(backend, L"audio-A", false, {{"volume", 30}, {"mute", 0}, {"vcp_60", 17}});
    CHECK(result.success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.writes[0].type == "volume");
    CHECK(fake.audioReads == 2);
    CHECK(fake.audio[0].muted);
    noOtherCalls();

    reset();
    fake.audio.push_back(fake.audio[0]);
    fake.audio[1].id = L"audio-B";
    CHECK(restoreDeviceSettings(backend, L"audio-B", false, {{"volume", 25}}).success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.writes[0].id == L"audio-B");
    CHECK(fake.audio[0].volumePercent == 50);
    CHECK(fake.audio[1].muted);

    for (int invalid = 0; invalid < 3; ++invalid) {
        reset();
        if (invalid == 0)
            fake.audio[0].volumeReadable = false;
        else if (invalid == 1)
            fake.audio[0].volumePercent = std::numeric_limits<float>::quiet_NaN();
        else
            fake.audio.push_back(fake.audio[0]);
        CHECK(!restoreDeviceSettings(backend, L"audio-A", false, {{"volume", 25}}).success);
        CHECK(fake.writes.empty());
    }

    reset();
    fake.audio[0].id = L"audio-wired-new-id";
    CHECK(!restoreDeviceSettings(backend, L"audio-A", false, {{"volume", 25}}).success);
    CHECK(fake.writes.empty());
    CHECK(restoreDeviceSettings(backend, L"audio-wired-new-id", false, {{"volume", 25}}).success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.writes[0].id == L"audio-wired-new-id");
    CHECK(fake.audio[0].muted);
    noOtherCalls();

    reset();
    fake.volumeDelta = 1.5F;
    CHECK(!restoreDeviceSettings(backend, L"audio-A", false, {{"volume", 25}}).success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.audioReads == 2);
    reset();
    fake.enumerateThrows = true;
    CHECK(!restoreDeviceSettings(backend, L"audio-A", false, {{"volume", 25}}).success);
    CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", 25}}).success);
    CHECK(fake.writes.empty());

    // last_input 仅为只读上下文，同源才恢复；初次能力查询可同时用于情景验证。
    reset();
    fake.controls.features[1].writable = false; // 比较输入源不需要写权限。
    result = restoreDeviceSettings(backend, L"monitor-A", true,
                                   {{"brightness", 70}, {"vcp_dc", 3}, {"last_input", 15}});
    CHECK(result.success);
    CHECK(fake.writes.size() == 2);
    CHECK(fake.controlReads == 2); // 初次一次、情景写后读回一次。
    CHECK(fake.controls.features[1].current == 15);
    noOtherCalls();

    reset();
    result = restoreDeviceSettings(backend, L"monitor-A", true,
                                   {{"brightness", 70}, {"vcp_dc", 3}, {"last_input", 17}});
    CHECK(result.success);
    CHECK(result.message == "输入源已变化，沿用当前设置");
    CHECK(fake.writes.empty());
    CHECK(fake.monitorReads == 1);
    CHECK(fake.controlReads == 1);
    CHECK(fake.monitors[0].brightnessPercent == 50);
    CHECK(fake.controls.features[0].current == 2);

    for (int invalid = 0; invalid < 5; ++invalid) {
        reset();
        auto& source = fake.controls.features[1];
        if (invalid == 0)
            source.current = 0; // 当前回报不在声明列表，不能当成保存的输入源。
        else if (invalid == 1)
            source.readable = false;
        else if (invalid == 2)
            source.advertised = false;
        else if (invalid == 3)
            fake.controls.id = L"other-monitor";
        else
            fake.controls.features.push_back(source);
        result = restoreDeviceSettings(backend, L"monitor-A", true,
                                       {{"brightness", 70}, {"vcp_dc", 3}, {"last_input", 15}});
        CHECK(!result.success);
        CHECK(fake.writes.empty());
        CHECK(fake.controlReads == 1);
    }
    reset();
    result = restoreDeviceSettings(backend, L"monitor-A", true,
                                   {{"brightness", 70}, {"vcp_dc", 3}, {"last_input", 16}});
    CHECK(!result.success); // 保存上下文也必须在当前显示器的声明列表中。
    CHECK(fake.writes.empty());
    reset();
    CHECK(!restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", 70}, {"last_input", 65536}})
               .success);
    CHECK(fake.writes.empty());
    CHECK(fake.controlReads == 0);

    // 无上下文时保留原有独立亮度路径：异常输入源不阻挡亮度，也不会写输入源。
    reset();
    fake.controls.features[1].current = 0;
    CHECK(restoreDeviceSettings(backend, L"monitor-A", true, {{"brightness", 70}, {"vcp_60", 17}}).success);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.writes[0].type == "brightness");
    CHECK(fake.controlReads == 0);
    CHECK(fake.controls.features[1].current == 0);
    noOtherCalls();

    std::cout << "Settings restore: " << checks << " checks passed\n";
}
