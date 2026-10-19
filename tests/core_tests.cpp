#include "core/device_registry.h"
#include "core/vgn_protocol.h"
#include "core/live_value_control.h"
#include "core/private_error_tracker.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using namespace peripheral;
using namespace std::chrono_literals;

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

Endpoint mouse(const std::string& id, TransportKind transport, const std::string& serial = {},
               const std::string& container = {}) {
    Endpoint result;
    result.endpointId = id;
    result.displayName = "VGN F2 Master+";
    result.vid = 0x3554;
    result.pid = transport == TransportKind::Receiver24G ? 0xFB56 : 0xFB57;
    result.transport = transport;
    result.kind = DeviceKind::Mouse;
    result.serial = serial;
    result.containerId = container;
    result.usagePage = 0xFF00;
    return result;
}

DeviceObservation battery(int percent, bool charging) {
    DeviceObservation result;
    result.linkState = LinkState::Online;
    result.reportedPercent = percent;
    result.charging = charging;
    DeviceCapabilities caps;
    caps.battery = true;
    caps.charging = true;
    result.caps = caps;
    result.statusDetail = "Validated battery response";
    return result;
}

void updateChecksum(vgn::Report& report) {
    const auto sum = std::accumulate(report.begin(), report.end() - 1, 0u);
    report.back() = static_cast<std::uint8_t>(0x55u - sum);
}

void protocolTests() {
    const auto onlineQuery = vgn::makeQuery(vgn::Command::Online);
    CHECK(onlineQuery.size() == 17);
    CHECK(onlineQuery[0] == 8);
    CHECK(onlineQuery[1] == 3);
    CHECK(onlineQuery.back() == 0x4A);
    CHECK(vgn::makeQuery(vgn::Command::Battery).back() == 0x49);

    auto onlineResponse = onlineQuery;
    onlineResponse[6] = 1;
    updateChecksum(onlineResponse);
    const auto online = vgn::parseResponse(onlineResponse, vgn::Command::Online);
    CHECK(online);
    CHECK(online.value->online == true);
    CHECK(!online.value->reportedPercent);
    onlineResponse[6] = 0;
    updateChecksum(onlineResponse);
    const auto unconfirmed = vgn::parseResponse(onlineResponse, vgn::Command::Online);
    CHECK(unconfirmed);
    CHECK(!unconfirmed.value->online.has_value());

    // Real FB56 receiver reply, captured while the user confirmed normal mouse
    // operation. A valid checksum does not prove the old offline interpretation.
    const vgn::Report workingMouseZeroHint = {0x08, 0x03, 0, 0, 0, 0x01, 0,
                                            0x6e, 0x2e, 0xdc, 0, 0, 0, 0, 0, 0, 0xd1};
    const auto captured = vgn::parseResponse(workingMouseZeroHint, vgn::Command::Online);
    CHECK(captured);
    CHECK(!captured.value->online.has_value());
    CHECK(!captured.value->reportedPercent);
    onlineResponse[6] = 2; // Unrecognized firmware value is also inconclusive.
    updateChecksum(onlineResponse);
    CHECK(!vgn::parseResponse(onlineResponse, vgn::Command::Online).value->online.has_value());

    auto batteryResponse = vgn::makeQuery(vgn::Command::Battery);
    batteryResponse[6] = 87;
    batteryResponse[7] = 1;
    batteryResponse[8] = 0x0F;
    batteryResponse[9] = 0xA0;
    updateChecksum(batteryResponse);
    const auto parsed = vgn::parseResponse(batteryResponse, vgn::Command::Battery);
    CHECK(parsed);
    CHECK(parsed.value->reportedPercent == 87); // Raw voltage must not replace 87%.
    CHECK(parsed.value->charging == true);
    CHECK(parsed.value->rawVoltage == 4000);
    CHECK(!parsed.value->online);

    batteryResponse[7] = 2; // Protocol says == 1, not merely != 0.
    updateChecksum(batteryResponse);
    CHECK(vgn::parseResponse(batteryResponse, vgn::Command::Battery).value->charging == false);
    for (const int percent : {0, 100}) {
        batteryResponse[6] = static_cast<std::uint8_t>(percent);
        updateChecksum(batteryResponse);
        CHECK(vgn::parseResponse(batteryResponse, vgn::Command::Battery).value->reportedPercent == percent);
    }
    batteryResponse[6] = 101;
    updateChecksum(batteryResponse);
    CHECK(vgn::parseResponse(batteryResponse, vgn::Command::Battery).error == vgn::ParseError::BatteryRange);
    batteryResponse[6] = 50;
    updateChecksum(batteryResponse);
    auto invalid = batteryResponse;
    invalid[0] = 9;
    CHECK(vgn::parseResponse(invalid, vgn::Command::Battery).error == vgn::ParseError::ReportId);
    invalid = batteryResponse;
    invalid[2] = 1;
    updateChecksum(invalid);
    CHECK(vgn::parseResponse(invalid, vgn::Command::Battery).error == vgn::ParseError::Status);
    CHECK(vgn::parseResponse(batteryResponse, vgn::Command::Online).error == vgn::ParseError::Command);
    invalid = batteryResponse;
    invalid.back() ^= 0x01;
    CHECK(vgn::parseResponse(invalid, vgn::Command::Battery).error == vgn::ParseError::Checksum);
    for (const std::size_t length : {std::size_t(0), std::size_t(16), std::size_t(18)}) {
        const std::vector<std::uint8_t> frame(length, 0);
        CHECK(vgn::parseResponse(frame, vgn::Command::Battery).error == vgn::ParseError::Length);
    }
}

