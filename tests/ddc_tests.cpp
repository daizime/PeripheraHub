#include "core/ddc_capabilities.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int checks = 0;
void check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        std::cerr << "Failed at line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void rejected(std::string_view text) {
    const auto result = peripheral::ddc::parseCapabilities(text);
    CHECK(!result.valid);
    CHECK(result.features.empty());
    CHECK(result.version.empty());
    CHECK(!result.error.empty());
}
} // namespace

int main() {
    using peripheral::ddc::parseCapabilities;
    // 由实机只读探测获得；MStarcmds 等厂商字段应原样忽略，不影响 VCP 解析。
    const std::string actual =
        "(prot(monitor)type(lcd)MStarcmds(01 02 03 07 0C E3 F3)"
        "vcp(02 04 05 08 10 12 14(05 08 0B 0C) 16 18 1A 52 60( 11 12 0F) AC AE B2 B6 C6 C8 C9 "
        "D6(01 04 05) DC(00 02 03 05 ) DF FD)mccs_ver(2.1)mswhql(1))";
    const auto typical = parseCapabilities(actual);
    CHECK(typical.valid);
    CHECK(typical.error.empty());
    CHECK(typical.version == "2.1");
    CHECK(typical.features.size() == 23);
    CHECK(typical.features.at(0x10).empty());
    CHECK(typical.features.at(0x12).empty());
    CHECK((typical.features.at(0x60) == std::vector<std::uint16_t>{0x11, 0x12, 0x0f}));
    CHECK((typical.features.at(0xdc) == std::vector<std::uint16_t>{0, 2, 3, 5}));
    CHECK(std::find(typical.features.at(0x60).begin(), typical.features.at(0x60).end(), 0) ==
          typical.features.at(0x60).end()); // 实机 current=0 不在声明列表内，不能补成可写值。
    CHECK(std::find(typical.features.at(0xdc).begin(), typical.features.at(0xdc).end(), 1) ==
          typical.features.at(0xdc).end()); // 情景 current=1 同理，调用方需显示未映射当前值。

    const auto mixed =
        parseCapabilities(" \n ( VcP ( 10\t60 ( f 0011 FFFF f ) dc(0) ) MCCS_VER ( 3.0 ) ) \r");
    CHECK(mixed.valid);
    CHECK(mixed.version == "3.0"); // 保留版本，调用方决定是否支持 MCCS 3.0 table 类型。
    CHECK((mixed.features.at(0x60) == std::vector<std::uint16_t>{15, 17, 65535}));
    CHECK(mixed.features.at(0xdc).front() == 0);
    const auto bare = parseCapabilities("vcp(10 60(0f)12)mccs_ver(2.1)");
    CHECK(bare.valid);
    CHECK(bare.features.size() == 3);
    CHECK(parseCapabilities("(vcp())").valid);
    CHECK(parseCapabilities("(vcp(60()))").features.at(0x60).empty());
    const auto nested = parseCapabilities("(model(foo(vcp(60(11))))vcp(10))");
    CHECK(nested.valid);
    CHECK(nested.features.size() == 1);
    CHECK(!nested.features.contains(0x60));
    CHECK(parseCapabilities("(prot(monitor)model(H24T7))").features.empty());

    rejected("");
    rejected("   ");
    rejected("()");
    rejected("(vcp(10 60(11))");
    rejected("(vcp(10)))");
    rejected("(vcp(10))(vcp(12))");
    rejected("junk(vcp(10)) extra");
    rejected("(vcp(10 10))");
    rejected("(vcp(10) vcp(12))");
    rejected("(vcp(60(11 (12))))");
    rejected("(vcp(60(11) 12 (01(02))))");
    rejected("(vcp(10 ZZ))");
    rejected("(vcp(100))");
    rejected("(vcp(F))");
    rejected("(vcp(0x10))");
    rejected("(vcp(60(10000)))");
    rejected("(vcp(60(0x11)))");
    rejected("(vcp(60(1g)))");
    rejected("(vcp(60(11,12)))");
    rejected("(vcp(10)mccs_ver(2))");
    rejected("(vcp(10)mccs_ver(.2))");
    rejected("(vcp(10)mccs_ver(2.))");
    rejected("(vcp(10)mccs_ver(2.2.1))");
    rejected("(vcp(10)mccs_ver(2.2)mccs_ver(2.1))");
    rejected(std::string("(vcp(10))\0(vcp(60(11)))", 23));
    rejected(std::string(64 * 1024 + 1, ' '));
    rejected(std::string(17, '(') + std::string(17, ')'));
    // 每个截断位置都必须失败，不能保留前半串的可写功能。
    for (std::size_t length = 0; length < actual.size(); ++length)
        rejected(std::string_view(actual).substr(0, length));
    CHECK(parseCapabilities("(vcp(10\v12\f60(11)))").valid);
    CHECK(parseCapabilities("(model(" + std::string(14, '(') + std::string(14, ')') + ")vcp(10))").valid);
    const std::string prefix = "(model(";
    const std::string suffix = ")vcp(10))";
    CHECK(parseCapabilities(prefix + std::string(64 * 1024 - prefix.size() - suffix.size(), 'x') + suffix)
              .valid);
    std::cout << "DDC capabilities: " << checks << " checks passed\n";
}
