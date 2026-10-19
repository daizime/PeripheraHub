#include "vgn_config.h"
#include <algorithm>
#include <bit>
#include <numeric>

namespace peripheral::vgn {
namespace {
void finalize(Report& report) {
    report.back() = static_cast<std::uint8_t>(0x55u -
        std::accumulate(report.begin(), report.end() - 1, 0u));
}
bool frame(std::span<const std::uint8_t> report, int command) {
    return report.size() == kReportSize && report[0] == kReportId &&
        report[1] == command && report[2] == 0 &&
        (std::accumulate(report.begin(), report.end(), 0u) & 0xffu) == 0x55u;
}
bool knownBlock(ReadBlock block) {
    return std::find(kProfileReads.begin(), kProfileReads.end(), block) != kProfileReads.end();
}
bool writableFlashBlock(std::uint16_t address, std::size_t length) {
    if (length == 2)
        return address == kFlashReportRate || address == kFlashStageCount ||
            address == kFlashActiveStage;
    if (length != 4)
        return false;
    for (std::size_t stage = 0; stage < kFlashDpiStages; ++stage)
        if (address == kFlashDpiBase + stage * 4)
            return true;
    return false;
}
// Each set multiplier bit doubles the axis, so the factor is 2^popcount. The
// multiplier has to be chosen by exact division: a DPI that no enabled format
// represents is rejected rather than moved to a neighbouring value.
std::optional<std::pair<int, int>> encodeDpiAxis(int dpi) {
    if (dpi <= 0)
        return std::nullopt;
    for (int multiplier = 0; multiplier < 4; ++multiplier) {
        const int unit = 50 * (1 << std::popcount(static_cast<unsigned>(multiplier)));
        if (dpi % unit != 0)
            continue;
        const int value = dpi / unit - 1;
        if (value >= 0 && value <= 1023)
            return std::make_pair(value, multiplier);
    }
    return std::nullopt;
}
} // namespace
Report identityQuery() {
    Report report{8,1,0,0,0,8,0xa6,0x31,0x7b,0xd4};
    finalize(report);
    return report;
}
bool matchesF2Identity(std::span<const std::uint8_t> response) {
    return frame(response, 1) && response[3] == 0 && response[4] == 0 &&
        response[5] == 8 && response[10] == 118 && response[11] == 35;
}
std::optional<Report> profileQuery(ReadBlock block) {
    if (!knownBlock(block))
        return std::nullopt;
    Report report{8,8,0,static_cast<std::uint8_t>(block.address >> 8),
        static_cast<std::uint8_t>(block.address),block.length};
    finalize(report);
    return report;
}
bool matchesProfileReply(std::span<const std::uint8_t> response, ReadBlock block) {
    return knownBlock(block) && frame(response, 8) &&
        response[3] == (block.address >> 8) && response[4] == (block.address & 0xff) &&
        response[5] == block.length;
}
std::optional<int> decodePollingRate(int raw) {
    switch (raw) {
    case 8: return 125; case 4: return 250; case 2: return 500;
    case 1: return 1000; case 16: return 2000; case 32: return 4000; case 64: return 8000;
    default: return std::nullopt;
    }
}
std::optional<DpiStage> decodeDpi(std::span<const std::uint8_t> record) {
    if (record.size() != 4 || (std::accumulate(record.begin(), record.end(), 0u) & 0xffu) != 0x55u)
        return std::nullopt;
    // Official 3950 format, corroborated by the captured sixth stage:
    // flags 1:0 / 5:4 are x/y multipliers; 3:2 / 7:6 are high value bits.
    // Each multiplier bit doubles its axis. X and Y need not be identical.
    const auto axis = [](int low, int high, int multiplier) {
        return (low + (high << 8) + 1) * 50 *
            ((multiplier & 1) ? 2 : 1) * ((multiplier & 2) ? 2 : 1);
    };
    const int flags = record[2];
    DpiStage result{axis(record[0], (flags >> 2) & 3, flags & 3),
                    axis(record[1], (flags >> 6) & 3, (flags >> 4) & 3)};
    if (result.x > 42000 || result.y > 42000)
        return std::nullopt;
    return result;
}
bool ProfileCapture::accept(ReadBlock block, std::span<const std::uint8_t> response) {
    if (!matchesProfileReply(response, block))
        return false;
    const auto index = static_cast<std::size_t>(std::find(kProfileReads.begin(), kProfileReads.end(), block) -
                                               kProfileReads.begin());
    // A late duplicate must not overwrite part of an already collected sample.
    if (received_[index])
        return false;
    std::copy_n(response.begin() + 6, block.length, bytes_.begin() + block.address);
    received_[index] = true;
    return true;
}
std::optional<Profile> ProfileCapture::decode() const {
    if (!std::all_of(received_.begin(), received_.end(), [](bool value) { return value; }))
        return std::nullopt;
    const auto scalar = [this](int address) -> std::optional<int> {
        if (((static_cast<unsigned>(bytes_[address]) + bytes_[address + 1]) & 0xffu) != 0x55u)
            return std::nullopt;
        return bytes_[address];
    };
    const auto rate = scalar(0), count = scalar(2), active = scalar(4), lod = scalar(10);
    const auto debounce = scalar(0xa9), sync = scalar(0xab), sleep = scalar(0xad);
    const auto angle = scalar(0xaf), ripple = scalar(0xb1), performance = scalar(0xb5);
    if (!rate || !decodePollingRate(*rate) || !count || *count < 1 || *count > 8 ||
        !active || *active >= *count || !lod || !debounce || !sync || *sync > 1 ||
        !sleep || !angle || *angle > 1 || !ripple || *ripple > 1 || !performance)
        return std::nullopt;
    Profile result;
    result.pollingRateHz = *decodePollingRate(*rate);
    result.activeStage = *active;
    result.lodCode = *lod;
    result.debounceMs = *debounce;
    result.motionSync = *sync == 1;
    result.sleepSeconds = *sleep * 10;
    result.angleSnapping = *angle == 1;
    result.rippleControl = *ripple == 1;
    result.performanceCode = *performance;
    for (int index = 0; index < *count; ++index) {
        const auto stage = decodeDpi(std::span(bytes_).subspan(12 + index * 4, 4));
        if (!stage)
            return std::nullopt;
        result.stages.push_back(*stage);
    }
    for (std::size_t index = 0; index < result.buttons.size(); ++index) {
        const auto record = std::span(bytes_).subspan(0x60 + index * 4, 4);
        if ((std::accumulate(record.begin(), record.end(), 0u) & 0xffu) != 0x55u)
            return std::nullopt;
        // Unknown actions retain their class/parameter; never call them disabled.
        result.buttons[index] = {record[0], (static_cast<int>(record[1]) << 8) | record[2]};
    }
    return result;
}
bool validProfileField(std::string_view key, int value) {
    if (key == "vgn_rate_hz") {
        constexpr std::array rates{125,250,500,1000,2000,4000,8000};
        return std::find(rates.begin(), rates.end(), value) != rates.end();
    }
    if (key == "vgn_stage_count") return value >= 1 && value <= 8;
    if (key == "vgn_active_stage") return value >= 0 && value < 8;
    if (key == "vgn_motion_sync" || key == "vgn_angle_snap" || key == "vgn_ripple")
        return value == 0 || value == 1;
    if (key == "vgn_sleep_seconds") return value >= 0 && value <= 2550 && value % 10 == 0;
    if (key == "vgn_lod_code" || key == "vgn_debounce_ms" || key == "vgn_performance_code")
        return value >= 0 && value <= 255;
    if (key.size() == 11 && key.substr(0, 8) == "vgn_dpi_" && key[8] >= '0' && key[8] <= '7' &&
        (key.substr(9) == "_x" || key.substr(9) == "_y"))
        return value >= 50 && value <= 42000 && value % 50 == 0;
    if (key.size() > 10 && key.substr(0, 8) == "vgn_key_" && key[8] >= '0' && key[8] <= '5') {
        if (key.substr(9) == "_class") return value >= 0 && value <= 255;
        if (key.substr(9) == "_param") return value >= 0 && value <= 65535;
    }
    return false;
}
std::array<std::uint8_t, 2> encodeScalarRecord(int value) {
    if (value < 0 || value > 255)
        return {};
    const auto first = static_cast<std::uint8_t>(value);
    return {first, static_cast<std::uint8_t>(0x55u - first)};
}
std::optional<std::array<std::uint8_t, 4>> encodeDpiRecord(int x, int y) {
    const auto xs = encodeDpiAxis(x), ys = encodeDpiAxis(y);
    if (!xs || !ys)
        return std::nullopt;
    std::array<std::uint8_t, 4> record{};
    record[0] = static_cast<std::uint8_t>(xs->first & 0xff);
    record[1] = static_cast<std::uint8_t>(ys->first & 0xff);
    record[2] = static_cast<std::uint8_t>(((xs->first >> 8) & 3) << 2 |
                                          (((ys->first >> 8) & 3) << 6) |
                                          xs->second | (ys->second << 4));
    record[3] = static_cast<std::uint8_t>(0x55u - (record[0] + record[1] + record[2]));
    return record;
}
std::optional<int> encodePollingRate(int hertz) {
    switch (hertz) {
    case 125: return 8; case 250: return 4; case 500: return 2;
    case 1000: return 1; case 2000: return 16; case 4000: return 32; case 8000: return 64;
    default: return std::nullopt;
    }
}
std::optional<Report> flashWriteQuery(std::uint16_t address, std::span<const std::uint8_t> data) {
    if (!writableFlashBlock(address, data.size()))
        return std::nullopt;
    Report report{};
    report[0] = kReportId;
    report[1] = kFlashWriteCommand;
    report[3] = static_cast<std::uint8_t>(address >> 8);
    report[4] = static_cast<std::uint8_t>(address & 0xff);
    report[5] = static_cast<std::uint8_t>(data.size());
    std::copy(data.begin(), data.end(), report.begin() + 6);
    finalize(report);
    return report;
}
} // namespace peripheral::vgn
