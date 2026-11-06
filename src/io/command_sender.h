// 下行指令统一出口：序列化、发布、日志只在这里实现一次。
// 各面板不再各写一套 Publish 模板 / Send 封装，新增一种指令只需在这里加一个构造器。
#pragma once

#include <pch.h>

#include "io/mqtt_client.h"

namespace RoboGo::io {

// 统一的指令发布：序列化成功才发布，并打一条 INFO 摘要（话题 + 字节数）。
// 返回是否成功，调用方可据此决定是否给用户反馈。
template<typename Message>
bool PublishCommand(MqttClient &mqtt_client, const char *topic, const Message &message) {
    std::string serialized;
    if (!message.SerializeToString(&serialized)) {
        SPDLOG_ERROR("指令序列化失败 topic={}", topic);
        return false;
    }
    mqtt_client.publish(topic, serialized);
    SPDLOG_INFO("下发指令 topic={} bytes={}", topic, serialized.size());
    return true;
}

// 通用指令（CommonCommand）：cmd_type 区分回血 / 复活 / 弹药兑换等，param 为附带参数（如发弹量）。
inline void SendCommonCommand(MqttClient &mqtt_client, uint32_t cmd_type, uint32_t param = 0) {
    CommonCommand command;
    command.set_cmd_type(cmd_type);
    command.set_param(param);
    PublishCommand(mqtt_client, "CommonCommand", command);
}

// 哨兵控制指令（SentryCtrlCommand）：1 补弹 / 3 远程补弹 / 4 远程回血 / 5 确认复活 / 6 金币复活 / 7-12 切换姿态。
inline void SendSentryCtrlCommand(MqttClient &mqtt_client, uint32_t command_id) {
    SentryCtrlCommand command;
    command.set_command_id(command_id);
    PublishCommand(mqtt_client, "SentryCtrlCommand", command);
}

// 飞镖指令（DartCommand）：目标编号、闸门开关、是否确认发射。
inline void SendDartCommand(MqttClient &mqtt_client, uint32_t target_id, bool open, bool launch_confirm) {
    DartCommand command;
    command.set_target_id(target_id);
    command.set_open(open);
    command.set_launch_confirm(launch_confirm);
    PublishCommand(mqtt_client, "DartCommand", command);
}

// 装配指令（AssemblyCommand）：difficulty 难度，operation 0 开始兑换 / 1 确认装配 / 2 取消装配。
inline void SendAssemblyCommand(MqttClient &mqtt_client, uint32_t difficulty, uint32_t operation) {
    AssemblyCommand command;
    command.set_difficulty(difficulty);
    command.set_operation(operation);
    PublishCommand(mqtt_client, "AssemblyCommand", command);
}

}  // namespace RoboGo::io
