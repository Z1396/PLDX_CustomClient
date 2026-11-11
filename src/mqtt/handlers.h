// 各 MQTT topic 的处理函数声明：按领域分文件实现，分发表在 mqtt_router.cpp。
// 本文件同时充当「已支持话题」的索引——新增协议消息时，先在这里加一行，再去对应领域文件实现。
#pragma once

#include "mqtt/mqtt_router.h"

namespace RoboGo::mqtt {

// ---------- handlers_match.cpp：比赛全局类（协议 2.2.3 ~ 2.2.7）----------
// 处理比赛全局状态
void HandleGameStatus(State &state, std::string_view topic, const std::string &payload);
// 处理全局单位状态
void HandleGlobalUnitStatus(State &state, std::string_view topic, const std::string &payload);
// 处理全局物流状态
void HandleGlobalLogisticsStatus(State &state, std::string_view topic, const std::string &payload);
// 处理全局特殊机制状态
void HandleGlobalSpecialMechanism(State &state, std::string_view topic, const std::string &payload);
// 处理全局事件状态
void HandleEvent(State &state, std::string_view topic, const std::string &payload);

// ---------- handlers_robot.cpp：本机与友军状态类（协议 2.2.8 ~ 2.2.13、2.2.16）----------
// 处理本机与友军状态
void HandleRobotInjuryStat(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军复活状态
void HandleRobotRespawnStatus(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军静态状态
void HandleRobotStaticStatus(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军动态状态
void HandleRobotDynamicStatus(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军模块状态
void HandleRobotModuleStatus(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军位置状态
void HandleRobotPosition(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军路径计划状态
void HandleRobotPathPlanInfo(State &state, std::string_view topic, const std::string &payload);

// ---------- handlers_ui.cpp：界面侧机制与指令回执（协议 2.2.14 起）----------
// 处理本机与友军状态
void HandleBuff(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军惩罚状态
void HandlePenaltyInfo(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军地图点击状态
void HandleMapClickInfo(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军雷达状态
void HandleRadarInfoToClient(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军技术核心运动状态
void HandleTechCoreMotionStateSync(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军性能选择状态
void HandleRobotPerformanceSelectionSync(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军运行状态
void HandleRuneStatusSync(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军哨兵状态
void HandleSentryStatusSync(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军dart选择目标状态
void HandleDartSelectTargetStatusSync(State &state, std::string_view topic, const std::string &payload);
// 处理本机与友军哨兵控制结果
void HandleSentryCtrlResult(State &state, std::string_view topic, const std::string &payload);

}  // namespace RoboGo::mqtt
