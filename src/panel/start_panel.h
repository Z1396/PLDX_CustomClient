// 开局准备面板（赛前配置）声明：选择队伍/机器人、输入 MQTT 服务器地址、登录连接，并发送各类赛前指令。
#pragma once

#include "main_window.h"
#include "io/mqtt_client.h"

// 绘制开局准备面板；仅当未进入比赛时由主窗口调用。
void StartPanel(const State &state, RoboGo::io::MqttClient &mqtt_client);
