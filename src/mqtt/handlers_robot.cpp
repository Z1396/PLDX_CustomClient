// 本机与友军状态类 MQTT 消息处理（协议 2.2.8 ~ 2.2.13、2.2.16）。
// 这些消息都是「解析成功即覆盖 State 快照」，没有额外副作用。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]
#include "mqtt/handlers.h"

namespace RoboGo::mqtt {

// 2.2.8 机器人受伤统计（1Hz）：各类扣血明细与击杀者。
void HandleRobotInjuryStat(State &state, std::string_view topic, const std::string &payload) {
    RobotInjuryStat message;
    if (ParseInto(topic, payload, message)) {
        state.robot_injury_stat = std::move(message);
    }
}

// 2.2.9 复活状态（1Hz）：是否待复活、读条进度、金币复活条件。
void HandleRobotRespawnStatus(State &state, std::string_view topic, const std::string &payload) {
    RobotRespawnStatus message;
    if (ParseInto(topic, payload, message)) {
        state.robot_respawn_status = std::move(message);
    }
}

// 2.2.10 机器人静态属性（1Hz）：robot_id / 兵种 / 等级 / 最大血量热量等。
// robot_id 决定本机红蓝阵营，是判断敌我的依据。
void HandleRobotStaticStatus(State &state, std::string_view topic, const std::string &payload) {
    RobotStaticStatus message;
    if (ParseInto(topic, payload, message)) {
        state.robot_static_status = std::move(message);
    }
}

// 2.2.11 机器人实时数据（10Hz）：血量 / 热量 / 缓冲能量 / 弹量等，是 HUD 实时数值的直接来源。
void HandleRobotDynamicStatus(State &state, std::string_view topic, const std::string &payload) {
    RobotDynamicStatus message;
    if (ParseInto(topic, payload, message)) {
        state.robot_dynamic_status = std::move(message);
    }
}

// 2.2.12 模块运行状态（1Hz）：11 个模块的在线状态。
void HandleRobotModuleStatus(State &state, std::string_view topic, const std::string &payload) {
    RobotModuleStatus message;
    if (ParseInto(topic, payload, message)) {
        state.robot_module_status = std::move(message);
    }
}

// 2.2.13 机器人坐标与朝向（1Hz）：世界坐标 X/Y/Z 与 yaw 角。
void HandleRobotPosition(State &state, std::string_view topic, const std::string &payload) {
    // TODO 文档没有把坐标系说清楚，需要实际测试。猜测也许与雷达站是一样的？
    RobotPosition message;
    if (ParseInto(topic, payload, message)) {
        state.robot_position = std::move(message);
    }
}

// 2.2.16 哨兵轨迹规划（触发式）：意图 + 49 点相对位移。
void HandleRobotPathPlanInfo(State &state, std::string_view topic, const std::string &payload) {
    RobotPathPlanInfo message;
    if (ParseInto(topic, payload, message)) {
        state.robot_path_plan_info = std::move(message);
    }
}

}  // namespace RoboGo::mqtt
