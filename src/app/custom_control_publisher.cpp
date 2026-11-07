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

void PublishCustomControlIfDue(State &state, RoboGo::io::MqttClient &mqtt_client) {
    const auto now = std::chrono::steady_clock::now();
    if (!IsCustomControlPublishDue(state.last_custom_control_publish_time, now)) {
        return;
    }
    // 缺任一关键快照都无法定位敌方，直接跳过本次下发。
    if (!state.robot_static_status.has_value() || !state.global_unit_status.has_value()) {
        return;
    }

    // 血量数组前半为己方、后半为对方。2027 每方 4 台(1,3,4,7)，旧协议每方 5 台，故只做基本尺寸校验。
    const auto &robot_health = state.global_unit_status->robot_health();
    const auto robot_health_size = robot_health.size();
    const auto half_robot_health_size = robot_health_size / 2;
    if (half_robot_health_size < 4 || robot_health_size < half_robot_health_size * 2) {
        return;  // 数据不足，越界防呆
    }

    std::array<uint16_t, kCustomControlRobotCount> enemy_health{};
    if (half_robot_health_size == 4) {
        // 2027 标准：段内下标 0..3 对应 1/3/4/7 号；
        // 载荷槽位固定为 重装(0)/保留(1)/步兵3(2)/步兵4(3)/哨兵(4)，因此映射为 0←1号, 2←3号, 3←4号, 4←7号。
        constexpr int kEnemySlotFromIndex[4] = {0, 2, 3, 4};
        for (int k = 0; k < 4; k++) {
            // 协议里血量是 uint32，下行包只有 2 字节，先钳位再收窄。
            const uint16_t hp = static_cast<uint16_t>(
                    std::min(robot_health[half_robot_health_size + k], uint32_t{65535}));
            enemy_health[kEnemySlotFromIndex[k]] = hp;
        }
        enemy_health[1] = 0;  // 工程槽位在 2027 血量数组中已不存在，先填 0，槽位定义待机器人端确认
    } else if (robot_health_size >= half_robot_health_size + static_cast<int>(kCustomControlRobotCount)) {
        // 旧协议容错：每方 ≥5 台时按旧顺序直取 5 台
        for (size_t k = 0; k < kCustomControlRobotCount; k++) {
            const int enemy_index = half_robot_health_size + static_cast<int>(k);
            const uint16_t hp = static_cast<uint16_t>(
                    std::min(robot_health[enemy_index], uint32_t{65535}));
            enemy_health[k] = hp;
        }
    } else {
        return;  // 段长度既不是 2027 的 4，也不够旧协议的 5
    }

    // 自增调试计数器：下位机据此判断控制包的新鲜度，uint8_t 自然回绕 255→0。
    static uint8_t debug_increment_counter = 0;
    const uint8_t current_debug_counter = debug_increment_counter;
    debug_increment_counter = static_cast<uint8_t>(debug_increment_counter + 1);

    const auto data = BuildCustomControlPayload(enemy_health, current_debug_counter);
    CustomControl custom_control;
    custom_control.set_data(std::string(data.begin(), data.end()));
    std::string serialized;
    if (custom_control.SerializeToString(&serialized)) {
        mqtt_client.publish("CustomControl", serialized);
        state.last_custom_control_publish_time = now;
    }  // 序列化失败则保持上次时间戳，下帧重试

    // 【2027 变更】原空中支援的「呼叫标志 / 反制冷却」两字节已改为保留位并固定填 0，不再打印。
    // 日志只在内容真正变化时输出：原来按 50Hz 打印（实测 35 秒 1734 行，一场约 2 万行），
    // 会把真正的 ERROR 淹没。比较时必须跳过每包自增的调试计数器，否则每包都算「变化」。
    static std::vector<uint8_t> last_logged_payload;
    static uint64_t unchanged_skips = 0;
    bool payload_changed = last_logged_payload.size() != data.size();  // 尺寸变了（协议改过）也算变化
    if (!payload_changed) {
        for (std::size_t index = 0; index < data.size(); ++index) {
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
        SPDLOG_INFO("CustomControl 已发出: 重装={} 保留={} 步兵3={} 步兵4={} 哨兵={} 计数器={} 省略={}",
                    enemy_health[0], enemy_health[1], enemy_health[2], enemy_health[3], enemy_health[4],
                    data[kCustomControlDebugCounterOffset], unchanged_skips);
        last_logged_payload.assign(data.begin(), data.end());
        unchanged_skips = 0;
    } else {
        ++unchanged_skips;
    }
}
