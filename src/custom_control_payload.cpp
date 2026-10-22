// 自定义控制载荷实现：将敌情/空中支援等状态打包为固定布局字节块并控制发布节奏。
#include "custom_control_payload.h"

namespace {

    // 空中支援计数每减 1 后进入的冷却时长(45 秒)，期间持续上报剩余秒数。
    inline constexpr auto kAirSupportCounterCooldownDuration = std::chrono::seconds(45);

} // namespace

// 功能：标记一次"对手呼叫空中支援"事件。
// 思路：直接置位待发送请求标志，供后续打包时消费。
void NoteOpponentAirSupportCalled(AirSupportCustomControlState &state) {
    state.pending_air_support_call = true; // 置位请求标志，等待下次打包发送
}

// 功能：比赛开始时重置空中支援控制状态。
// 思路：将剩余计数恢复为初始值，并清空已记录的冷却起始时刻。
void ResetAirSupportCounterForMatchStart(AirSupportCustomControlState &state) {
    state.previous_counter_remaining = kInitialAirSupportCounterRemaining; // 重置为初始剩余计数(5)
    state.counter_cooldown_started_at.reset(); // 清除冷却计时起点，表示无冷却
}

// 功能：记录本帧空中支援剩余计数并检测是否减 1。
// 思路：若已记录上一帧计数且本帧恰好比其小 1，则判定触发了一次支援、启动冷却计时。
bool NoteAirSupportCounterRemaining(AirSupportCustomControlState &state,
                                    uint8_t remaining,
                                    std::chrono::steady_clock::time_point now) {
    // 判断是否相对上一帧恰好减少了 1(认为发生了一次空中支援调用)
    const bool decreased_by_one = state.previous_counter_remaining.has_value()
                                  && static_cast<int>(*state.previous_counter_remaining)
                                         == static_cast<int>(remaining) + 1;
    state.previous_counter_remaining = remaining; // 更新为本次计数，供下一帧比较
    if (decreased_by_one) {
        state.counter_cooldown_started_at = now; // 触发减 1，记录冷却起始时刻
    }
    return decreased_by_one; // 返回是否触发冷却
}

// 功能：以"消费"语义取走待发送的空中支援呼叫标志。
// 思路：若标志已置位则清零并返回 1，保证同一请求只上报一次。
uint8_t ConsumeAirSupportCallFlag(AirSupportCustomControlState &state) {
    if (!state.pending_air_support_call) {
        return 0; // 无待发送请求
    }
    state.pending_air_support_call = false; // 消费后清除标志
    return 1; // 返回已触发
}

// 功能：计算当前空中支援冷却剩余秒数(供写入载荷)。
// 思路：未进入冷却或已超时返回哨兵值 255；否则用总时长减去已过秒数。
uint8_t GetAirSupportCounterCooldownByte(const AirSupportCustomControlState &state,
                                         std::chrono::steady_clock::time_point now) {
    if (!state.counter_cooldown_started_at.has_value()) {
        return kInactiveAirSupportCounterCooldown; // 尚未进入冷却，返回非激活哨兵值
    }

    const auto elapsed = now - *state.counter_cooldown_started_at; // 已过去的时间
    if (elapsed < std::chrono::seconds(0) || elapsed >= kAirSupportCounterCooldownDuration) {
        return kInactiveAirSupportCounterCooldown; // 时钟异常或冷却已结束，按非激活处理
    }

    const auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed); // 取整到秒
    return static_cast<uint8_t>((kAirSupportCounterCooldownDuration - elapsed_seconds).count()); // 剩余秒数
}

// 功能：按固定布局构造完整自定义控制载荷字节数组。
// 思路：先以小端格式写入各机器人血量，再依次填入呼叫标志、冷却字节与调试计数。
std::array<uint8_t, kCustomControlPayloadSize> BuildCustomControlPayload(
    std::span<const uint16_t, kCustomControlRobotCount> enemy_health,
    AirSupportCustomControlState &state,
    uint8_t debug_counter,
    std::chrono::steady_clock::time_point now) {
    std::array<uint8_t, kCustomControlPayloadSize> data{}; // 全部初始化为 0
    for (size_t k = 0; k < kCustomControlRobotCount; k++) {
        const uint16_t hp = enemy_health[k];
        data[k * 2] = static_cast<uint8_t>(hp & 0xFF); // 低字节
        data[k * 2 + 1] = static_cast<uint8_t>((hp >> 8) & 0xFF); // 高字节(小端序)
    }

    data[kCustomControlAirSupportCallOffset] = ConsumeAirSupportCallFlag(state); // 填入空中支援呼叫标志
    data[kCustomControlCounterCooldownOffset] = GetAirSupportCounterCooldownByte(state, now); // 填入冷却剩余秒数
    data[kCustomControlDebugCounterOffset] = debug_counter; // 填入调试计数器
    return data; // 返回打包好的载荷
}

// 功能：判断当前时刻是否应发布自定义控制载荷。
// 思路：从未发布过，或距上次发布已达设定间隔则允许发布。
bool IsCustomControlPublishDue(std::optional<std::chrono::steady_clock::time_point> last_publish_time,
                               std::chrono::steady_clock::time_point now) {
    return !last_publish_time.has_value() || now - *last_publish_time >= kCustomControlPublishInterval;
}
