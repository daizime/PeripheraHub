// Hardware acceptance probe for one VGN F2 Master+ DPI stage record.
//
// It is read-only unless --apply is passed. Even then it performs exactly one
// whitelisted 4-byte write and one read-back, and never retries. The probe is a
// measurement tool for docs/PRIVATE_WRITE_CANDIDATES_2026-10-19.md, not the
// product write path: no GUI, no archiving, no automatic recovery.
#include "platform/WindowsBackend.h"
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
int usage() {
    std::cerr << "usage: vgn_dpi_write_probe --stage N --x DPI --y DPI [--apply]\n"
                 "  without --apply the device is only read\n";
    return 64;
}
template <class Container>
std::string hex(const Container& bytes) {
    std::ostringstream text;
    text << std::hex << std::uppercase << std::setfill('0');
    for (const auto byte : bytes)
        text << std::setw(2) << static_cast<int>(byte) << ' ';
    return text.str();
}
} // namespace

int main(int argc, char** argv) {
    int stage = -1, x = 0, y = 0, rate = 0, activeStage = -1;
    bool apply = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&](int& target) {
            if (index + 1 >= argc)
                return false;
            target = std::atoi(argv[++index]);
            return true;
        };
        if (argument == "--stage") {
            if (!value(stage)) return usage();
        } else if (argument == "--x") {
            if (!value(x)) return usage();
        } else if (argument == "--y") {
            if (!value(y)) return usage();
        } else if (argument == "--rate") {
            if (!value(rate)) return usage();
        } else if (argument == "--active-stage") {
            if (!value(activeStage)) return usage();
        } else if (argument == "--apply") {
            apply = true;
        } else {
            return usage();
        }
    }
    const bool scalar = rate > 0 || activeStage >= 0;
    if (!scalar && (stage < 0 || x <= 0 || y <= 0))
        return usage();

    peripheral::platform::WindowsBackend backend;
    std::vector<peripheral::platform::HidDeviceInfo> candidates;
    for (const auto& device : backend.enumerateHidDevices())
        if (device.vid == 0x3554 && (device.pid == 0xFB56 || device.pid == 0xFB57) &&
            device.usagePage == 0xFF02 && device.usage == 2 && device.vgnBatteryCandidate)
            candidates.push_back(device);
    if (candidates.empty()) {
        std::cout << "NO_DEVICE no VGN F2 control interface is present\n";
        return 2;
    }
    if (candidates.size() > 1) {
        // Writing to a guessed interface is exactly what this project forbids.
        std::cout << "AMBIGUOUS " << candidates.size() << " VGN control interfaces are present\n";
        return 2;
    }
    const auto& device = candidates.front();
    std::cout << "DEVICE " << device.pid << " usage FF02:0002 reports " << device.inputReportBytes << '\n';

    const auto current = backend.queryVgnBattery(device);
    if (!current.profile) {
        std::cout << "READ_FAILED " << (current.profileError.empty() ? current.error : current.profileError) << '\n';
        return 3;
    }
    std::cout << "CURRENT_STAGES " << current.profile->stages.size() << " active "
              << current.profile->activeStage << " rate " << current.profile->pollingRateHz << " Hz\n";
    for (std::size_t index = 0; index < current.profile->stages.size(); ++index)
        std::cout << "  stage " << index << " x " << current.profile->stages[index].x << " y "
                  << current.profile->stages[index].y << '\n';

    if (scalar) {
        if (rate > 0 && activeStage >= 0) {
            std::cout << "ONE_ACTION_AT_A_TIME\n";
            return 64;
        }
        const bool isRate = rate > 0;
        std::cout << (isRate ? "RATE_REQUEST " : "ACTIVE_STAGE_REQUEST ")
                  << (isRate ? rate : activeStage) << '\n';
        if (!apply) {
            std::cout << "DRY_RUN no write was sent; repeat with --apply\n";
            return 0;
        }
        const auto result = isRate ? backend.writeVgnPollingRate(device, rate)
                                   : backend.writeVgnActiveStage(device, activeStage);
        std::cout << "SENT " << (result.sent ? "yes" : "no") << " unchanged "
                  << (result.unchanged ? "yes" : "no") << '\n';
        std::cout << "BEFORE " << result.before << " AFTER "
                  << (result.after ? std::to_string(*result.after) : std::string("-")) << '\n';
        if (!result.error.empty())
            std::cout << "ERROR " << result.error << '\n';
        std::cout << (result.verified ? "VERIFIED read-back matches\n" : "UNVERIFIED read-back differs\n");
        return result.verified ? 0 : 5;
    }

    const auto record = peripheral::vgn::encodeDpiRecord(x, y);
    if (!record) {
        std::cout << "NOT_REPRESENTABLE " << x << "x" << y << '\n';
        return 4;
    }
    std::cout << "REQUESTED_RECORD " << hex(*record) << "at address "
              << (peripheral::vgn::kFlashDpiBase + stage * 4) << '\n';
    if (stage >= static_cast<int>(current.profile->stages.size())) {
        std::cout << "STAGE_NOT_ENABLED " << stage << '\n';
        return 4;
    }
    if (!apply) {
        std::cout << "DRY_RUN no write was sent; repeat with --apply to send exactly one write\n";
        return 0;
    }

    const auto result = backend.writeVgnDpiStage(device, stage, x, y);
    std::cout << "SENT " << (result.sent ? "yes" : "no") << " unchanged " << (result.unchanged ? "yes" : "no")
              << '\n';
    std::cout << "BEFORE " << hex(result.before) << '\n';
    std::cout << "AFTER  " << hex(result.after) << '\n';
    if (!result.error.empty())
        std::cout << "ERROR " << result.error << '\n';
    std::cout << (result.verified ? "VERIFIED read-back matches\n" : "UNVERIFIED read-back does not match\n");
    return result.verified ? 0 : 5;
}
