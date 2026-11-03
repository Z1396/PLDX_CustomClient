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

// 功能：每帧由 UI 线程调用，采集键鼠状态并写入跨线程共享的原子变量。
// 说明：真正的上报由 start_keyboard_mouse_publisher 启动的独立线程按固定周期完成，
//       本函数只做采样（ImGui 的状态只能在 UI 线程读），因此不会再受渲染帧率影响。
// 参数 mqtt_client：保留形参以稳定调用点（发布已移到上报线程）。
void update_keyboard_mouse_state(RoboGo::io::MqttClient &mqtt_client);

// 功能：启动/停止固定周期的键鼠上报线程（进程内只启动一次，stop 可重复调用）。
// 参数 mqtt_client：发布用的客户端，其生命周期必须长于上报线程
//        （application 里先 stop_keyboard_mouse_publisher，再停 MQTT）。
void start_keyboard_mouse_publisher(RoboGo::io::MqttClient &mqtt_client);
void stop_keyboard_mouse_publisher();

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

// 功能：无视"标记模式"开关，直接下发一次地图标记（指挥台点地图时用）。
// 说明：指挥台的鼠标是可见的，点地图就是要打点，不需要先切成标记模式。
void send_map_marker_now(RoboGo::io::MqttClient &mqtt_client, float map_x, float map_y, bool include_sentry);

// 功能：查询/设置当前登录身份是否为"指挥手"。
// 说明：指挥手不是机器人、没有自己的 robot_id，登录槽位沿用飞镖的 8 号，
//       因此不能用 robot_id 判断身份，必须单独记一个角色标志。
bool is_commander_role();
void set_commander_role(bool enabled);

// 功能：查询指挥台（全屏综合指挥界面）是否已打开。
bool is_commander_console_active();

// 功能：打开/关闭指挥台。打开时自动退出比赛模式，保证鼠标箭头可见、键鼠不被捕获。
void enter_commander_console();
void exit_commander_console();

// 功能：指挥手登录成功后自动进入指挥台（每次成功登录只自动进一次）。
// 说明：指挥手的赛前页面里没有任何可用指令（弹药/回血/复活/哨兵/飞镖全在指挥台内），
//       停在那一页只会多一次点击，所以一旦连上就直铺开指挥台；退出指挥台后不会被立刻拽回去。
void maybe_auto_enter_commander_console(RoboGo::io::MqttClient &mqtt_client);

// 功能：离开指挥台并退出登录（指挥手唯一的出口）。
// 说明：指挥手没有"赛前指令页"，退出后停在那页只会让人再点一次，所以直接断开连接回登录面板重新选身份。
void leave_commander_console(RoboGo::io::MqttClient &mqtt_client);
