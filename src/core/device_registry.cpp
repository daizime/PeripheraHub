#include "device_registry.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>

namespace peripheral {
namespace {

std::string normalizedContainer(const std::string& value) {
    std::string result;
    for (const unsigned char c : value) {
        if (c != '{' && c != '}') {
            result.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return result;
}

bool serialConflict(const Endpoint& a, const Endpoint& b) {
    return !a.serial.empty() && !b.serial.empty() && a.serial != b.serial;
}

bool sameSerialIdentity(const Endpoint& a, const Endpoint& b) {
    return !a.serial.empty() && a.serial == b.serial && a.vid == b.vid;
}

bool compatiblePathIdentity(const Endpoint& known, const Endpoint& endpoint) {
    if (known.kind != endpoint.kind || serialConflict(known, endpoint)) {
        return false;
    }
    // 路径会被系统复用；Container 冲突时，只有同 VID、同序列号可继续沿用身份。
    return known.containerId.empty() || endpoint.containerId.empty() ||
           normalizedContainer(known.containerId) == normalizedContainer(endpoint.containerId) ||
           sameSerialIdentity(known, endpoint);
}

bool samePhysicalIdentity(const Endpoint& a, const Endpoint& b) {
    if (a.kind != b.kind || serialConflict(a, b)) {
        return false;
    }
    if (!a.containerId.empty() && !b.containerId.empty()) {
        // 旧 groupKey 也不能越过已知的 Container 冲突，把替换设备合并回来。
        return normalizedContainer(a.containerId) == normalizedContainer(b.containerId) ||
               sameSerialIdentity(a, b);
    }
    if (sameSerialIdentity(a, b)) {
        return true;
    }
    return !a.groupKey.empty() && a.groupKey == b.groupKey;
}

std::string initialId(const Endpoint& endpoint) {
    const auto suffix = ":" + std::to_string(static_cast<int>(endpoint.kind));
    if (!endpoint.containerId.empty()) {
        return "container:" + normalizedContainer(endpoint.containerId) + suffix;
    }
    if (!endpoint.serial.empty()) {
        std::ostringstream value;
        value << "serial:" << std::hex << endpoint.vid << ':' << endpoint.serial << suffix;
        return value.str();
    }
    if (!endpoint.groupKey.empty()) {
        return "group:" + endpoint.groupKey + suffix;
    }
    return "endpoint:" + endpoint.endpointId;
}

bool isFresh(MonotonicTimePoint when, MonotonicTimePoint now, std::chrono::milliseconds freshness) {
    // Monotonic time cannot go backwards, so when > now only happens if a
    // caller used a different clock; treat such values as unusable evidence.
    return freshness >= std::chrono::milliseconds::zero() && when <= now && now - when <= freshness;
}

void addCapabilities(DeviceCapabilities& to, const DeviceCapabilities& from) {
    to.battery |= from.battery;
    to.charging |= from.charging;
    to.brightness |= from.brightness;
    to.volume |= from.volume;
    to.mute |= from.mute;
    to.dpi |= from.dpi;
    to.pollingRate |= from.pollingRate;
    to.rgb |= from.rgb;
    to.keyMapping |= from.keyMapping;
    to.eq |= from.eq;
}

int transportRank(TransportKind transport) {
    switch (transport) {
    case TransportKind::WiredUsb:
        return 30;
    case TransportKind::Receiver24G:
        return 20;
    case TransportKind::DisplayPort:
        return 10;
    default:
        return 0;
    }
}

} // namespace

bool isVgnF2MasterPlus(const Endpoint& endpoint) {
    return endpoint.vid == 0x3554 && (endpoint.pid == 0xFB56 || endpoint.pid == 0xFB57);
}

bool isLangtuL98Candidate(const Endpoint& endpoint) {
    return endpoint.vid == 0x1A2C && (endpoint.pid == 0x7FFF || endpoint.pid == 0x484A);
}

std::string DeviceRegistry::findOwner(const Endpoint& endpoint) const {
    const auto compatibleOwner = [&](const std::string& id) {
        const auto record = records_.find(id);
        if (record == records_.end()) {
            return false;
        }
        const auto known = record->second.knownEndpoints.find(endpoint.endpointId);
        return known != record->second.knownEndpoints.end() &&
               compatiblePathIdentity(known->second, endpoint);
    };
    const auto binding = explicitBindings_.find(endpoint.endpointId);
    // 显式关联只跳过跨传输的自动匹配，不能无视同一路径后来换了实体的证据。
    if (binding != explicitBindings_.end() && compatibleOwner(binding->second)) {
        return binding->second;
    }
    const auto previous = owners_.find(endpoint.endpointId);
    if (previous != owners_.end() && compatibleOwner(previous->second)) {
        return previous->second;
    }
    for (const auto& [id, record] : records_) {
        if (record.kind != endpoint.kind) {
            continue;
        }
        // A blank-serial interface must not bridge two distinct serials.
        const bool conflict =
            std::any_of(record.knownEndpoints.begin(), record.knownEndpoints.end(),
                        [&](const auto& item) { return serialConflict(item.second, endpoint); });
        if (conflict) {
            continue;
        }
        for (const auto& [unused, known] : record.knownEndpoints) {
            if (samePhysicalIdentity(known, endpoint)) {
                return id;
            }
        }
    }
    return {};
}

void DeviceRegistry::setEndpoints(std::vector<Endpoint> endpoints, MonotonicTimePoint now) {
    (void)now; // Enumeration is evidence of interface presence, not a successful query.
    std::set<std::string> previouslyPresent;
    for (auto& [id, record] : records_) {
        for (const auto& [endpointId, endpoint] : record.activeEndpoints) {
            previouslyPresent.insert(endpointId);
        }
        record.activeEndpoints.clear();
    }
    std::sort(endpoints.begin(), endpoints.end(),
              [](const auto& a, const auto& b) { return a.endpointId < b.endpointId; });
    for (auto& endpoint : endpoints) {
        if (endpoint.endpointId.empty()) {
            continue;
        }
        const auto previousOwner = owners_.find(endpoint.endpointId);
        auto owner = findOwner(endpoint);
        if (owner.empty()) {
            owner = initialId(endpoint);
            // Conflicting identities must not accidentally collide via a reused
            // container or group identifier.
            if (records_.count(owner) != 0) {
                owner += ":endpoint:" + endpoint.endpointId;
                const auto prefix = owner;
                for (std::size_t suffix = 2; records_.count(owner) != 0; ++suffix) {
                    owner = prefix + ':' + std::to_string(suffix);
                }
            }
        }
        auto& record = records_[owner];
        record.logicalId = owner;
        record.kind = endpoint.kind;
        if (!endpoint.displayName.empty()) {
            record.name = endpoint.displayName;
        }
        record.knownEndpoints[endpoint.endpointId] = endpoint;
        record.activeEndpoints[endpoint.endpointId] = endpoint;
        if (previouslyPresent.count(endpoint.endpointId) == 0 ||
            (previousOwner != owners_.end() && previousOwner->second != owner)) {
            // Reappearance starts a new transport session; an old response is
            // still a battery cache, but cannot prove the new session is online.
            record.observations.erase(endpoint.endpointId);
        }
        owners_[endpoint.endpointId] = owner;
    }
}

void DeviceRegistry::mergeRecord(const std::string& from, const std::string& to) {
    if (from == to) {
        return;
    }
    const auto source = records_.find(from);
    if (source == records_.end()) {
        return;
    }
    auto& target = records_[to];
    target.logicalId = to;
    if (target.name.empty()) {
        target.name = source->second.name;
    }
    if (target.kind == DeviceKind::Unknown) {
        target.kind = source->second.kind;
    }
    for (const auto& [id, endpoint] : source->second.knownEndpoints) {
        target.knownEndpoints[id] = endpoint;
        owners_[id] = to;
    }
    for (const auto& [id, endpoint] : source->second.activeEndpoints) {
        target.activeEndpoints[id] = endpoint;
    }
    for (const auto& [id, observation] : source->second.observations) {
        const auto existing = target.observations.find(id);
        if (existing == target.observations.end() || existing->second.when < observation.when) {
            target.observations[id] = observation;
        }
    }
    if (source->second.batteryWhen &&
        (!target.batteryWhen || *target.batteryWhen < *source->second.batteryWhen)) {
        target.battery = source->second.battery;
        target.batteryWhen = source->second.batteryWhen;
        target.batterySampledAt = source->second.batterySampledAt;
        target.batteryEndpointId = source->second.batteryEndpointId;
    }
    for (auto& [id, binding] : explicitBindings_) {
        if (binding == from) {
            binding = to;
        }
    }
    records_.erase(source);
}

bool DeviceRegistry::bindEndpoints(const std::vector<std::string>& endpointIds,
                                   const std::string& logicalId) {
    if (endpointIds.empty() || logicalId.empty()) {
        return false;
    }
    DeviceKind kind = DeviceKind::Unknown;
    const auto target = records_.find(logicalId);
    if (target != records_.end()) {
        kind = target->second.kind;
    }
    for (const auto& id : endpointIds) {
        const auto owner = owners_.find(id);
        if (id.empty() || owner == owners_.end()) {
            return false;
        }
        const auto candidateKind = records_.at(owner->second).kind;
        if (kind != DeviceKind::Unknown && candidateKind != DeviceKind::Unknown && kind != candidateKind) {
            return false;
        }
        if (candidateKind != DeviceKind::Unknown) {
            kind = candidateKind;
        }
    }
    records_[logicalId].logicalId = logicalId;
    for (const auto& id : endpointIds) {
        // mergeRecord may change the owner of later endpoints in this list.
        mergeRecord(owners_.at(id), logicalId);
        explicitBindings_[id] = logicalId;
    }
    return true;
}

bool DeviceRegistry::observe(const std::string& endpointId, const DeviceObservation& observation,
                             MonotonicTimePoint now) {
    const auto owner = owners_.find(endpointId);
    if (owner == owners_.end() || (observation.reportedPercent && (*observation.reportedPercent < 0 ||
                                                                   *observation.reportedPercent > 100))) {
        return false;
    }
    auto& record = records_.at(owner->second);
    if (record.activeEndpoints.count(endpointId) == 0) {
        return false; // A delayed result from a removed interface is not current evidence.
    }
    const auto previous = record.observations.find(endpointId);
    if (previous != record.observations.end() && previous->second.when > now) {
        return false; // Late completion cannot overwrite a more recent query.
    }
    record.observations[endpointId] = {observation, now};
    if (observation.linkState == LinkState::Online && (observation.reportedPercent || observation.charging) &&
        (!record.batteryWhen || *record.batteryWhen <= now)) {
        // Each response is a sample: missing fields remain unknown rather than
        // borrowing a potentially unrelated charging flag from an older sample.
        record.battery = {observation.reportedPercent, observation.charging};
        record.batteryWhen = now;
        record.batterySampledAt = observation.sampledAt;
        record.batteryEndpointId = endpointId;
    }
    return true;
}

std::vector<DeviceSnapshot> DeviceRegistry::snapshots(MonotonicTimePoint now,
                                                      std::chrono::milliseconds freshness) const {
    std::vector<DeviceSnapshot> result;
    for (const auto& [id, record] : records_) {
        DeviceSnapshot snapshot;
        snapshot.logicalId = id;
        snapshot.name = record.name;
        snapshot.kind = record.kind;
        snapshot.battery = record.battery;
        snapshot.lastUpdated = record.batterySampledAt;
        const ObservationRecord* newest = nullptr;
        bool online = false;
        bool offline = false;
        for (const auto& [endpointId, endpoint] : record.activeEndpoints) {
            snapshot.endpoints.push_back(endpoint);
            snapshot.receiverPresent |= endpoint.transport == TransportKind::Receiver24G;
            snapshot.wiredPresent |= endpoint.transport == TransportKind::WiredUsb;
            const auto observation = record.observations.find(endpointId);
            if (observation == record.observations.end()) {
                continue;
            }
            if (observation->second.observation.caps) {
                addCapabilities(snapshot.caps, *observation->second.observation.caps);
            }
            if (!newest || newest->when < observation->second.when) {
                newest = &observation->second;
            }
            if (isFresh(observation->second.when, now, freshness)) {
                online |= observation->second.observation.linkState == LinkState::Online;
                offline |= observation->second.observation.linkState == LinkState::Offline;
            }
        }
        if (record.activeEndpoints.empty()) {
            snapshot.linkState = LinkState::TransportLost;
            snapshot.statusDetail = "All known interfaces are absent";
        } else if (online) {
            snapshot.linkState = LinkState::Online;
        } else if (offline) {
            snapshot.linkState = LinkState::Offline;
        } else if (newest) {
            snapshot.linkState = isFresh(newest->when, now, freshness) ? newest->observation.linkState
                                                                       : LinkState::Reconnecting;
        }
        if (!record.activeEndpoints.empty() && newest) {
            snapshot.statusDetail = newest->observation.statusDetail;
        }
        const auto batterySource = record.observations.find(record.batteryEndpointId);
        const bool sourceOnline = record.activeEndpoints.count(record.batteryEndpointId) != 0 &&
                                  batterySource != record.observations.end() &&
                                  batterySource->second.observation.linkState == LinkState::Online &&
                                  isFresh(batterySource->second.when, now, freshness);
        snapshot.stale = !record.batteryWhen || !sourceOnline ||
                         !isFresh(*record.batteryWhen, now, freshness) ||
                         snapshot.linkState != LinkState::Online;
        result.push_back(std::move(snapshot));
    }
    return result;
}

std::optional<Endpoint> DeviceRegistry::selectEndpoint(const std::string& logicalId, MonotonicTimePoint now,
                                                       std::chrono::milliseconds freshness) const {
    const auto record = records_.find(logicalId);
    if (record == records_.end()) {
        return std::nullopt;
    }
    std::optional<Endpoint> selected;
    int bestScore = -10000;
    for (const auto& [id, endpoint] : record->second.activeEndpoints) {
        int score = transportRank(endpoint.transport);
        // Vendor-defined collections outrank ordinary keyboard/mouse input.
        if (endpoint.usagePage >= 0xFF00) {
            score += 5;
        }
        const auto observation = record->second.observations.find(id);
        if (observation != record->second.observations.end() &&
            isFresh(observation->second.when, now, freshness)) {
            switch (observation->second.observation.linkState) {
            case LinkState::Online:
                score += 1000;
                break;
            case LinkState::Offline:
                score -= 200;
                break;
            case LinkState::Reconnecting:
            case LinkState::TransportLost:
                score -= 100;
                break;
            default:
                break;
            }
        }
        if (!selected || score > bestScore) {
            selected = endpoint;
            bestScore = score;
        }
    }
    return selected;
}

} // namespace peripheral
