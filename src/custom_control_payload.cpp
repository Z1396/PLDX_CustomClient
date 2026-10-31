// 自定义控制载荷实现：将敌情、空中支援等状态打包成固定布局二进制载荷，同时控制MQTT上报发布节奏
#include "custom_control_payload.h"

// 文件内部匿名命名空间：仅放置本编译单元私有的常量(空中支援冷却总时长)，
// 不对外暴露，避免与其他编译单元的符号冲突。
namespace {
    // 空中支援计数消耗一次之后，进入冷却的总时长：45秒；冷却期间需要持续上报剩余冷却秒数给上位机/裁判系统
    inline constexpr auto kAirSupportCounterCooldownDuration = std::chrono::seconds(45);
} // namespace

// 功能：标记一次【对手呼叫空中支援】事件
// 思路：只把一个待发送标志位置true；不立刻发包，等到下一次打包载荷的时候统一消费这个标志
//       解耦事件触发和发包时机，避免事件一来就频繁发送报文，交给发布节奏统一管控
void NoteOpponentAirSupportCalled(AirSupportCustomControlState &state) {
    state.pending_air_support_call = true; // 置位待发送标记，等待打包载荷时消费
}

// 功能：比赛开局的时候，重置空中支援的整套状态
// 思路：支援计数恢复到比赛初始值，清空冷却计时起点，冷却状态重置为未激活
void ResetAirSupportCounterForMatchStart(AirSupportCustomControlState &state) {
    // 恢复空中支援剩余可用次数为初始值（宏 kInitialAirSupportCounterRemaining 一般是 5）
    state.previous_counter_remaining = kInitialAirSupportCounterRemaining;
    // 清空冷却起始时间点，std::optional清空 = 当前没有冷却在跑
    state.counter_cooldown_started_at.reset();
}

// 功能：对比上一帧和当前帧的空中支援剩余次数，检测是否刚消耗掉一次支援（次数-1）
// 思路：只有本帧数值恰好 = 上一帧 - 1，才判定为触发一次空中支援；一旦命中，记录冷却开始时间
// 参数 remaining：裁判报上来的当前剩余支援次数；now：当前steady_clock时间戳
// 返回值：true = 刚刚消耗一次支援、开启冷却；false = 次数没变或者变化不是-1
bool NoteAirSupportCounterRemaining(AirSupportCustomControlState &state,
                                    uint8_t remaining,
                                    std::chrono::steady_clock::time_point now) {
    // 判断条件：上一帧有记录数值，并且【当前值 == 上一帧 - 1】，判定消耗一次支援
    const bool decreased_by_one = state.previous_counter_remaining.has_value()
                                  && static_cast<int>(*state.previous_counter_remaining)
                                     == static_cast<int>(remaining) + 1;

    // 更新历史记录：把当前帧次数保存下来，留给下一帧做对比
    state.previous_counter_remaining = remaining;

    if (decreased_by_one) {
        // 确认消耗一次支援，记录冷却起始时刻，开始45s倒计时
        state.counter_cooldown_started_at = now;
    }
    return decreased_by_one;
}

// 功能：【消费式读取】空中支援呼叫标记，保证同一个事件只上报一次
// 思路：典型的一次性标志：读到标记为true就清零、返回1；后续再读就是0，防止同个事件重复上报多条MQTT消息
uint8_t ConsumeAirSupportCallFlag(AirSupportCustomControlState &state) {
    if (!state.pending_air_support_call) {
        return 0; // 没有待上报的呼叫事件
    }
    state.pending_air_support_call = false; // 消费，清除标志
    return 1; // 返回1代表本次载荷里面需要带上“发生了空中支援呼叫”
}

// 功能：计算空中支援冷却剩余秒数，输出单字节给载荷
// 思路：
// 1. 没有冷却计时（optional无值）→ 返回哨兵值 255（kInactiveAirSupportCounterCooldown，代表冷却未激活）
// 2. 已经在冷却期内：总45s - 已经过去的秒数 = 剩余冷却秒
// 3. 时间回拨 / 冷却超时结束 → 同样返回哨兵255
uint8_t GetAirSupportCounterCooldownByte(const AirSupportCustomControlState &state,
                                         std::chrono::steady_clock::time_point now) {
    // 没有开启冷却计时，直接返回非激活哨兵值
    if (!state.counter_cooldown_started_at.has_value()) {
        return kInactiveAirSupportCounterCooldown;
    }

    // 计算从冷却开始到现在流逝的时间
    const auto elapsed = now - *state.counter_cooldown_started_at;
    // 时间倒流（NTP校时回拨）或者冷却时长已经超过45s → 冷却结束，哨兵值
    if (elapsed < std::chrono::seconds(0) || elapsed >= kAirSupportCounterCooldownDuration) {
        return kInactiveAirSupportCounterCooldown;
    }

    // 流逝时间向下取整到整秒
    const auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
    // 剩余冷却秒 = 总冷却时间 - 已流逝秒，转uint8放入报文
    return static_cast<uint8_t>((kAirSupportCounterCooldownDuration - elapsed_seconds).count());
}

// 功能：组装完整自定义上报载荷，固定长度std::array，固定字段排布、小端编码
// 报文布局顺序：
// [机器人1血量低字节][机器人1血量高字节][机器人2血量低高]... → 敌方血量区
// [空中支援呼叫标志 1B]
// [冷却剩余秒数 1B]
// [调试计数器 1B]
// enemy_health：span，存放多个敌方机器人的uint16血量；state：支援状态机；debug_counter：调试用自增计数
std::array<uint8_t, kCustomControlPayloadSize> BuildCustomControlPayload(
    std::span<const uint16_t, kCustomControlRobotCount> enemy_health,
    AirSupportCustomControlState &state,
    uint8_t debug_counter,
    std::chrono::steady_clock::time_point now) {
    // 定长数组，全部初始填充0，避免脏数据
    std::array<uint8_t, kCustomControlPayloadSize> data{};

    // 循环写入每一台敌方机器人血量，uint16血量拆成两个字节，【小端序】
    for (size_t k = 0; k < kCustomControlRobotCount; k++) {
        const uint16_t hp = enemy_health[k];
        data[k * 2]     = static_cast<uint8_t>(hp & 0xFF);       // 低字节放前面（小端）
        data[k * 2 + 1] = static_cast<uint8_t>((hp >> 8) & 0xFF);// 高字节放后面
    }

    // 填充支援呼叫标志：消费式读取，事件只上报一次
    data[kCustomControlAirSupportCallOffset] = ConsumeAirSupportCallFlag(state);
    // 填充冷却剩余秒数字节（255代表无冷却）
    data[kCustomControlCounterCooldownOffset] = GetAirSupportCounterCooldownByte(state, now);
    // 填充调试计数器，用来排查丢包、乱序
    data[kCustomControlDebugCounterOffset] = debug_counter;

    return data;
}

// 功能：判断这一时刻该不该发布这条自定义控制载荷（用来控制MQTT发包频率）
// 思路：节流逻辑，防止高频刷屏发包；
// 条件：从来没发过 或者 当前距离上一次发布的时间 >= 设定上报间隔（比如200ms）
bool IsCustomControlPublishDue(std::optional<std::chrono::steady_clock::time_point> last_publish_time,
                              std::chrono::steady_clock::time_point now) {
    // last_publish_time无值 = 从未发布，允许发；否则判断间隔是否达标
    return !last_publish_time.has_value() || now - *last_publish_time >= kCustomControlPublishInterval;
}
