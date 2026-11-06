// 各 MQTT topic 的处理函数声明：按领域分文件实现，分发表在 mqtt_router.cpp。
// 本文件同时充当「已支持话题」的索引——新增协议消息时，先在这里加一行，再去对应领域文件实现。
#pragma once

#include "mqtt/mqtt_router.h"

namespace RoboGo::mqtt {

// ---------- handlers_match.cpp：比赛全局类（协议 2.2.3 ~ 2.2.7）----------
void HandleGameStatus(State &state, std::string_view topic, const std::string &payload);
void HandleGlobalUnitStatus(State &state, std::string_view topic, const std::string &payload);
void HandleGlobalLogisticsStatus(State &state, std::string_view topic, const std::string &payload);
void HandleGlobalSpecialMechanism(State &state, std::string_view topic, const std::string &payload);
void HandleEvent(State &state, std::string_view topic, const std::string &payload);

// ---------- handlers_robot.cpp：本机与友军状态类（协议 2.2.8 ~ 2.2.13、2.2.16）----------
void HandleRobotInjuryStat(State &state, std::string_view topic, const std::string &payload);
void HandleRobotRespawnStatus(State &state, std::string_view topic, const std::string &payload);
void HandleRobotStaticStatus(State &state, std::string_view topic, const std::string &payload);
void HandleRobotDynamicStatus(State &state, std::string_view topic, const std::string &payload);
void HandleRobotModuleStatus(State &state, std::string_view topic, const std::string &payload);
void HandleRobotPosition(State &state, std::string_view topic, const std::string &payload);
void HandleRobotPathPlanInfo(State &state, std::string_view topic, const std::string &payload);

// ---------- handlers_ui.cpp：界面侧机制与指令回执（协议 2.2.14 起）----------
void HandleBuff(State &state, std::string_view topic, const std::string &payload);
void HandlePenaltyInfo(State &state, std::string_view topic, const std::string &payload);
void HandleMapClickInfo(State &state, std::string_view topic, const std::string &payload);
void HandleRadarInfoToClient(State &state, std::string_view topic, const std::string &payload);
void HandleTechCoreMotionStateSync(State &state, std::string_view topic, const std::string &payload);
void HandleRobotPerformanceSelectionSync(State &state, std::string_view topic, const std::string &payload);
void HandleRuneStatusSync(State &state, std::string_view topic, const std::string &payload);
void HandleSentryStatusSync(State &state, std::string_view topic, const std::string &payload);
void HandleDartSelectTargetStatusSync(State &state, std::string_view topic, const std::string &payload);
void HandleSentryCtrlResult(State &state, std::string_view topic, const std::string &payload);

}  // namespace RoboGo::mqtt
