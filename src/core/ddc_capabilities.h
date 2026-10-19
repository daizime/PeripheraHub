#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace peripheral::ddc {

struct Capabilities {
    bool valid = false;
    std::string version;
    std::map<std::uint8_t, std::vector<std::uint16_t>> features;
    std::string error;
};

// 只解析设备自述；列表存在不等于功能已通过实际读写验证。
// 任一语法错误都会清空结果，避免残缺或嵌套数据被当作可写枚举。
Capabilities parseCapabilities(std::string_view text);

} // namespace peripheral::ddc
