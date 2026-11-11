// 自定义控制包（CustomControl）下发实现。
//
// 这 13 字节是客户端与机器人电控之间的私有约定：5 个 uint16 小端血量槽 + 2 字节保留 + 1 字节调试计数。
// 任何格式改动都必须与电控同步，否则机器人端会读错位。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]
#include "app/custom_control_publisher.h"

#include "custom_control_payload.h"  // BuildCustomControlPayload / 槽位与偏移常量
#include "logging/logging.h"

/**
 * @brief 按需下发 CustomControl 自定义控制包（敌方血量透传包）
 * @param state 全局状态容器，保存战场状态、上一次发包时间、机器人静态信息等
 * @param mqtt_client MQTT客户端，用于把序列化后的protobuf消息发布到 "CustomControl" topic
 * @note 下发频率受 IsCustomControlPublishDue 控制（不是无脑每帧发，做节流控频）
 * @note 依赖全局战场状态 global_unit_status（protobuf消息，里面repeated robot_health是机器人血量数组）
 * @note 2027协议：每方4台机器人 [1重装,3步兵,4步兵,7哨兵]；旧协议：每方5台（含2工程）
 * @note 下发包内包含一个自增uint8计数器，下位机用来判断包是否新鲜、丢包、时序错乱
 */
void PublishCustomControlIfDue(State &state, RoboGo::io::MqttClient &mqtt_client) {
    // 获取当前稳态时钟时间（steady_clock 不受系统时间回拨影响，适合做间隔计时）
    const auto now = std::chrono::steady_clock::now();

    // 判断是否到了该发包的时间；未到时间直接return，节流，避免高频刷屏发包
    if (!IsCustomControlPublishDue(state.last_custom_control_publish_time, now)) {
        return;
    }

    // 前置校验：缺少【机器人静态信息】或者【全局战场状态】，没有敌方血量数据源，直接跳过本次下发
    // 这两个快照必须同时就绪，否则无法正确解析、映射敌方血量
    if (!state.robot_static_status.has_value() || !state.global_unit_status.has_value()) {
        return;
    }

    // 拿到protobuf里repeated数组 robot_health：顺序【己方机器人血量 + 对方机器人血量】拼接
    const auto &robot_health = state.global_unit_status->robot_health();
    const auto robot_health_size = robot_health.size();
    // 对半切分：前半段 = 己方，后半段 = 敌方
    const auto half_robot_health_size = robot_health_size / 2;

    // 基础防呆校验：
    // 1. 每方至少4个数据（2027标准最低要求）
    // 2. 总长度必须等于两半之和（防止奇数长度畸形包）
    if (half_robot_health_size < 4 || robot_health_size < half_robot_health_size * 2) {
        return;  // 数据残缺，直接放弃本次下发，防止数组越界
    }

    // 目标输出数组：敌方血量，固定槽位，给到 BuildCustomControlPayload 组装载荷
    // kCustomControlRobotCount：包定义里支持的机器人数量（2027是5个槽：重装、保留工程、步兵3、步兵4、哨兵）
    std::array<uint16_t, kCustomControlRobotCount> enemy_health{}; // 零初始化，默认全部填0

    if (half_robot_health_size == 4) {
        // ====================== 2027 新版协议分支 ======================
        // robot_health后半段敌方顺序：下标0→1号重装、1→3号步兵、2→4号步兵、3→7号哨兵
        // 输出enemy_health载荷槽位映射：
        // enemy_health[0] → 重装(1号)
        // enemy_health[1] → 保留工程槽（2027协议已经没有工程，固定填0）
        // enemy_health[2] → 步兵3(3号)
        // enemy_health[3] → 步兵4(4号)
        // enemy_health[4] → 哨兵(7号)
        constexpr int kEnemySlotFromIndex[4] = {0, 2, 3, 4};
        for (int k = 0; k < 4; k++) {
            // half_robot_health_size + k：取后半段敌方血量
            // 血量原始类型是uint32，下行包只用2字节uint16，所以上限钳位到65535，防止溢出截断负数
            const uint16_t hp = static_cast<uint16_t>(
                std::min(robot_health[half_robot_health_size + k], uint32_t{65535}));
            // 映射到输出数组对应的槽位
            enemy_health[kEnemySlotFromIndex[k]] = hp;
        }
        enemy_health[1] = 0;  // 工程槽位在2027协议里已经不存在，该槽位保留，固定填0，等待下位机确认定义
    } else if (robot_health_size >= half_robot_health_size + static_cast<int>(kCustomControlRobotCount)) {
        // ====================== 旧协议容错分支 ======================
        // 旧协议每方≥5台机器人，直接按顺序依次取敌方5台血量，一一对应填入enemy_health
        for (size_t k = 0; k < kCustomControlRobotCount; k++) {
            const int enemy_index = half_robot_health_size + static_cast<int>(k);
            const uint16_t hp = static_cast<uint16_t>(
                std::min(robot_health[enemy_index], uint32_t{65535}));
            enemy_health[k] = hp;
        }
    } else {
        // 既不是2027的4台模式，长度又不够旧协议5台，协议版本无法识别，放弃下发
        return;
    }

    // 自增包计数器，uint8_t，溢出自动回绕 255→0
    // 下位机使用这个计数器判断包是否连续、是否新鲜、是否丢包
    static uint8_t debug_increment_counter = 0;
    const uint8_t current_debug_counter = debug_increment_counter;
    debug_increment_counter = static_cast<uint8_t>(debug_increment_counter + 1);

    // 组装原始二进制载荷（敌方血量数组 + 计数器 + 保留位等）
    const auto data = BuildCustomControlPayload(enemy_health, current_debug_counter);
    // 填充protobuf消息，把原始二进制载荷塞进data字段
    CustomControl custom_control;
    custom_control.set_data(std::string(data.begin(), data.end()));

    std::string serialized;
    // protobuf序列化：对象 -> 二进制字符串
    if (custom_control.SerializeToString(&serialized)) {
        // MQTT发布到主题 CustomControl，下位机订阅这个topic收包
        mqtt_client.publish("CustomControl", serialized);
        // 更新发包时间戳，作为下一次节流判断的基准
        state.last_custom_control_publish_time = now;
    }
    // 序列化失败：不更新时间戳，下一轮到时间会重试发包

    // ====================== 日志防抖逻辑（重点，避免日志洪水） ======================
    // 需求：只有载荷【业务内容真正变化】才打印INFO日志；
    // 注意：计数器每包都会+1，属于变化但是无业务意义，对比的时候跳过计数器偏移位置
    static std::vector<uint8_t> last_logged_payload; // 静态变量，保存上一次打印的载荷
    static uint64_t unchanged_skips = 0;             // 连续无变化跳过的包计数

    bool payload_changed = last_logged_payload.size() != data.size(); // 载荷长度变了直接判定为变更（协议改动）
    if (!payload_changed) {
        // 长度相同，逐字节对比
        for (std::size_t index = 0; index < data.size(); ++index) {
            // 跳过计数器字节，这个字节每包自增，不算业务变化
            if (index == kCustomControlDebugCounterOffset) {
                continue;
            }
            if (data[index] != last_logged_payload[index]) {
                payload_changed = true;
                break;
            }
        }
    }

    if (payload_changed) {
        // 业务数据发生变化：打印日志，更新缓存，清零跳过计数
        SPDLOG_INFO("CustomControl 已发出: 重装={} 保留={} 步兵3={} 步兵4={} 哨兵={} 计数器={} 省略={}",
                    enemy_health[0], enemy_health[1], enemy_health[2], enemy_health[3], enemy_health[4],
                    data[kCustomControlDebugCounterOffset], unchanged_skips);
        last_logged_payload.assign(data.begin(), data.end());
        unchanged_skips = 0;
    } else {
        // 业务数据完全没变：跳过打印，计数累加，防止日志刷屏淹没ERROR
        ++unchanged_skips;
    }
}

