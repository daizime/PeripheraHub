#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace peripheral::n9 {
constexpr std::uint8_t kReportId = 0x55;
constexpr std::size_t kReportSize = 64;
using Report = std::array<std::uint8_t, kReportSize>;
enum class BatteryKind { Discharging = 2, Charging = 3, Full = 4 };
struct Status {
    int batteryPercent = 0;
    BatteryKind batteryKind = BatteryKind::Discharging;
    int eqMode = 0;
    bool surround = false, spatialSound = false, sidetone = false, rgb = false;
    int autoPowerOffCode = 0, sidetoneVolume = 0;
    std::array<std::uint8_t, 4> dongleVersion{}, headsetVersion{};
};
struct PeqPoint {
    int frequencyHz = 0, gainTenthsDb = 0, qTenths = 0, filterType = 0;
    bool enabled = false;
};

// Independent implementation of the N9 V2 query formats, verified against
// 373B:12D6 captures. No old N9 serial commands or configuration writes.
Report statusQuery();
std::optional<Report> peqQuery(int point);
std::optional<Status> parseStatus(std::span<const std::uint8_t> report);
std::optional<PeqPoint> parsePeq(std::span<const std::uint8_t> report);
// Persistence bounds for raw, read-only fields; these are not write ranges.
bool validProfileField(std::string_view key, int value);

// Write frames observed in the vendor web driver. See
// docs/PRIVATE_WRITE_CANDIDATES_2026-10-19.md. The reply layout omits the point
// byte, so these request offsets are deliberately one byte behind parsePeq.
constexpr std::uint8_t kPeqWriteCommand = 0x68;
constexpr std::uint8_t kEqModeCommand = 0x21;
constexpr int kMaskAll = 0x1f;
// Rejects out-of-range points and fields instead of clamping them. Q follows
// the observed device range 0.5..5.0; no reply is expected for either frame.
std::optional<Report> peqWriteQuery(int point, const PeqPoint& value);
std::optional<Report> eqModeWriteQuery(int mode);
} // namespace peripheral::n9
