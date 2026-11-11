// 自定义控制载荷实现：将敌情打包成固定布局二进制载荷，同时控制MQTT上报发布节奏
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]
#include "custom_control_payload.h"  // 本模块头文件：载荷布局常量与函数声明

/**
 * @brief 组装 13 字节自定义控制载荷：5 个 uint16 小端血量槽 + 2 字节保留 + 1 字节调试计数。
 * @param enemy_health 定长 span，编译期保证正好 5 个血量槽，无需运行时长度检查。
 * @param debug_counter 自增调试计数，下位机据此判断包的新鲜度与丢包。
 * @return 定长 13 字节数组，可直接塞进 protobuf 的 CustomControl.data 字段。
 * @note 布局顺序固定：[血量0低][血量0高]…[血量4低][血量4高][保留][保留][计数]；
 *       每个 uint16 低字节在前（小端）。任何偏移改动都必须与机器人电控同步，否则下位机会读错位。
 * @note 2 个保留字节原为 2027 删除的「空中支援呼叫 / 反制冷却」，现固定写 0 但偏移保持不变。
 */
std::array<uint8_t, kCustomControlPayloadSize> BuildCustomControlPayload(  // 返回定长字节数组
    std::span<const uint16_t, kCustomControlRobotCount> enemy_health,  // 定长 span，保证正好 5 个血量值
    uint8_t debug_counter) {  // 调试自增计数
    // 定长数组，全部初始填充0，避免脏数据
    std::array<uint8_t, kCustomControlPayloadSize> data{};  // 值初始化：所有字节清零

    // 循环写入每一台敌方机器人血量，uint16血量拆成两个字节，【小端序】
    for (size_t k = 0; k < kCustomControlRobotCount; k++) {  // 遍历 5 台机器人
        const uint16_t hp = enemy_health[k];  // 取出第 k 台机器人的血量
        data[k * 2]     = static_cast<uint8_t>(hp & 0xFF);       // 低字节放前面（小端）
        data[k * 2 + 1] = static_cast<uint8_t>((hp >> 8) & 0xFF);  // 高字节放后面
    }

    // 2027 协议删除空中支援后，原"呼叫标志"与"冷却计数"两字节改为保留位：固定写 0，偏移不变
    data[kCustomControlAirSupportCallOffset] = 0;  // 保留字节：固定 0，待与机器人端协商后再定义用途
    data[kCustomControlCounterCooldownOffset] = 0;  // 保留字节：固定 0，待与机器人端协商后再定义用途
    // 填充调试计数器，用来排查丢包、乱序
    data[kCustomControlDebugCounterOffset] = debug_counter;  // 逐帧自增，便于观察到丢失

    return data; // 返回组装完成的定长字节载荷
}

/**
 * @brief 判断这一时刻该不该下发 CustomControl 载荷（10Hz 节流）。
 * @param last_publish_time 上次发布时刻，nullopt 表示从未发布。
 * @param now 当前 steady_clock 时刻，不受系统时间回拨影响。
 * @return true 表示应当立即发布；false 表示本轮跳过。
 * @note 间隔取 kCustomControlPublishInterval = 100ms（10Hz），常量见 custom_control_payload.h:18；
 *       旧注释曾误写「比如 200ms」，此处按常量更正。
 * @note 由 PublishCustomControlIfDue() 每帧调用；只有序列化成功才更新时间戳，
 *       因此失败时下一帧会重试，不会永久卡住。
 */
bool IsCustomControlPublishDue(std::optional<std::chrono::steady_clock::time_point> last_publish_time,  // 上次发布时刻，空=从未发布
                              std::chrono::steady_clock::time_point now) {  // 当前时刻
    // last_publish_time无值 = 从未发布，允许发；否则判断间隔是否达标
    return !last_publish_time.has_value() || now - *last_publish_time >= kCustomControlPublishInterval;  // 短路或：首次立即发，之后按间隔节流
}