void identityTests() {
    const auto now = MonotonicTimePoint{} + 1h;
    DeviceRegistry registry;
    const auto receiver = mouse("receiver", TransportKind::Receiver24G);
    const auto wired = mouse("wired", TransportKind::WiredUsb);
    registry.setEndpoints({receiver, wired}, now);
    CHECK(registry.snapshots(now).size() == 2); // Matching VID/brand is not identity.
    CHECK(registry.bindEndpoints({"receiver", "wired"}, "my-mouse"));
    auto snapshots = registry.snapshots(now);
    CHECK(snapshots.size() == 1);
    CHECK(snapshots[0].logicalId == "my-mouse");
    CHECK(snapshots[0].receiverPresent);
    CHECK(snapshots[0].wiredPresent);
    CHECK(snapshots[0].linkState == LinkState::Unknown); // Presence is not online.
    CHECK(!snapshots[0].battery.reportedPercent);
    CHECK(!snapshots[0].battery.charging);
    CHECK(!snapshots[0].caps.dpi);

    CHECK(registry.observe("receiver", battery(62, false), now));
    CHECK(registry.selectEndpoint("my-mouse", now)->endpointId == "receiver");
    DeviceObservation retry;
    retry.linkState = LinkState::Reconnecting;
    CHECK(registry.observe("wired", retry, now + 1s));
    CHECK(registry.selectEndpoint("my-mouse", now + 1s)->endpointId == "receiver");
    CHECK(registry.observe("wired", battery(63, true), now + 2s));
    CHECK(registry.selectEndpoint("my-mouse", now + 2s)->endpointId == "wired");

    registry.setEndpoints({receiver}, now + 3s);
    snapshots = registry.snapshots(now + 3s);
    CHECK(snapshots.size() == 1);
    CHECK(!snapshots[0].wiredPresent);
    CHECK(registry.selectEndpoint("my-mouse", now + 3s)->endpointId == "receiver");
    CHECK(snapshots[0].battery.reportedPercent == 63);
    CHECK(snapshots[0].stale); // The latest sample came from the unplugged cable.
    CHECK(!registry.observe("wired", battery(10, false), now + 4s));
    CHECK(registry.observe("receiver", battery(63, false), now + 4s));
    CHECK(!registry.snapshots(now + 4s)[0].stale);
    CHECK(registry.snapshots(now + 4s)[0].battery.charging == false);

    DeviceRegistry serials;
    serials.setEndpoints(
        {mouse("A", TransportKind::Receiver24G, "ONE"), mouse("B", TransportKind::WiredUsb, "TWO")}, now);
    CHECK(serials.snapshots(now).size() == 2);
    DeviceRegistry matchingSerials;
    matchingSerials.setEndpoints(
        {mouse("A", TransportKind::Receiver24G, "ONE"), mouse("B", TransportKind::WiredUsb, "ONE")}, now);
    CHECK(matchingSerials.snapshots(now).size() == 1);
    DeviceRegistry containers;
    containers.setEndpoints({mouse("A", TransportKind::Receiver24G, {}, "{ABC-123}"),
                             mouse("B", TransportKind::WiredUsb, {}, "abc-123")},
                            now);
    CHECK(containers.snapshots(now).size() == 1);
    DeviceRegistry conflicts;
    conflicts.setEndpoints({mouse("A", TransportKind::Receiver24G, "ONE", "same"),
                            mouse("B", TransportKind::WiredUsb, "TWO", "same")},
                           now);
    CHECK(conflicts.snapshots(now).size() == 2);

    Endpoint keyboard;
    keyboard.endpointId = "keyboard";
    keyboard.kind = DeviceKind::Keyboard;
    registry.setEndpoints({receiver, keyboard}, now + 4s);
    CHECK(!registry.bindEndpoints({"receiver", "keyboard"}, "bad-pair"));
    CHECK(registry.snapshots(now + 4s).size() == 2);
    CHECK(!registry.bindEndpoints({"missing"}, "bad-pair"));
    CHECK(isVgnF2MasterPlus(receiver));
    auto unknownVgn = receiver;
    unknownVgn.pid = 0x1234;
    CHECK(!isVgnF2MasterPlus(unknownVgn));
    keyboard.vid = 0x1A2C;
    keyboard.pid = 0x7FFF;
    CHECK(isLangtuL98Candidate(keyboard));
}

