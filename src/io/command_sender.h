// 下行指令统一出口：序列化、发布、日志只在这里实现一次。
// 各面板不再各写一套 Publish 模板 / Send 封装，新增一种指令只需在这里加一个构造器。
#pragma once

#include <pch.h>
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]

#include "io/mqtt_client.h"

namespace RoboGo::io {

// 统一的指令发布：序列化成功才发布，并打一条 INFO 摘要（话题 + 字节数）。
// 返回是否成功，调用方可据此决定是否给用户反馈。
template<typename Message>
/**
 * @brief 序列化并发布一条上行 protobuf 指令，是全项目指令发布的唯一出口
 * @param mqtt_client MQTT 客户端。
 * @param topic 上行话题名。
 * @param message 已填好字段的 protobuf 消息。
 * @note 序列化失败只打 ERROR 并返回 false；成功则打一条 INFO 摘要（话题 + 字节数）。
 * @note 各面板不再各写一套 Publish/Send 封装，新增指令只需在本文件加一个构造器。
 */
bool PublishCommand(MqttClient &mqtt_client, const char *topic, const Message &message) {
    std::string serialized;
    if (!message.SerializeToString(&serialized)) {
        SPDLOG_ERROR("指令序列化失败 topic={}", topic);
        return false;
    }
    // publish() 内部还会再判一次 connection_state，未订阅成功时指令被静默丢弃。
    mqtt_client.publish(topic, serialized);
    SPDLOG_INFO("下发指令 topic={} bytes={}", topic, serialized.size());
    return true;
}

/**
 * @brief 通用指令（CommonCommand）：cmd_type 区分回血 / 复活 / 弹药兑换等，param 为附带参数（如发弹量）。，目标话题 CommonCommand
 * @param mqtt_client MQTT 客户端，最终由 publish() 投递。
 * @note 统一走 PublishCommand：序列化失败只记 ERROR 并返回 false，不会误发空包。
 * @note publish() 内部还会再判一次 connection_state，订阅完成前的指令会被静默丢弃。
 */
inline void SendCommonCommand(MqttClient &mqtt_client, uint32_t cmd_type, uint32_t param = 0) {
    CommonCommand command;
    command.set_cmd_type(cmd_type);
    command.set_param(param);
    // CommonCommand 的语义由 param 承载：兑换、回血、复活等通用动作。
    PublishCommand(mqtt_client, "CommonCommand", command);
}

/**
 * @brief 哨兵控制指令（SentryCtrlCommand）：1 补弹 / 3 远程补弹 / 4 远程回血 / 5 确认复活 / 6 金币复活 / 7-12 切换姿态。，目标话题 SentryCtrlCommand
 * @param mqtt_client MQTT 客户端，最终由 publish() 投递。
 * @note 统一走 PublishCommand：序列化失败只记 ERROR 并返回 false，不会误发空包。
 * @note publish() 内部还会再判一次 connection_state，订阅完成前的指令会被静默丢弃。
 */
inline void SendSentryCtrlCommand(MqttClient &mqtt_client, uint32_t command_id) {
    SentryCtrlCommand command;
    command.set_command_id(command_id);
    PublishCommand(mqtt_client, "SentryCtrlCommand", command);
}

/**
 * @brief 飞镖指令（DartCommand）：目标编号、闸门开关、是否确认发射。，目标话题 DartCommand
 * @param mqtt_client MQTT 客户端，最终由 publish() 投递。
 * @note 统一走 PublishCommand：序列化失败只记 ERROR 并返回 false，不会误发空包。
 * @note publish() 内部还会再判一次 connection_state，订阅完成前的指令会被静默丢弃。
 */
inline void SendDartCommand(MqttClient &mqtt_client, uint32_t target_id, bool open, bool launch_confirm) {
    DartCommand command;
    command.set_target_id(target_id);
    command.set_open(open);
    command.set_launch_confirm(launch_confirm);
    // 飞镖指令只带目标与发射意图；闸门状态由 DartSelectTargetStatusSync 下行同步。
    PublishCommand(mqtt_client, "DartCommand", command);
}

/**
 * @brief 装配指令（AssemblyCommand）：difficulty 难度，operation 0 开始兑换 / 1 确认装配 / 2 取消装配。，目标话题 AssemblyCommand
 * @param mqtt_client MQTT 客户端，最终由 publish() 投递。
 * @note 统一走 PublishCommand：序列化失败只记 ERROR 并返回 false，不会误发空包。
 * @note publish() 内部还会再判一次 connection_state，订阅完成前的指令会被静默丢弃。
 */
inline void SendAssemblyCommand(MqttClient &mqtt_client, uint32_t difficulty, uint32_t operation) {
    AssemblyCommand command;
    command.set_difficulty(difficulty);
    command.set_operation(operation);
    PublishCommand(mqtt_client, "AssemblyCommand", command);
}

}  // namespace RoboGo::io



