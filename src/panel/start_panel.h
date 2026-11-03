// 开局准备面板（赛前配置）声明：选择队伍/机器人、输入 MQTT 服务器地址、登录连接，并发送各类赛前指令。
#pragma once

#include "main_window.h"  // 提供 State 全局状态类型
#include "io/mqtt_client.h"  // 提供 MqttClient，用于连接服务器与发送赛前指令

// 绘制开局准备面板；仅当未进入比赛时由主窗口调用。
// udp_video_inverted：当前 UDP 画面是否已 180 度翻转（用于按钮回显）。
// 返回：本帧用户是否点了「UDP 画面反转」按钮，由调用方翻转实际方向并写日志。
bool StartPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, bool udp_video_inverted);