void stateTests() {
    const auto now = MonotonicTimePoint{} + 1h;
    DeviceRegistry registry;
    const auto receiver = mouse("receiver", TransportKind::Receiver24G);
    registry.setEndpoints({receiver}, now);
    const auto id = registry.snapshots(now)[0].logicalId;
    auto reading = battery(72, false);
    // 墙上采样时间只用于展示；调度顺序来自显式单调时间。
    reading.sampledAt = TimePoint{} + 1h;
    CHECK(registry.observe("receiver", reading, now));
    auto snapshot = registry.snapshots(now + 5s)[0];
    CHECK(snapshot.linkState == LinkState::Online);
    CHECK(!snapshot.stale);
    CHECK(snapshot.caps.battery);
    CHECK(snapshot.lastUpdated && *snapshot.lastUpdated == TimePoint{} + 1h);
    snapshot = registry.snapshots(now + 16s)[0];
    CHECK(snapshot.linkState == LinkState::Reconnecting);
    CHECK(snapshot.stale);
    CHECK(snapshot.battery.reportedPercent == 72);

    DeviceObservation offline;
    offline.linkState = LinkState::Offline;
    offline.statusDetail = "Receiver reports device offline";
    CHECK(registry.observe("receiver", offline, now + 20s));
    snapshot = registry.snapshots(now + 20s)[0];
    CHECK(snapshot.receiverPresent);
    CHECK(snapshot.linkState == LinkState::Offline);
    CHECK(snapshot.stale);
    CHECK(snapshot.battery.reportedPercent == 72);
    CHECK(!registry.observe("receiver", battery(50, true), now + 10s));
    CHECK(!registry.observe("receiver", battery(101, false), now + 21s));
    CHECK(registry.snapshots(now + 21s)[0].battery.reportedPercent == 72);
    registry.setEndpoints({}, now + 21s);
    snapshot = registry.snapshots(now + 21s)[0];
    CHECK(!snapshot.receiverPresent);
    CHECK(snapshot.linkState == LinkState::TransportLost);
    CHECK(snapshot.stale);
    CHECK(snapshot.battery.reportedPercent == 72);
    CHECK(!registry.selectEndpoint(id, now + 21s));
    CHECK(!registry.observe("receiver", battery(0, false), now + 22s));

    registry.setEndpoints({receiver}, now + 22s);
    CHECK(registry.snapshots(now + 22s)[0].linkState == LinkState::Unknown);
    CHECK(registry.observe("receiver", battery(0, false), now + 23s));
    snapshot = registry.snapshots(now + 23s)[0];
    CHECK(snapshot.linkState == LinkState::Online);
    CHECK(snapshot.battery.reportedPercent == 0); // Real zero remains valid.
    CHECK(snapshot.battery.charging == false);
    CHECK(!snapshot.stale);

    // 系统时间回拨：墙上采样时间可以倒退，单调顺序继续推进，新样本不能被当成迟到结果丢弃。
    auto rollback = battery(64, false);
    rollback.sampledAt = TimePoint{} + 2h;
    CHECK(registry.observe("receiver", rollback, now + 30s));
    rollback.sampledAt = TimePoint{} + 1h; // Wall clock moved backwards by one hour.
    rollback.reportedPercent = 65;
    CHECK(registry.observe("receiver", rollback, now + 31s));
    snapshot = registry.snapshots(now + 31s)[0];
    CHECK(snapshot.battery.reportedPercent == 65 && !snapshot.stale);
}

