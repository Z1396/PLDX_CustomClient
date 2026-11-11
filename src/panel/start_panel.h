// 开局准备面板（赛前配置）声明：选择队伍/机器人、输入 MQTT 服务器地址、登录连接，并发送各类赛前指令。
#pragma once

#include "main_window.h"  // 提供 State 全局状态类型
#include "io/mqtt_client.h"  // 提供 MqttClient，用于连接服务器与发送赛前指令

// 绘制开局准备面板；仅当未进入比赛时由主窗口调用。
// udp_video_inverted：当前 UDP 画面是否已 180 度翻转（用于按钮回显）。
// 返回：本帧用户是否点了「UDP 画面反转」按钮，由调用方翻转实际方向并写日志。
bool StartPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, bool udp_video_inverted);

/**
 * @brief 比赛中的"装配"浮层（仅重装可用）。为什么需要：装配按钮原来只在开局面板里，比赛开始后要按 Esc 退出比赛模式才能点到，而装配有 45 秒窗口，两次 Esc 的代价太大。交互：F1
 *        开关；数字键 1~4 选难度；Enter 执行当前可做的操作（核心空闲=开始兑换、已到位=确认装配）；Backspace 取消装配。全部走键盘——比赛模式下鼠标是锁定的，点击按钮会顺带把云台带偏。
 * @return 本帧是否正在显示（调用方可据此决定是否让出其它覆盖层的位置）。
 */
bool ShowAssemblyOverlay(const State &state, RoboGo::io::MqttClient &mqtt_client);
