// 比赛全局类 MQTT 消息处理（协议 2.2.3 ~ 2.2.7）：比赛状态、全局单位、后勤、特殊机制、事件。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]
#include "mqtt/handlers.h"

#include "app/robot_registry.h"      // kRobotTypeToSlot：兵种 -> 本地槽位映射
#include "app/team_util.h"           // IsRedRobotId：robot_id 阵营判定单一来源
#include "logging/logging.h"         // SPDLOG_*
#include "mqtt/event_formatter.h"    // Event 文案翻译与顶部提示入队

namespace RoboGo::mqtt {

// 2.2.3 比赛全局状态（1Hz）：局号、比分、阶段、阶段剩余秒数、胜负。
/**
 * @brief 解析并覆盖比赛全局状态快照，供顶部胶囊与阶段提示读取。
 * @param state 全局状态，写入 game_status。
 * @param topic 话题名，仅用于 ParseInto 的解析日志；值恒为 "GameStatus"。
 * @param payload protobuf 序列化的 GameStatus 原始字节。
 * @note 由分发表 DispatchMqttMessage 在主线程回调，1Hz 到达；解析失败保留上一次快照，界面不闪回初始值。
 * @note 2027 移除空中支援后，原「进入比赛阶段重置反制次数」的上升沿逻辑已一并删除。
 */
void HandleGameStatus(State &state, std::string_view topic, const std::string &payload) {
    GameStatus game_status;
    if (!ParseInto(topic, payload, game_status)) {
        return;
    }
    // 【2027 变更】空中支援已移除，原「进入比赛中即重置空中支援反制次数」的上升沿处理一并删除。
    state.game_status = std::move(game_status);
}

// 2.2.4 全局单位状态（1Hz）：双方基地/前哨站血量、每方 4 台机器人血量与弹量、总伤害与控制度。
// 注意：本函数在赋值后仍要读 robot_health()/robot_bullets()，因此不能把消息 move 进 State。
void HandleGlobalUnitStatus(State &state, std::string_view topic, const std::string &payload) {
    GlobalUnitStatus global_unit_status;
    if (!ParseInto(topic, payload, global_unit_status)) {
        return;
    }
    // 不能 move：后面还要从这份消息里读 robot_health() / robot_bullets() 派生双方面板数据。
    state.global_unit_status = global_unit_status;

    // 同一份数据驱动左右双方血量/弹量面板，也是 CustomControl 敌方血量透传的数据源。
    const auto &robot_health = global_unit_status.robot_health();
    const auto update_robot_health = [](RobotStatus &robot, uint32_t health) {
        robot.health = health;                                  // 血量会因受伤下降，也会因补血上升
        robot.max_health = std::max(robot.max_health, health);  // 只向上更新，作为血条 100% 的基准
    };
    // 注意：缺静态状态时这里按蓝方处理（与 WeAreRed 的「缺数据按红方」不同），故用 IsRedRobotId 而非 WeAreRed。
    const bool is_red_team = state.robot_static_status.has_value()
                             && RoboGo::app::team_util::IsRedRobotId(state.robot_static_status->robot_id());
    const auto robot_health_size = robot_health.size();
    const auto half_robot_health_size = robot_health_size / 2;

    // 【2027 规范】robot_health 顺序：己方 1,3,4,7 + 对方 1,3,4,7（每方 4 台，已去掉 2 号工程）。
    // 本地数组槽位：0=重装(1) 1=保留(2) 2=步兵(3) 3=步兵(4) 4=废弃步兵(5) 5=空中(6) 6=哨兵(7) 7=飞镖(8) 8=雷达(9)。
    // 旧协议（每方 5 台，顺序 1,2,3,4,7）对应槽位 0,1,2,3,6，这里自适应兼容两种长度。
            // 2027 每方 4 台（1/3/4/7），旧协议每方 5 台（含 2 号工程）；
    // 用 kRobotTypeToSlot 把「兵种编号」翻到本地数组槽位，两种长度都能兼容。
    const int client_indices_2027[] = {
            RoboGo::app::robot_registry::kRobotTypeToSlot[1],
            RoboGo::app::robot_registry::kRobotTypeToSlot[3],
            RoboGo::app::robot_registry::kRobotTypeToSlot[4],
            RoboGo::app::robot_registry::kRobotTypeToSlot[7]};
    const int client_indices_legacy[] = {0, 1, 2, 3, 6};
    const int *client_indices = half_robot_health_size == 4 ? client_indices_2027 : client_indices_legacy;
    const int health_count = std::min<int>(half_robot_health_size, half_robot_health_size == 4 ? 4 : 5);
    const int enemy_offset = half_robot_health_size;

    // 协议约定：数组前半段己方、后半段对方；同一份数据同时驱动左右两侧血量块。
    for (int i = 0; i < health_count; i++) {
        // 协议约定：前半段为己方，后半段为对方。
        const int self_index = i;
        const int enemy_index = i + enemy_offset;
        const int client_index = client_indices[i];
        if (is_red_team) {
            update_robot_health(state.red_robots[client_index], robot_health[self_index]);
            update_robot_health(state.blue_robots[client_index], robot_health[enemy_index]);
        } else {
            update_robot_health(state.blue_robots[client_index], robot_health[self_index]);
            update_robot_health(state.red_robots[client_index], robot_health[enemy_index]);
        }
    }

    // 处理弹量
    const auto &robot_bullets = global_unit_status.robot_bullets();
    const auto robot_bullets_size = robot_bullets.size();
    const auto half_robot_bullets_size = robot_bullets_size / 2;
    for (int i = 0; i < half_robot_bullets_size; i++) {
        int index = i;  // 默认认为协议顺序与本地数组一一对应
        // 官方未明确说明弹量数组顺序，这里按每方长度做容错映射。
        if (half_robot_bullets_size == 4) {
            index = client_indices_2027[i];  // 2027：1,3,4,7 -> 槽位 0,2,3,6
        } else if (half_robot_bullets_size == 6) {
            // 1 2 3 4 6 7：第 5 项起是 6 号和 7 号，跳过本地 5 号（废弃步兵）槽位
            if (i >= 4) {
                index = i + 1;
            }
        } else if (half_robot_bullets_size == 5) {
            // 1 3 4 6 7：第 4、5 项后移两位（跳过 2 号工程与 5 号），第 2、3 项后移一位
            if (i >= 3) {
                index = i + 2;
            } else if (i >= 1) {
                index = i + 1;
            }
        }

        int red_index;
        int blue_index;
        if (is_red_team) {
            red_index = i;
            blue_index = i + half_robot_bullets_size;
        } else {
            red_index = i + half_robot_bullets_size;
            blue_index = i;
        }
        if (red_index < robot_bullets_size) {
            state.red_robots[index].bullets = robot_bullets[red_index];
        }
        if (blue_index < robot_bullets_size) {
            state.blue_robots[index].bullets = robot_bullets[blue_index];
        }
    }
}

// 2.2.5 全局后勤信息（1Hz）：剩余经济、累计经济、科技等级、加密等级。
/**
 * @brief 解析并覆盖全局后勤快照，供开局面板显示经济、科技与加密等级。
 * @param state 全局状态，写入 global_logistics_status。
 * @param topic 话题名，仅用于解析日志；值恒为 "GlobalLogisticsStatus"。
 * @param payload protobuf 序列化的 GlobalLogisticsStatus 原始字节。
 * @note 该消息只有经济/科技/加密三类字段，**不含「是否在补给区」**；兑换允许发弹量按钮的
 *       唯一解锁依据是 Buff type=6，不能从这里推断。
 * @note 1Hz 推送；ParseInto 失败直接返回并保留旧值，避免按钮状态抖动。
 */
void HandleGlobalLogisticsStatus(State &state, std::string_view topic, const std::string &payload) {
    GlobalLogisticsStatus global_logistics_status;
    if (ParseInto(topic, payload, global_logistics_status)) {
        state.global_logistics_status = std::move(global_logistics_status);
    }
}

// 2.2.6 全局特殊机制（1Hz）：堡垒占领计时等机制 ID 与时间参数。
void HandleGlobalSpecialMechanism(State &state, std::string_view topic, const std::string &payload) {
    GlobalSpecialMechanism global_special_mechanism;
    if (ParseInto(topic, payload, global_special_mechanism)) {
        state.global_special_mechanism = std::move(global_special_mechanism);
    }
}

// 2.2.7 比赛事件（触发式）：击杀、前哨站被毁、能量机关、阵地控制、装配结果等。
// 事件既要写进系统日志，重要事件还要推一条顶部提示（2027 的致盲等窗口只有几秒，日志来不及看）。
void HandleEvent(State &state, std::string_view topic, const std::string &payload) {
    Event event;
    if (!ParseInto(topic, payload, event)) {
        return;
    }
    //FormatEventText表示格式化事件文本，用于系统日志和顶部提示
    const std::string text = FormatEventText(event);
    AppendSystemLog(state, 0, text);                       // 事件日志统一 INFO 级
    PushEventToast(state, event.event_id(), text);          // 按事件号决定是否弹提示
}

}  // namespace RoboGo::mqtt