void reusedIdentityTests() {
    const auto now = MonotonicTimePoint{} + 1h;
    const auto active = [](const std::vector<DeviceSnapshot>& snapshots) {
        const auto found = std::find_if(snapshots.begin(), snapshots.end(),
                                        [](const auto& item) { return !item.endpoints.empty(); });
        CHECK(found != snapshots.end());
        return *found;
    };

    // 同一系统路径先后属于不同序列号，第三次碰撞也必须生成独立记录。
    DeviceRegistry collisions;
    auto endpoint = mouse("reused-path", TransportKind::Receiver24G, "A", "same-container");
    collisions.setEndpoints({endpoint}, now);
    const auto aId = active(collisions.snapshots(now)).logicalId;
    CHECK(collisions.observe(endpoint.endpointId, battery(41, false), now));
    endpoint.serial = "B";
    collisions.setEndpoints({endpoint}, now + 1s);
    const auto bId = active(collisions.snapshots(now + 1s)).logicalId;
    CHECK(aId != bId);
    CHECK(collisions.observe(endpoint.endpointId, battery(52, false), now + 1s));
    endpoint.serial = "C";
    collisions.setEndpoints({endpoint}, now + 2s);
    const auto c = active(collisions.snapshots(now + 2s));
    CHECK(collisions.snapshots(now + 2s).size() == 3);
    CHECK(c.logicalId != aId && c.logicalId != bId);
    CHECK(!c.battery.reportedPercent);
    CHECK(c.linkState == LinkState::Unknown);
    CHECK(c.stale);
    CHECK(collisions.observe(endpoint.endpointId, battery(63, true), now + 2s));
    endpoint.serial = "B";
    collisions.setEndpoints({endpoint}, now + 3s);
    const auto b = active(collisions.snapshots(now + 3s));
    CHECK(b.logicalId == bId);
    CHECK(b.battery.reportedPercent == 52);
    CHECK(b.linkState == LinkState::Unknown); // 旧电量不能证明新会话在线。
    CHECK(b.stale);
    endpoint.serial = "A";
    collisions.setEndpoints({endpoint}, now + 4s);
    const auto a = active(collisions.snapshots(now + 4s));
    CHECK(a.logicalId == aId);
    CHECK(a.battery.reportedPercent == 41);
    CHECK(a.stale);

    for (const bool sharedGroup : {false, true}) {
        DeviceRegistry reused;
        auto original = mouse("same-path", TransportKind::Receiver24G, {}, "original-container");
        if (sharedGroup) {
            original.groupKey = "old-group";
        }
        reused.setEndpoints({original}, now);
        const auto oldId = active(reused.snapshots(now)).logicalId;
        CHECK(reused.observe(original.endpointId, battery(72, false), now));
        auto replacement = original;
        replacement.containerId = "replacement-container";
        reused.setEndpoints({replacement}, now + 1s);
        const auto fresh = active(reused.snapshots(now + 1s));
        CHECK(reused.snapshots(now + 1s).size() == 2);
        CHECK(fresh.logicalId != oldId);
        CHECK(!fresh.battery.reportedPercent);
        CHECK(fresh.linkState == LinkState::Unknown);
        CHECK(fresh.stale);
        const auto history = reused.snapshots(now + 1s);
        const auto old = std::find_if(history.begin(), history.end(),
                                      [&](const auto& item) { return item.logicalId == oldId; });
        CHECK(old != history.end());
        CHECK(old->linkState == LinkState::TransportLost);
        CHECK(old->battery.reportedPercent == 72);
        CHECK(old->stale);
    }

    // 相同序列号和 VID 可跨 Container 变化维持身份，避免破坏合法连接切换。
    DeviceRegistry serialMatch;
    auto serialEndpoint = mouse("serial-path", TransportKind::Receiver24G, "reliable", "old-container");
    serialMatch.setEndpoints({serialEndpoint}, now);
    const auto serialId = active(serialMatch.snapshots(now)).logicalId;
    serialEndpoint.containerId = "new-container";
    serialMatch.setEndpoints({serialEndpoint}, now + 1s);
    CHECK(serialMatch.snapshots(now + 1s).size() == 1);
    CHECK(active(serialMatch.snapshots(now + 1s)).logicalId == serialId);
    serialEndpoint.containerId = "third-container";
    serialEndpoint.vid = 0x1234;
    serialMatch.setEndpoints({serialEndpoint}, now + 2s);
    CHECK(serialMatch.snapshots(now + 2s).size() == 2);
    CHECK(active(serialMatch.snapshots(now + 2s)).logicalId != serialId);

    DeviceRegistry boundSerial;
    auto oldSerial = mouse("bound-serial", TransportKind::Receiver24G, "original", "shared");
    boundSerial.setEndpoints({oldSerial}, now);
    CHECK(boundSerial.bindEndpoints({oldSerial.endpointId}, "explicit-serial"));
    auto newSerial = oldSerial;
    newSerial.serial = "replacement";
    boundSerial.setEndpoints({newSerial}, now + 1s);
    CHECK(boundSerial.snapshots(now + 1s).size() == 2);
    CHECK(active(boundSerial.snapshots(now + 1s)).logicalId != "explicit-serial");

    // 显式关联记住每条路径自己的身份；有线和接收器的不同 Container 本身不冲突。
    DeviceRegistry bound;
    auto receiver = mouse("bound-rx", TransportKind::Receiver24G, {}, "receiver-container");
    auto wired = mouse("bound-wire", TransportKind::WiredUsb, {}, "wired-container");
    bound.setEndpoints({receiver, wired}, now);
    CHECK(bound.bindEndpoints({receiver.endpointId, wired.endpointId}, "user-bound-mouse"));
    CHECK(bound.observe(receiver.endpointId, battery(84, false), now));
    bound.setEndpoints({receiver, wired}, now + 1s);
    CHECK(bound.snapshots(now + 1s).size() == 1);
    CHECK(active(bound.snapshots(now + 1s)).logicalId == "user-bound-mouse");
    CHECK(active(bound.snapshots(now + 1s)).endpoints.size() == 2);
    auto replacedReceiver = receiver;
    replacedReceiver.containerId = "another-receiver";
    bound.setEndpoints({replacedReceiver, wired}, now + 2s);
    const auto afterReplacement = bound.snapshots(now + 2s);
    CHECK(afterReplacement.size() == 2);
    const auto replacement =
        std::find_if(afterReplacement.begin(), afterReplacement.end(),
                     [&](const auto& item) { return item.logicalId != "user-bound-mouse"; });
    CHECK(replacement != afterReplacement.end());
    CHECK(!replacement->battery.reportedPercent);
    CHECK(replacement->linkState == LinkState::Unknown);
    CHECK(replacement->endpoints.size() == 1);
    const auto original =
        std::find_if(afterReplacement.begin(), afterReplacement.end(),
                     [&](const auto& item) { return item.logicalId == "user-bound-mouse"; });
    CHECK(original != afterReplacement.end());
    CHECK(original->endpoints.size() == 1);
    CHECK(original->endpoints[0].endpointId == wired.endpointId);
    CHECK(original->battery.reportedPercent == 84);
    CHECK(original->stale);
    bound.setEndpoints({receiver, wired}, now + 3s);
    const auto returned = bound.snapshots(now + 3s);
    const auto restored = std::find_if(returned.begin(), returned.end(), [&](const auto& item) {
        return item.logicalId == "user-bound-mouse";
    });
    CHECK(restored != returned.end());
    CHECK(restored->endpoints.size() == 2);
    CHECK(restored->battery.reportedPercent == 84);
    CHECK(restored->stale);
}

