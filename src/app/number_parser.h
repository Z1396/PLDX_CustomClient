// number_parser.h：无异常的整数解析（C++23 std::expected + std::from_chars）。
// 取代原先 std::stoi / std::atoi + try-catch 的写法：不抛异常、不分配、失败带原因。
#pragma once

#include <charconv>
#include <expected>
#include <limits>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace RoboGo::app {

// 解析失败的具体原因：用于日志与单测，避免「一个 nullopt 什么都说不清」。
enum class IntegerParseError {
    Empty,               // 输入为空
    InvalidCharacter,    // 开头不是合法十进制数字
    OutOfRange,          // 超出目标类型（或 long long）的表示范围
    TrailingCharacters,  // 数字之后还有多余字符（比 std::stoi 更严格）
};

[[nodiscard]] inline constexpr const char *ToString(const IntegerParseError error) noexcept {
    switch (error) {
        case IntegerParseError::Empty:              return "empty";
        case IntegerParseError::InvalidCharacter:   return "invalid character";
        case IntegerParseError::OutOfRange:         return "out of range";
        case IntegerParseError::TrailingCharacters: return "trailing characters";
    }
    return "unknown";
}

/**
 * @brief 把十进制字符串解析为整数，全程不抛异常、不分配内存。
 * @note 思路：统一用 long long 作中间类型（只依赖 from_chars 保证存在的重载），再按目标类型做范围检查，避开 uint16_t/uint32_t 等小整型重载的实现差异。
 * @note 注意：与 std::stoi 不同，这里要求整串都是数字；"12abc" 会返回 TrailingCharacters。
 */
template <typename T>
/**
 * @brief ParseInteger
 * @note 纯函数：只读入参，不修改全局状态，可在任意线程调用。
 */
[[nodiscard]] std::expected<T, IntegerParseError> ParseInteger(const std::string_view text) noexcept {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>, "T must be an integer type");
    if (text.empty()) {
        return std::unexpected(IntegerParseError::Empty);
    }
    long long value = 0;
    const char *const first = text.data();
    const char *const last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);  // 默认 base 10，不跳前导空白
    if (ec == std::errc::invalid_argument) {
        return std::unexpected(IntegerParseError::InvalidCharacter);
    }
    if (ec == std::errc::result_out_of_range) {
        return std::unexpected(IntegerParseError::OutOfRange);
    }
    if (ptr != last) {
        return std::unexpected(IntegerParseError::TrailingCharacters);
    }
    if constexpr (std::is_unsigned_v<T>) {
        if (value < 0 ||
            static_cast<unsigned long long>(value) > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
            return std::unexpected(IntegerParseError::OutOfRange);
        }
    } else {
        if (value < static_cast<long long>(std::numeric_limits<T>::min()) ||
            value > static_cast<long long>(std::numeric_limits<T>::max())) {
            return std::unexpected(IntegerParseError::OutOfRange);
        }
    }
    return static_cast<T>(value);
}

}  // namespace RoboGo::app

