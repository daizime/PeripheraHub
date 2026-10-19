#include "core/ddc_capabilities.h"

#include <algorithm>

namespace peripheral::ddc {
namespace {
constexpr std::size_t kMaxLength = 64 * 1024;
constexpr int kMaxDepth = 16;

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}
bool digit(char c) {
    return c >= '0' && c <= '9';
}
int hex(char c) {
    if (digit(c))
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
void skip(std::string_view text, std::size_t& at) {
    while (at < text.size() && space(text[at]))
        ++at;
}
std::string_view trim(std::string_view text) {
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    return text;
}
std::size_t closeGroup(std::string_view text, std::size_t open) {
    int depth = 0;
    for (std::size_t at = open; at < text.size(); ++at) {
        if (text[at] == '(')
            ++depth;
        else if (text[at] == ')' && --depth == 0)
            return at;
    }
    return std::string_view::npos;
}
bool number(std::string_view text, std::size_t& at, std::size_t min, std::size_t max, std::uint16_t& value) {
    const auto begin = at;
    unsigned result = 0;
    while (at < text.size() && hex(text[at]) >= 0) {
        if (at - begin == max)
            return false;
        result = result * 16 + static_cast<unsigned>(hex(text[at++]));
    }
    value = static_cast<std::uint16_t>(result);
    return at - begin >= min;
}
bool parseFeatures(std::string_view text, Capabilities& result) {
    std::size_t at = 0;
    while (skip(text, at), at < text.size()) {
        std::uint16_t code = 0;
        if (!number(text, at, 2, 2, code) || (at < text.size() && !space(text[at]) && text[at] != '('))
            return false;
        auto [feature, inserted] =
            result.features.emplace(static_cast<std::uint8_t>(code), std::vector<std::uint16_t>{});
        if (!inserted)
            return false;
        skip(text, at);
        if (at == text.size() || text[at] != '(')
            continue;
        const auto end = closeGroup(text, at);
        auto values = text.substr(at + 1, end - at - 1);
        std::size_t offset = 0;
        while (skip(values, offset), offset < values.size()) {
            std::uint16_t value = 0;
            // 枚举只允许平面十六进制列表，不接受嵌套或猜测补全。
            if (!number(values, offset, 1, 4, value) || (offset < values.size() && !space(values[offset])))
                return false;
            auto& choices = feature->second;
            if (std::find(choices.begin(), choices.end(), value) == choices.end())
                choices.push_back(value);
        }
        at = end + 1;
    }
    return true;
}
} // namespace

Capabilities parseCapabilities(std::string_view text) {
    auto invalid = [](const char* reason) { return Capabilities{false, {}, {}, reason}; };
    if (text.size() > kMaxLength)
        return invalid("Capabilities exceeds 64 KiB");
    text = trim(text);
    if (text.empty())
        return invalid("Empty capabilities");
    int depth = 0;
    for (char c : text) {
        if (c == '\0' || (c == '(' && ++depth > kMaxDepth) || (c == ')' && --depth < 0))
            return invalid("Invalid capabilities structure");
    }
    if (depth != 0)
        return invalid("Unbalanced capabilities parentheses");
    if (text.front() == '(') {
        if (closeGroup(text, 0) != text.size() - 1)
            return invalid("Unexpected text outside capabilities");
        text = text.substr(1, text.size() - 2);
    }
    Capabilities result;
    bool seenVcp = false, seenVersion = false, seenField = false;
    std::size_t at = 0;
    while (skip(text, at), at < text.size()) {
        std::string name;
        while (at < text.size() &&
               (digit(text[at]) || text[at] == '_' || (text[at] >= 'a' && text[at] <= 'z') ||
                (text[at] >= 'A' && text[at] <= 'Z'))) {
            const char c = text[at++];
            name += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }
        skip(text, at);
        if (name.empty() || at == text.size() || text[at] != '(')
            return invalid("Expected named capabilities field");
        const auto end = closeGroup(text, at);
        const auto body = text.substr(at + 1, end - at - 1);
        if (name == "vcp") {
            if (seenVcp || !parseFeatures(body, result))
                return invalid("Invalid or repeated VCP feature list");
            seenVcp = true;
        } else if (name == "mccs_ver") {
            const auto version = trim(body);
            const auto dot = version.find('.');
            if (seenVersion || dot == 0 || dot == std::string_view::npos || dot + 1 == version.size() ||
                !std::all_of(version.begin(), version.begin() + dot, digit) ||
                !std::all_of(version.begin() + dot + 1, version.end(), digit))
                return invalid("Invalid or repeated MCCS version");
            result.version = version;
            seenVersion = true;
        }
        seenField = true;
        at = end + 1;
    }
    if (!seenField)
        return invalid("No capabilities fields");
    result.valid = true;
    return result;
}
} // namespace peripheral::ddc
