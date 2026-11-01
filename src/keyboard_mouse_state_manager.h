#pragma once // 头文件防重包含：#pragma 语义由编译器保证，只被预处理一次

#include <pch.h> // 预编译头：集中引入标准库与第三方头，加速整体编译

// 键鼠状态管理头文件：声明全局键鼠采集、比赛/标记模式控制及 MQTT 上报相关接口。
#include "io/mqtt_client.h"

// 功能：查询当前是否处于比赛模式。
bool is_in_game_mode();

// 功能：进入比赛模式(置位开关)。
void enter_game_mode();

// 功能：注册平台层的游戏输入捕获回调，在进入/退出比赛模式时通知其独占输入。
// 参数 handler：回调函数，参数为是否启用输入捕获。
void set_game_input_capture_handler(std::function<void(bool enabled)> handler);

// 功能：每帧采集并(按约 75Hz)上报键鼠状态到裁判系统。
// 参数 mqtt_client：用于发布 KeyboardMouseControl 消息的 MQTT 客户端。
void update_keyboard_mouse_state(RoboGo::io::MqttClient &mqtt_client);

// 功能：获取当前鼠标灵敏度系数。
float get_mouse_sensitivity();

// 功能：设置鼠标灵敏度系数。
// 参数 sensitivity：新的灵敏度值。
void set_mouse_sensitivity(float sensitivity);

// 功能：查询当前是否处于地图点位标记模式。
bool is_marker_mode();

// 功能：查询地图标记是否包含哨兵。
bool is_marker_mode_include_sentry();

// 功能：设置地图标记是否包含哨兵。
// 参数 include：true 表示群发给哨兵。
void set_marker_mode_include_sentry(bool include);

// 功能：进入地图点位标记模式。
void enter_marker_mode();

// 功能：退出地图点位标记模式。
void exit_marker_mode();

// 功能：在标记模式下向裁判系统发送一次地图点击标记指令(MapClickCmd)。
// 参数 mqtt_client：MQTT 客户端；map_x/map_y：地图上的归一化坐标。
void send_map_marker(RoboGo::io::MqttClient &mqtt_client, float map_x, float map_y);
