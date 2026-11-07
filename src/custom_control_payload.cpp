// 自定义控制载荷实现：将敌情打包成固定布局二进制载荷，同时控制MQTT上报发布节奏
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]
#include "custom_control_payload.h"  // 本模块头文件：载荷布局常量与函数声明

// 功能：组装完整自定义上报载荷，固定长度std::array，固定字段排布、小端编码
// 报文布局顺序：
// [机器人1血量低字节][机器人1血量高字节][机器人2血量低高]... → 敌方血量区
// [保留字节 1B]（2027 起空出，固定填 0）
// [保留字节 1B]（2027 起空出，固定填 0）
// [调试计数器 1B]
// enemy_health：span，存放多个敌方机器人的uint16血量；debug_counter：调试用自增计数
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

// 功能：判断这一时刻该不该发布这条自定义控制载荷（用来控制MQTT发包频率）
// 思路：节流逻辑，防止高频刷屏发包；
// 条件：从来没发过 或者 当前距离上一次发布的时间 >= 设定上报间隔（比如200ms）
bool IsCustomControlPublishDue(std::optional<std::chrono::steady_clock::time_point> last_publish_time,  // 上次发布时刻，空=从未发布
                              std::chrono::steady_clock::time_point now) {  // 当前时刻
    // last_publish_time无值 = 从未发布，允许发；否则判断间隔是否达标
    return !last_publish_time.has_value() || now - *last_publish_time >= kCustomControlPublishInterval;  // 短路或：首次立即发，之后按间隔节流
}
