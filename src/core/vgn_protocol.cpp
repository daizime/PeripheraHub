#include "vgn_protocol.h"

#include <numeric>

namespace peripheral::vgn {

Report makeQuery(Command command) {
    Report report{};
    report[0] = kReportId;
    report[1] = static_cast<std::uint8_t>(command);
    const auto sum = std::accumulate(report.begin(), report.end() - 1, 0u);
    report.back() = static_cast<std::uint8_t>(0x55u - sum);
    return report;
}

ParseResult parseResponse(const std::vector<std::uint8_t>& report, Command expected) {
    if (report.size() != kReportSize) {
        return {std::nullopt, ParseError::Length};
    }
    if (report[0] != kReportId) {
        return {std::nullopt, ParseError::ReportId};
    }
    if (report[1] != static_cast<std::uint8_t>(expected) ||
        (expected != Command::Online && expected != Command::Battery)) {
        return {std::nullopt, ParseError::Command};
    }
    if (report[2] != 0) {
        return {std::nullopt, ParseError::Status};
    }
    const auto sum = std::accumulate(report.begin(), report.end(), 0u);
    if ((sum & 0xFFu) != 0x55u) {
        return {std::nullopt, ParseError::Checksum};
    }
    Response response;
    response.command = expected;
    if (expected == Command::Online) {
        // FB56 read-only capture (2026-10-19): the receiver returned zero while
        // the user could move the mouse. Preserve uncertainty instead of turning
        // an unverified firmware-specific hint into a definitive offline state.
        if (report[6] == 1)
            response.online = true;
    } else {
        if (report[6] > 100) {
            return {std::nullopt, ParseError::BatteryRange};
        }
        response.reportedPercent = report[6];
        response.charging = report[7] == 1;
        response.rawVoltage =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(report[8]) << 8) | report[9]);
    }
    return {response, ParseError::None};
}

ParseResult parseResponse(const Report& report, Command expected) {
    return parseResponse(std::vector<std::uint8_t>(report.begin(), report.end()), expected);
}

} // namespace peripheral::vgn
