#pragma once

#include "device_model.h"

#include <map>

namespace peripheral {

// Single-owner model: callers synchronize access or use it on one thread.
class DeviceRegistry {
  public:
    // A complete enumeration replaces endpoint presence. Missing devices keep
    // their cached measurements and become TransportLost.
    void setEndpoints(std::vector<Endpoint> endpoints, MonotonicTimePoint now = MonotonicClock::now());

    // Cross-mode matching without a common container/serial requires this
    // explicit binding. Conflicting device kinds are rejected atomically.
    bool bindEndpoints(const std::vector<std::string>& endpointIds, const std::string& logicalId);

    // Unknown endpoints and invalid percentages are rejected without changing
    // cached data. Absence of a value never means zero or false.
    // now is monotonic: wall-clock corrections must not make old samples look
    // fresh or make a new sample look like a late result.
    bool observe(const std::string& endpointId, const DeviceObservation& observation,
                 MonotonicTimePoint now = MonotonicClock::now());

    std::vector<DeviceSnapshot>
    snapshots(MonotonicTimePoint now = MonotonicClock::now(),
              std::chrono::milliseconds freshness = std::chrono::seconds(15)) const;

    // Successful, fresh channels outrank untested ones. A failed wired channel
    // does not prevent using a working receiver. No I/O occurs in this method.
    std::optional<Endpoint>
    selectEndpoint(const std::string& logicalId, MonotonicTimePoint now = MonotonicClock::now(),
                   std::chrono::milliseconds freshness = std::chrono::seconds(15)) const;

  private:
    struct ObservationRecord {
        DeviceObservation observation;
        MonotonicTimePoint when;
    };
    struct Record {
        std::string logicalId;
        std::string name;
        DeviceKind kind = DeviceKind::Unknown;
        std::map<std::string, Endpoint> knownEndpoints;
        std::map<std::string, Endpoint> activeEndpoints;
        std::map<std::string, ObservationRecord> observations;
        BatteryState battery;
        std::optional<MonotonicTimePoint> batteryWhen;
        // Wall-clock sample time kept separately for display only.
        std::optional<TimePoint> batterySampledAt;
        std::string batteryEndpointId;
    };

    std::string findOwner(const Endpoint& endpoint) const;
    void mergeRecord(const std::string& from, const std::string& to);
    std::map<std::string, Record> records_;
    std::map<std::string, std::string> owners_;
    std::map<std::string, std::string> explicitBindings_;
};

} // namespace peripheral