void liveControlTests() {
    // A held drag produces hardware writes before release, at a bounded rate.
    // This fake clock represents rapid UI events and slower asynchronous I/O.
    LiveValueControl volume(60);
    volume.selectTarget(L"headset-a");
    std::vector<LiveValueControl::Request> writes;
    std::optional<LiveValueControl::Request> running;
    std::int64_t completedAt = 0;
    for (std::int64_t now = 0; now <= 600; now += 10) {
        if (running && now >= completedAt) {
            CHECK(volume.complete(running->token));
            running.reset();
        }
        if (now <= 500)
            CHECK(volume.request(L"headset-a", static_cast<int>(now / 5)));
        if (now == 500)
            volume.flush();
        if (const auto next = volume.take(now)) {
            CHECK(!running);
            running = next;
            completedAt = now + 20;
            writes.push_back(*next);
        }
    }
    CHECK(writes.size() >= 8 && writes.size() <= 10);
    CHECK(writes.front().percent == 0);
    CHECK(writes.back().percent == 100);
    CHECK(!volume.active());

    // A slow DDC operation cannot build a queue of intermediate positions.
    LiveValueControl brightness(200);
    brightness.selectTarget(L"monitor-a");
    CHECK(brightness.request(L"monitor-a", 40));
    auto first = brightness.take(0);
    CHECK(first && first->percent == 40);
    for (int value = 41; value <= 70; ++value) {
        CHECK(brightness.request(L"monitor-a", value));
        CHECK(!brightness.take(500));
    }
    brightness.flush();
    CHECK(!brightness.take(501));
    CHECK(brightness.complete(first->token));
    auto final = brightness.take(502);
    CHECK(final && final->percent == 70);
    CHECK(brightness.complete(final->token));
    CHECK(!brightness.take(503));
    CHECK(!brightness.active());

    // A release flushes the final value before the next normal throttle slot.
    CHECK(brightness.request(L"monitor-a", 71));
    CHECK(!brightness.take(510));
    brightness.flush();
    final = brightness.take(510);
    CHECK(final && final->percent == 71);

    // Target changes discard the unsent old value. The already dispatched
    // command retains its ID and cannot complete a later command's token.
    CHECK(brightness.request(L"monitor-a", 72));
    const auto oldGeneration = final->generation;
    CHECK(brightness.selectTarget(L"monitor-b"));
    CHECK(!brightness.hasPending());
    CHECK(!brightness.request(L"monitor-a", 90));
    CHECK(brightness.request(L"monitor-b", 25));
    CHECK(!brightness.take(900));
    CHECK(brightness.complete(final->token));
    const auto other = brightness.take(900);
    CHECK(other && other->target == L"monitor-b" && other->percent == 25);
    CHECK(other->generation != oldGeneration);
    CHECK(!brightness.complete(final->token));
    CHECK(brightness.active());
    CHECK(brightness.complete(other->token));
    brightness.selectTarget({});
    CHECK(!brightness.request({}, 30));
    CHECK(!brightness.take(1000));
    CHECK(!brightness.active());
}

