// Read-only HID inventory. Lists the collections of one VID/PID so a new model
// can be planned without guessing: nothing here opens a device for writing and
// no vendor command is sent.
#include "platform/WindowsBackend.h"
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::string utf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}
int usage() {
    std::cerr << "usage: hid_inventory --vid 1A2C [--pid 7FFF]\n"
                 "  values are hexadecimal; --pid is optional\n";
    return 64;
}
} // namespace

int main(int argc, char** argv) {
    unsigned long vid = 0, pid = 0;
    bool filterPid = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&]() -> const char* { return index + 1 < argc ? argv[++index] : nullptr; };
        if (argument == "--vid") {
            const auto* value = next();
            if (!value) return usage();
            vid = std::strtoul(value, nullptr, 16);
        } else if (argument == "--pid") {
            const auto* value = next();
            if (!value) return usage();
            pid = std::strtoul(value, nullptr, 16);
            filterPid = true;
        } else {
            return usage();
        }
    }
    if (vid == 0)
        return usage();

    peripheral::platform::WindowsBackend backend;
    std::vector<peripheral::platform::HidDeviceInfo> matches;
    for (auto& device : backend.enumerateHidDevices())
        if (device.vid == vid && (!filterPid || device.pid == pid))
            matches.push_back(std::move(device));
    std::cout << "MATCHES " << matches.size() << '\n';
    for (const auto& device : matches) {
        // Restore every manipulator per device: the previous loop ended in
        // decimal, which printed later vendor IDs as if they were decimal.
        std::cout << std::hex << std::uppercase << std::setfill('0');
        std::cout << "VID_" << std::setw(4) << device.vid << " PID_" << std::setw(4) << device.pid
                  << " iface " << std::dec << device.interfaceNumber << " usage "
                  << std::hex << std::setw(4) << device.usagePage << ':'
                  << std::setw(4) << device.usage << " reports in/out/feature " << std::dec
                  << device.inputReportBytes << '/' << device.outputReportBytes << '/'
                  << device.featureReportBytes << " n9 " << (device.n9ControlCandidate ? "yes" : "no")
                  << " vgn " << (device.vgnBatteryCandidate ? "yes" : "no") << '\n';
        std::cout << "  product " << utf8(device.product) << '\n';
        if (!device.serial.empty())
            std::cout << "  serial " << utf8(device.serial) << '\n';
        if (!device.instanceId.empty())
            std::cout << "  instance " << utf8(device.instanceId) << '\n';
        if (!device.error.empty())
            std::cout << "  error " << device.error << '\n';
    }
    return matches.empty() ? 2 : 0;
}
