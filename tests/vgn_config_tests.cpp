#include "core/vgn_config.h"
#include "fixtures/vgn_f2_capture.h"
#include <iostream>
#include <numeric>
using namespace peripheral::vgn;
namespace {
void checksum(Report& report) {
    report.back() = static_cast<std::uint8_t>(0x55u - std::accumulate(report.begin(), report.end()-1, 0u));
}
}
int main() {
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* text) {
        ++checks;
        if (!ok) { ++failures; std::cerr << text << '\n'; }
    };
    const auto profile = fixture::f2Capture().decode();
    check(profile.has_value(), "actual capture decodes completely");
    if (!profile) return 1;
    check(profile->pollingRateHz == 1000 && profile->activeStage == 3 && profile->stages.size() == 6,
          "actual rate, active stage and count");
    const std::array<int,6> expected{400,800,1600,3200,6400,42000};
    for (std::size_t i = 0; i < expected.size(); ++i)
        check(profile->stages[i].x == expected[i] && profile->stages[i].y == expected[i], "captured DPI stage");
    check(profile->sleepSeconds == 60 && profile->debounceMs == 0 && profile->motionSync &&
          !profile->angleSnapping && !profile->rippleControl && profile->lodCode == 1,
          "actual performance fields retain zero and units");
    check(profile->buttons[0].actionClass == 1 && profile->buttons[0].parameter == 0x100 &&
          profile->buttons[5].actionClass == 2 && profile->buttons[5].parameter == 0x100,
          "button classes and big endian parameters match capture");
    check(!ProfileCapture{}.decode(), "empty read is unknown, never default DPI");
    for (std::size_t i = 0; i < kProfileReads.size(); ++i) {
        auto report = fixture::f2Profile[i];
        const auto block = kProfileReads[i];
        check(profileQuery(block).has_value() && matchesProfileReply(report, block), "known read validates");
        check(!matchesProfileReply(std::span(report).first(16),block), "truncation rejected");
        report[4] ^= 1; checksum(report);
        check(!matchesProfileReply(report,block), "late reply from different address rejected");
        report = fixture::f2Profile[i]; report[5] = 0; checksum(report);
        check(!matchesProfileReply(report,block), "wrong payload length rejected");
        report = fixture::f2Profile[i]; report[6] ^= 1;
        check(!matchesProfileReply(report,block), "corrupt frame rejected");
        ProfileCapture incomplete;
        for (std::size_t j = 0; j < kProfileReads.size(); ++j)
            if (i != j) incomplete.accept(kProfileReads[j],fixture::f2Profile[j]);
        check(!incomplete.decode(), "missing fragment never produces a profile");
    }
    auto duplicate = fixture::f2Capture();
    check(!duplicate.accept(kProfileReads[0],fixture::f2Profile[0]), "duplicate cannot replace prior chunk");
    check(!profileQuery({0x100,10}) && !profileQuery({0,0}) && !profileQuery({0,11}),
          "no arbitrary memory read or oversized request");
    for (int variant = 0; variant < 5; ++variant) {
        auto frames = fixture::f2Profile;
        if (variant == 0) frames[0][6] = 3, frames[0][7] = 0x52; // Unknown rate, valid scalar checksum.
        if (variant == 1) frames[0][8] = 0, frames[0][9] = 0x55; // Zero count, no clamp to one.
        if (variant == 2) frames[0][10] = 6, frames[0][11] = 0x4f; // Active is outside enabled count.
        if (variant == 3) frames[1][8] ^= 1; // Bad DPI record but good outer frame.
        if (variant == 4) frames[7][6] ^= 1; // Bad button record but good outer frame.
        ProfileCapture capture;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            checksum(frames[i]);
            capture.accept(kProfileReads[i],frames[i]);
        }
        check(!capture.decode(), "invalid scalar, DPI or button record rejects profile without fallback");
    }
    const std::array<std::uint8_t,4> independentAxes{7,15,0,0x3f};
    const auto axes = decodeDpi(independentAxes);
    check(axes && axes->x == 400 && axes->y == 800, "X and Y do not have to match");
    const std::array<std::uint8_t,4> tooHigh{0,0,0xff,0x56};
    check(!decodeDpi(tooHigh), "other sensor ranges are not claimed on this model");
    Report identity{8,1,0,0,0,8,0x4d,0,0x45,0xf6,0x76,0x23,5,0,0,0,0x1e};
    check(matchesF2Identity(identity), "actual CID 118 MID 35 identity validates");
    identity[10] = 117; checksum(identity);
    check(!matchesF2Identity(identity), "same receiver PID does not admit a different CID");
    check(identityQuery()[1] == 1 && identityQuery()[5] == 8, "identification request has no flash write");
    check(validProfileField("vgn_dpi_5_x",42000) && validProfileField("vgn_key_5_param",65535), "readback fields roundtrip");
    check(!validProfileField("vgn_dpi_8_x",400) && !validProfileField("vgn_key_6_param",1) &&
          !validProfileField("vgn_dpi_0_x",42050) && !validProfileField("vgn_rate_hz",999) &&
          !validProfileField("vgn_motion_sync",2), "invalid saved settings rejected");
    // Write candidates: frame layout from the vendor bundle, still unverified on
    // hardware. Every encoded stage must decode back through the verified reader.
    for (int dpi : {400, 800, 1600, 3200, 6400, 26000, 42000}) {
        const auto record = encodeDpiRecord(dpi, dpi);
        const auto decoded = record ? decodeDpi(*record) : std::nullopt;
        check(decoded && decoded->x == dpi && decoded->y == dpi,
              "encoded DPI record round-trips through the verified decoder");
    }
    check(!encodeDpiRecord(42025,42025) && !encodeDpiRecord(0,400) && !encodeDpiRecord(400,-50) &&
          !encodeDpiRecord(204850,400), "values without an exact format are rejected, never rounded");
    const auto stage = encodeDpiRecord(3200,3200);
    check(stage.has_value(), "stage four is representable");
    if (stage) {
        const auto write = flashWriteQuery(kFlashDpiBase + 3 * 4, *stage);
        check(write.has_value(), "whitelisted DPI address builds a frame");
        if (write) {
            check((*write)[0] == 8 && (*write)[1] == 7 && (*write)[2] == 0 && (*write)[3] == 0 &&
                  (*write)[4] == 24 && (*write)[5] == 4, "write frame names command, address and length");
            check((*write)[6] == (*stage)[0] && (*write)[9] == (*stage)[3], "record bytes are carried verbatim");
            check((std::accumulate(write->begin(), write->end(), 0u) & 0xffu) == 0x55u,
                  "whole write frame sums to 0x55");
        }
    }
    const auto scalar = encodeScalarRecord(1);
    check(scalar[0] == 1 && scalar[1] == 0x54, "scalar record is value plus complement");
    check(flashWriteQuery(kFlashActiveStage, scalar).has_value() &&
          flashWriteQuery(kFlashReportRate, scalar).has_value(), "known scalars are writable");
    const auto shortRecord = encodeDpiRecord(400, 400);
    const auto truncated = shortRecord ? std::span<const std::uint8_t>(*shortRecord).first(2)
                                       : std::span<const std::uint8_t>{};
    check(!flashWriteQuery(6, scalar) && !flashWriteQuery(kFlashDpiBase + 3 * 4, scalar) &&
          !flashWriteQuery(kFlashDpiBase + 3 * 4, truncated) && !flashWriteQuery(0x100, scalar),
          "other addresses and lengths never build a write");
    check(encodePollingRate(1000) == 1 && encodePollingRate(125) == 8 && encodePollingRate(8000) == 64 &&
          !encodePollingRate(999), "polling rate codes invert the read mapping");
    std::cout << checks << " VGN configuration checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