// A failing private read must be logged once, not on every poll; a changed
// reason is a new event, and recovery closes it.
void privateErrorTests() {
    core::PrivateErrorTracker tracker;
    const std::wstring path = L"hid\\path-a";
    CHECK(tracker.update(path, "timeout") == core::PrivateErrorTracker::Change::Failed);
    CHECK(tracker.update(path, "timeout") == core::PrivateErrorTracker::Change::None);
    CHECK(tracker.failing(path) && tracker.size() == 1);
    CHECK(tracker.update(path, "no reply") == core::PrivateErrorTracker::Change::Failed);
    CHECK(tracker.update(path, "") == core::PrivateErrorTracker::Change::Recovered);
    CHECK(!tracker.failing(path) && tracker.size() == 0);
    CHECK(tracker.update(path, "") == core::PrivateErrorTracker::Change::None);

    const std::wstring other = L"hid\\path-b";
    tracker.update(path, "timeout");
    tracker.update(other, "timeout");
    CHECK(tracker.size() == 2);
    // 设备移除后丢弃记录，重新接入从干净状态开始。
    tracker.retain({other});
    CHECK(tracker.size() == 1 && tracker.failing(other) && !tracker.failing(path));
    CHECK(tracker.update(path, "timeout") == core::PrivateErrorTracker::Change::Failed);
}

} // namespace

int main() {
    protocolTests();
    identityTests();
    stateTests();
    reusedIdentityTests();
    liveControlTests();
    privateErrorTests();
    std::cout << "Core tests passed (" << checks << " checks).\n";
}
