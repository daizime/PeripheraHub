// Hardware acceptance probe for one N9 V2 Ultra PEQ point.
//
// Read-only unless --apply is passed. Even then it reads the point, sends one
// whitelisted 0x68 frame, and reads the same point back once. No retries.
#include "platform/WindowsBackend.h"
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
int usage() {
    std::cerr << "usage: n9_peq_write_probe --point N --hz HZ --gain DB --q Q [--type T] [--enable 0|1] [--apply]\n"
                 "  gain is dB with one decimal, q is the Q factor with one decimal\n";
    return 64;
}
std::string describe(const peripheral::n9::PeqPoint& point) {
    std::ostringstream text;
    text << point.frequencyHz << " Hz " << std::showpos << std::fixed << std::setprecision(1)
         << point.gainTenthsDb / 10.0 << std::noshowpos << " dB Q " << std::setprecision(1)
         << point.qTenths / 10.0 << " type " << point.filterType << (point.enabled ? " enabled" : " bypassed");
    return text.str();
}
} // namespace

int main(int argc, char** argv) {
    int pointIndex = -1, filterType = 0, enabled = 0, setMode = -1;
    bool restoreMode = false, allowChange = false;
    double hertz = 0.0, gainDb = 0.0, q = 0.0;
    bool apply = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&]() -> const char* {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        if (argument == "--point") {
            const auto* value = next();
            if (!value) return usage();
            pointIndex = std::atoi(value);
        } else if (argument == "--hz") {
            const auto* value = next();
            if (!value) return usage();
            hertz = std::atof(value);
        } else if (argument == "--gain") {
            const auto* value = next();
            if (!value) return usage();
            gainDb = std::atof(value);
        } else if (argument == "--q") {
            const auto* value = next();
            if (!value) return usage();
            q = std::atof(value);
        } else if (argument == "--type") {
            const auto* value = next();
            if (!value) return usage();
            filterType = std::atoi(value);
        } else if (argument == "--enable") {
            const auto* value = next();
            if (!value) return usage();
            enabled = std::atoi(value);
        } else if (argument == "--set-eq-mode") {
            const auto* value = next();
            if (!value) return usage();
            setMode = std::atoi(value);
        } else if (argument == "--restore-eq-mode") {
            restoreMode = true;
        } else if (argument == "--allow-change") {
            allowChange = true;
        } else if (argument == "--apply") {
            apply = true;
        } else {
            return usage();
        }
    }
    if (pointIndex < 0 || hertz <= 0 || q <= 0)
        return usage();

    peripheral::n9::PeqPoint requested;
    requested.frequencyHz = static_cast<int>(std::lround(hertz));
    requested.gainTenthsDb = static_cast<int>(std::lround(gainDb * 10.0));
    requested.qTenths = static_cast<int>(std::lround(q * 10.0));
    requested.filterType = filterType;
    requested.enabled = enabled != 0;

    peripheral::platform::WindowsBackend backend;
    std::vector<peripheral::platform::HidDeviceInfo> candidates;
    for (const auto& device : backend.enumerateHidDevices())
        if (device.vid == 0x373B && (device.pid == 0x12D6 || device.pid == 0x12D5) &&
            device.usagePage == 0xFF90 && device.usage == 1 && device.n9ControlCandidate)
            candidates.push_back(device);
    if (candidates.empty()) {
        std::cout << "NO_DEVICE no N9 V2 control collection is present\n";
        return 2;
    }
    if (candidates.size() > 1) {
        std::cout << "AMBIGUOUS " << candidates.size() << " N9 control collections are present\n";
        return 2;
    }
    const auto& device = candidates.front();
    std::cout << "DEVICE 373B:" << std::hex << device.pid << std::dec << " usage FF90:0001\n";

    const auto current = backend.queryN9Controls(device);
    if (!current.status) {
        std::cout << "READ_FAILED " << current.error << '\n';
        return 3;
    }
    std::cout << "STATUS battery " << current.status->batteryPercent << "% eqMode "
              << current.status->eqMode << '\n';
    const auto& existing = current.peq[static_cast<std::size_t>(pointIndex)];
    if (!existing) {
        std::cout << "POINT_UNREADABLE " << pointIndex << '\n';
        return 3;
    }
    std::cout << "POINT_BEFORE " << pointIndex << " " << describe(*existing) << '\n';
    std::cout << "POINT_REQUEST " << pointIndex << " " << describe(requested) << '\n';
    if (!apply) {
        std::cout << "DRY_RUN no write was sent; repeat with --apply to send exactly one write\n";
        return 0;
    }

    // The vendor driver switches to the Custom mode before writing PEQ points;
    // without that step a point write was silently ignored on this receiver.
    const int originalMode = current.status->eqMode;
    if (setMode >= 0) {
        const auto mode = backend.setN9EqMode(device, setMode);
        std::cout << "EQ_MODE before " << mode.before << " requested " << mode.requested;
        if (mode.after)
            std::cout << " after " << *mode.after;
        std::cout << " sent " << (mode.sent ? "yes" : "no") << '\n';
        if (!mode.error.empty())
            std::cout << "EQ_MODE_ERROR " << mode.error << '\n';
        if (!mode.verified) {
            std::cout << "MODE_NOT_CONFIRMED point write aborted\n";
            return 5;
        }
        // 切换模式后必须重新读取该点的实际值：不同模式可能暴露不同的曲线，
        // 用切换前的读数判断"是否相同"会把未知值覆盖掉。
        const auto target = backend.queryN9Controls(device);
        if (!target.peq[static_cast<std::size_t>(pointIndex)]) {
            std::cout << "POINT_UNREADABLE_IN_TARGET_MODE\n";
            return 3;
        }
        const auto& actual = *target.peq[static_cast<std::size_t>(pointIndex)];
        std::cout << "POINT_IN_TARGET_MODE " << pointIndex << " " << describe(actual) << '\n';
        const auto same = [](const peripheral::n9::PeqPoint& left, const peripheral::n9::PeqPoint& right) {
            return left.frequencyHz == right.frequencyHz && left.gainTenthsDb == right.gainTenthsDb &&
                left.qTenths == right.qTenths && left.filterType == right.filterType &&
                left.enabled == right.enabled;
        };
        if (!same(actual, requested) && !allowChange) {
            std::cout << "REFUSED value differs from the request; nothing was written\n"
                         "  (repeat with --allow-change to overwrite this point)\n";
            if (restoreMode) {
                const auto restored = backend.setN9EqMode(device, originalMode);
                std::cout << "EQ_MODE_RESTORE requested " << originalMode
                          << (restored.verified ? " verified\n" : " unverified\n");
            }
            return 7;
        }
    }

    const auto result = backend.writeN9PeqPoint(device, pointIndex, requested);
    std::cout << "SENT " << (result.sent ? "yes" : "no") << " unchanged "
              << (result.unchanged ? "yes" : "no") << '\n';
    if (result.after)
        std::cout << "POINT_AFTER " << pointIndex << " " << describe(*result.after) << '\n';
    if (!result.error.empty())
        std::cout << "ERROR " << result.error << '\n';
    std::cout << (result.verified ? "VERIFIED read-back matches\n" : "UNVERIFIED read-back differs\n");

    if (restoreMode && setMode >= 0) {
        const auto restored = backend.setN9EqMode(device, originalMode);
        std::cout << "EQ_MODE_RESTORE requested " << originalMode;
        if (restored.after)
            std::cout << " after " << *restored.after;
        std::cout << (restored.verified ? " verified\n" : " unverified\n");
        if (!restored.verified)
            return 6;
    }
    return result.verified ? 0 : 5;
}
