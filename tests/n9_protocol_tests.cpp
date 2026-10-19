#include "core/n9_protocol.h"
#include <algorithm>
#include <iostream>
#include <vector>
using namespace peripheral::n9;
int main() {
    int failures = 0, checks = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::cerr << label << '\n'; }
    };
    // Real N9 V2 Ultra 373B:12D6 reply, 2026-10-19. Remaining bytes are zero.
    Report status{0x55, 0x65, 0x50, 2, 0, 0, 0, 2, 0, 4, 0, 0, 1, 7, 0, 0, 2, 9};
    auto decoded = parseStatus(status);
    check(decoded.has_value(), "captured status validates");
    if (!decoded) return 1;
    check(decoded->batteryPercent == 80, "80 percent is a device reading");
    check(decoded->batteryKind == BatteryKind::Discharging, "raw battery state 2 is discharging");
    check(decoded->eqMode == 0 && !decoded->sidetone && decoded->sidetoneVolume == 4, "status offsets match capture");
    check(decoded->headsetVersion == std::array<std::uint8_t,4>{0,0,2,9}, "headset firmware bytes retain order");
    for (int percent : {0, 100}) {
        status[2] = static_cast<std::uint8_t>(percent);
        check(parseStatus(status)->batteryPercent == percent, "real zero/full are valid");
    }
    status[2] = 101;
    check(!parseStatus(status), "invalid percentage is unknown, not clamped");
    status[2] = 80;
    status[3] = 26;
    check(!parseStatus(status), "unknown battery kind cannot establish live status");
    status[3] = 3;
    check(parseStatus(status)->batteryKind == BatteryKind::Charging, "charging state remains distinct");
    status[0] = 0x41;
    check(!parseStatus(status), "other N9 HID collection is rejected");
    Report peq{0x55,0x69,1,0,0x40,0xff,0xec,0,9,1,0};
    const auto point = parsePeq(peq);
    check(point && point->frequencyHz == 64 && point->gainTenthsDb == -20 && point->qTenths == 9,
          "captured PEQ has 64 Hz, -2.0 dB and Q 0.9");
    check(point && point->filterType == 1 && !point->enabled, "raw filter/enabled flags are preserved");
    peq[2] = 2;
    check(!parsePeq(peq), "wrong PEQ bank is rejected");
    peq[2] = 1;
    peq[10] = 2;
    check(!parsePeq(peq), "unrecognized enable flag is rejected");
    for (std::size_t size : {std::size_t(0),std::size_t(63),std::size_t(65)}) {
        std::vector<std::uint8_t> frame(size);
        check(!parseStatus(frame) && !parsePeq(frame), "truncated and padded frames are rejected");
    }
    check(!peqQuery(-1) && !peqQuery(10), "point range is bounded");
    for (int i = 0; i < 10; ++i) {
        const auto query = peqQuery(i);
        check(query && (*query)[0] == 0x55 && (*query)[1] == 0x69 && (*query)[2] == 1 && (*query)[3] == i,
              "queries select one known bank and point");
        check(query && std::all_of(query->begin()+4,query->end(),[](auto v){return v==0;}), "queries have no write payload");
    }
    check(statusQuery()[1] == 0x65 && statusQuery()[2] == 1, "status query addresses headset");
    check(validProfileField("n9_peq_0_gain10", -20), "negative PEQ gain may be persisted");
    check(!validProfileField("n9_peq_10_gain10", 1), "persistence rejects extra points");
    check(!validProfileField("n9_peq_0_gain10", -32769), "persistence validates signed range");
    check(!validProfileField("n9_peq_0_unknown", 1), "unknown fields cannot enter a profile");
    // Write frames: layout taken from the vendor bundle, not yet confirmed on
    // hardware. The captured first point must re-encode to the same values.
    const PeqPoint captured{64, -20, 9, 1, false};
    const auto write = peqWriteQuery(0, captured);
    check(write.has_value(), "a captured point is encodable");
    if (write) {
        check((*write)[0] == 0x55 && (*write)[1] == 0x68 && (*write)[2] == 1 && (*write)[3] == 0,
              "write names the observed bank and point");
        check((*write)[4] == 0 && (*write)[5] == 64 && (*write)[6] == 0xff && (*write)[7] == 0xec,
              "frequency and signed gain keep their observed order");
        check((*write)[8] == 0 && (*write)[9] == 9 && (*write)[10] == 1 && (*write)[11] == 0 &&
              (*write)[12] == 0x1f, "Q, type, enable and mask follow the captured point");
        check(std::all_of(write->begin() + 13, write->end(), [](auto v) { return v == 0; }),
              "write leaves the rest of the report empty");
    }
    check(!peqWriteQuery(-1, captured) && !peqWriteQuery(10, captured), "write rejects other points");
    for (const auto& bad : {PeqPoint{0, 0, 9, 1, false}, PeqPoint{64, 0, 4, 1, false},
                            PeqPoint{64, 0, 51, 1, false}, PeqPoint{64, 0, 9, 6, false},
                            PeqPoint{20001, 0, 9, 1, false}, PeqPoint{64, -32769, 9, 1, false}})
        check(!peqWriteQuery(0, bad), "unrepresentable write fields are rejected, not clamped");
    const auto mode = eqModeWriteQuery(1);
    check(mode && (*mode)[1] == 0x21 && (*mode)[2] == 1 && (*mode)[3] == 1, "EQ mode write selector");
    check(!eqModeWriteQuery(-1) && !eqModeWriteQuery(3), "EQ mode is bounded to the observed range");
    std::cout << checks << " N9 protocol checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
