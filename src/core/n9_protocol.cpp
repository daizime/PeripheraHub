#include "n9_protocol.h"
#include <algorithm>

namespace peripheral::n9 {
namespace {
bool matches(std::span<const std::uint8_t> report, std::uint8_t command) {
    return report.size() == kReportSize && report[0] == kReportId && report[1] == command;
}
int bigEndian(std::span<const std::uint8_t> report, int offset) {
    return (static_cast<int>(report[offset]) << 8) | report[offset + 1];
}
} // namespace
Report statusQuery() {
    Report report{};
    report[0] = kReportId;
    report[1] = 0x65;
    report[2] = 1; // Headset target, not the dongle.
    return report;
}
std::optional<Report> peqQuery(int point) {
    if (point < 0 || point >= 10)
        return std::nullopt;
    auto report = statusQuery();
    report[1] = 0x69;
    report[3] = static_cast<std::uint8_t>(point);
    return report;
}
std::optional<Status> parseStatus(std::span<const std::uint8_t> report) {
    if (!matches(report, 0x65) || report[2] > 100 || report[3] < 2 || report[3] > 4 ||
        report[4] > 2 || report[5] > 1 || report[6] > 1 || report[8] > 1 || report[18] > 1)
        return std::nullopt;
    Status value;
    value.batteryPercent = report[2];
    value.batteryKind = static_cast<BatteryKind>(report[3]);
    value.eqMode = report[4];
    value.surround = report[5] == 1;
    value.spatialSound = report[6] == 1;
    value.autoPowerOffCode = report[7];
    value.sidetone = report[8] == 1;
    value.sidetoneVolume = report[9];
    std::copy_n(report.begin() + 10, 4, value.dongleVersion.begin());
    std::copy_n(report.begin() + 14, 4, value.headsetVersion.begin());
    value.rgb = report[18] == 1;
    return value;
}
std::optional<PeqPoint> parsePeq(std::span<const std::uint8_t> report) {
    if (!matches(report, 0x69) || report[2] != 1 || report[9] > 5 || report[10] > 1)
        return std::nullopt;
    PeqPoint value;
    value.frequencyHz = bigEndian(report, 3);
    const int gain = bigEndian(report, 5);
    value.gainTenthsDb = gain >= 32768 ? gain - 65536 : gain;
    value.qTenths = bigEndian(report, 7);
    value.filterType = report[9];
    value.enabled = report[10] == 1;
    if (value.frequencyHz == 0 || value.qTenths == 0)
        return std::nullopt;
    return value;
}
std::optional<Report> peqWriteQuery(int point, const PeqPoint& value) {
    if (point < 0 || point >= 10 || value.frequencyHz < 1 || value.frequencyHz > 20000 ||
        value.gainTenthsDb < -32768 || value.gainTenthsDb > 32767 || value.qTenths < 5 ||
        value.qTenths > 50 || value.filterType < 0 || value.filterType > 5)
        return std::nullopt;
    Report report{};
    report[0] = kReportId;
    report[1] = kPeqWriteCommand;
    report[2] = 1; // The only bank observed on this receiver.
    report[3] = static_cast<std::uint8_t>(point);
    const auto frequency = static_cast<std::uint16_t>(value.frequencyHz);
    const auto gain = static_cast<std::uint16_t>(value.gainTenthsDb);
    const auto q = static_cast<std::uint16_t>(value.qTenths);
    report[4] = static_cast<std::uint8_t>(frequency >> 8);
    report[5] = static_cast<std::uint8_t>(frequency & 0xff);
    report[6] = static_cast<std::uint8_t>(gain >> 8);
    report[7] = static_cast<std::uint8_t>(gain & 0xff);
    report[8] = static_cast<std::uint8_t>(q >> 8);
    report[9] = static_cast<std::uint8_t>(q & 0xff);
    report[10] = static_cast<std::uint8_t>(value.filterType);
    report[11] = value.enabled ? 1 : 0;
    report[12] = kMaskAll;
    return report;
}
std::optional<Report> eqModeWriteQuery(int mode) {
    if (mode < 0 || mode > 2)
        return std::nullopt;
    Report report{};
    report[0] = kReportId;
    report[1] = kEqModeCommand;
    report[2] = 1; // Write selector; read uses 0.
    report[3] = static_cast<std::uint8_t>(mode);
    return report;
}
bool validProfileField(std::string_view key, int value) {
    if (key == "n9_eq_mode") return value >= 0 && value <= 2;
    if (key == "n9_sidetone" || key == "n9_surround" || key == "n9_spatial" || key == "n9_rgb")
        return value == 0 || value == 1;
    if (key == "n9_autooff" || key == "n9_sidetone_volume") return value >= 0 && value <= 255;
    if (key.size() < 10 || key.substr(0, 7) != "n9_peq_" || key[7] < '0' || key[7] > '9')
        return false;
    const auto suffix = key.substr(8);
    if (suffix == "_hz" || suffix == "_q10") return value > 0 && value <= 65535;
    if (suffix == "_gain10") return value >= -32768 && value <= 32767;
    if (suffix == "_type") return value >= 0 && value <= 5;
    if (suffix == "_enabled") return value == 0 || value == 1;
    return false;
}
} // namespace peripheral::n9
