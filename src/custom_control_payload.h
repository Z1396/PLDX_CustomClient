#pragma once  // 头文件包含守卫：防止本头被重复包含

#include <pch.h>  // 预编译头(PCH)：集中包含 STL、spdlog 等公共依赖

// 自定义控制载荷模块：定义上报给裁判系统的自定义字节块布局与编解码辅助接口。
inline constexpr size_t kCustomControlRobotCount = 5; // 敌方可上报血量的机器人数量(共 5 台)
inline constexpr size_t kCustomControlHealthDataSize = kCustomControlRobotCount * sizeof(uint16_t); // 血量数据区字节数 = 机器人数 * 2 字节(uint16)
inline constexpr size_t kCustomControlAirSupportCallOffset = kCustomControlHealthDataSize; // 空中支援请求标志的偏移(紧接血量区之后)
inline constexpr size_t kCustomControlCounterCooldownOffset = kCustomControlAirSupportCallOffset + 1; // 空中支援冷却计数的偏移(+1 跳过 1 字节呼叫标志)
inline constexpr size_t kCustomControlDebugCounterOffset = kCustomControlCounterCooldownOffset + 1; // 调试计数器的偏移(+1 跳过 1 字节冷却计数)
inline constexpr size_t kCustomControlPayloadSize = kCustomControlDebugCounterOffset + 1; // 整个自定义控制载荷的总字节数
inline constexpr uint8_t kInactiveAirSupportCounterCooldown = 255; // 表示冷却未激活的哨兵值(255)
inline constexpr uint8_t kInitialAirSupportCounterRemaining = 5; // 比赛开始时空中支援计数的初始值
inline constexpr auto kCustomControlPublishInterval = std::chrono::milliseconds(100); // 自定义控制载荷发布间隔(100ms，即 10Hz)

// 空中支援自定义控制状态：维护"呼叫空中支援"请求标志与计数冷却，供打包载荷时读取。
struct AirSupportCustomControlState {  // 纯数据状态结构，无成员函数，由外部函数读写
    bool pending_air_support_call = false; // 是否有待发送的"呼叫空中支援"请求
    std::optional<uint8_t> previous_counter_remaining; // 上一帧收到的剩余计数(用于判断是否恰好减 1)；optional 无值表示尚未收到过
    std::optional<std::chrono::steady_clock::time_point> counter_cooldown_started_at; // 计数减 1 的时刻，用于推算冷却剩余秒数
};  // AirSupportCustomControlState 结构体定义结束

// 功能：标记一次"对手呼叫空中支援"事件，设置待发送请求标志。
// 参数 state：空中支援控制状态(会被修改以置位待发送标志)。
void NoteOpponentAirSupportCalled(AirSupportCustomControlState &state);  // 只置标志、不立即发包

// 功能：比赛开始时重置空中支援计数与冷却状态到初始值。
// 参数 state：空中支援控制状态。
void ResetAirSupportCounterForMatchStart(AirSupportCustomControlState &state);  // 开局调用一次，清空冷却与历史计数

// 功能：记录本帧收到的空中支援剩余计数，并判断是否相较上一帧恰好减 1(触发冷却计时)。
// 参数 state：控制状态；remaining：本帧剩余计数；now：当前时刻。
// 返回：若剩余计数较上一帧减少了 1 则返回 true(用于启动冷却计时)。
bool NoteAirSupportCounterRemaining(AirSupportCustomControlState &state,  // 会被更新 previous_counter_remaining 与冷却起点
                                    uint8_t remaining,  // 裁判系统本帧上报的剩余支援次数
                                    std::chrono::steady_clock::time_point now);  // 当前单调时钟时刻，避免受系统校时影响

// 功能：消费(取走并清空)待发送的空中支援呼叫标志。
// 参数 state：控制状态。
// 返回：若此前有待发送请求返回 1，否则返回 0。
uint8_t ConsumeAirSupportCallFlag(AirSupportCustomControlState &state);  // 读后即清，保证同一事件只上报一次

// 功能：根据冷却起始时刻计算当前应填入载荷的冷却剩余秒数。
// 参数 state：控制状态；now：当前时刻。
// 返回：剩余秒数；若未处于冷却或已超时则返回 kInactiveAirSupportCounterCooldown(255)。
uint8_t GetAirSupportCounterCooldownByte(const AirSupportCustomControlState &state,  // 只读，不修改状态
                                         std::chrono::steady_clock::time_point now);  // 当前时刻，用于算已经流逝多久

// 功能：按既定布局打包完整自定义控制载荷字节数组。
// 参数 enemy_health：各敌方机器人血量(uint16 小端)；state：控制状态；
//       debug_counter：调试计数；now：当前时刻(用于冷却计算)。
// 返回：长度为 kCustomControlPayloadSize 的载荷数组。
std::array<uint8_t, kCustomControlPayloadSize> BuildCustomControlPayload(  // 返回定长数组，长度在编译期确定
    std::span<const uint16_t, kCustomControlRobotCount> enemy_health,  // 定长 span：编译期即校验元素个数必须为 5
    AirSupportCustomControlState &state,  // 非 const：打包时会消费呼叫标志
    uint8_t debug_counter,  // 自增调试计数，用于排查丢包/乱序
    std::chrono::steady_clock::time_point now);  // 当前时刻，用于计算冷却剩余秒

// 功能：判断当前是否到达自定义控制载荷的发布时间。
// 参数 last_publish_time：上次发布时刻(可空)；now：当前时刻。
// 返回：从未发布或距上次发布已满间隔则返回 true。
bool IsCustomControlPublishDue(std::optional<std::chrono::steady_clock::time_point> last_publish_time,  // 按值传入，空值表示从未发布
                               std::chrono::steady_clock::time_point now);  // 当前时刻，与上次发布时刻做差比较
