#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace peripheral::vgn {

constexpr std::uint8_t kReportId = 0x08;
constexpr std::size_t kPayloadSize = 16;
constexpr std::size_t kReportSize = kPayloadSize + 1;
using Report = std::array<std::uint8_t, kReportSize>;

enum class Command : std::uint8_t { Online = 0x03, Battery = 0x04 };
enum class ParseError { None, Length, ReportId, Command, Status, Checksum, BatteryRange };

struct Response {
    Command command = Command::Online;
    // Positive confirmation only. On the observed FB56 firmware, a zero byte
    // also occurs while the mouse works, so it cannot establish Offline.
    std::optional<bool> online;
    std::optional<int> reportedPercent;
    std::optional<bool> charging;
    std::optional<std::uint16_t> rawVoltage; // Raw protocol value; never converted to percent.
};

struct ParseResult {
    std::optional<Response> value;
    ParseError error = ParseError::None;
    explicit operator bool() const { return value.has_value(); }
};

// Returns HID report ID followed by a 16-byte payload. Only read queries exist.
Report makeQuery(Command command);

// The transport must provide the complete report, including report ID. Padded,
// truncated, unnumbered, nonzero-status and corrupt frames are rejected.
ParseResult parseResponse(const std::vector<std::uint8_t>& report, Command expected);
ParseResult parseResponse(const Report& report, Command expected);

} // namespace peripheral::vgn
