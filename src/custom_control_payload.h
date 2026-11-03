#pragma once  // 头文件包含守卫：防止本头被重复包含

#include <pch.h>  // 预编译头(PCH)：集中包含 STL、spdlog 等公共依赖

// 自定义控制载荷模块：定义上报给裁判系统的自定义字节块布局与编解码辅助接口。
// 【2027 协议变更】空中支援指令/状态已在 2027 协议中删除，原用于"呼叫标志 + 冷却计数"的 2 字节
// 现为【保留位】：固定填 0，偏移保持不变以免影响机器人端既有解析；具体用途待与机器人端协商后再定义。
inline constexpr size_t kCustomControlRobotCount = 5; // 敌方可上报血量的机器人数量(共 5 台)
inline constexpr size_t kCustomControlHealthDataSize = kCustomControlRobotCount * sizeof(uint16_t); // 血量数据区字节数 = 机器人数 * 2 字节(uint16)
inline constexpr size_t kCustomControlAirSupportCallOffset = kCustomControlHealthDataSize; // 2027 保留字节 0 的偏移(紧接血量区之后，原空中支援请求标志位)
inline constexpr size_t kCustomControlCounterCooldownOffset = kCustomControlAirSupportCallOffset + 1; // 2027 保留字节 1 的偏移(+1 跳过第 1 个保留字节，原空中支援冷却计数位)
inline constexpr size_t kCustomControlDebugCounterOffset = kCustomControlCounterCooldownOffset + 1; // 调试计数器的偏移(+1 跳过第 2 个保留字节)
inline constexpr size_t kCustomControlPayloadSize = kCustomControlDebugCounterOffset + 1; // 整个自定义控制载荷的总字节数
inline constexpr auto kCustomControlPublishInterval = std::chrono::milliseconds(100); // 自定义控制载荷发布间隔(100ms，即 10Hz)

// 功能：按既定布局打包完整自定义控制载荷字节数组。
// 参数 enemy_health：各敌方机器人血量(uint16 小端)；debug_counter：调试计数。
// 返回：长度为 kCustomControlPayloadSize 的载荷数组。
// 说明：2027 起不再有空中支援状态入参，原先的 2 个状态字节固定填 0(保留位)。
std::array<uint8_t, kCustomControlPayloadSize> BuildCustomControlPayload(  // 返回定长数组，长度在编译期确定
    std::span<const uint16_t, kCustomControlRobotCount> enemy_health,  // 定长 span：编译期即校验元素个数必须为 5
    uint8_t debug_counter);  // 自增调试计数，用于排查丢包/乱序

// 功能：判断当前是否到达自定义控制载荷的发布时间。
// 参数 last_publish_time：上次发布时刻(可空)；now：当前时刻。
// 返回：从未发布或距上次发布已满间隔则返回 true。
bool IsCustomControlPublishDue(std::optional<std::chrono::steady_clock::time_point> last_publish_time,  // 按值传入，空值表示从未发布
                               std::chrono::steady_clock::time_point now);  // 当前时刻，与上次发布时刻做差比较
