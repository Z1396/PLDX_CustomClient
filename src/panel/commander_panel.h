// commander_panel.h：指挥手全屏综合指挥台（全屏战术地图 + 双方信息 + 哨兵/飞镖/空中指令）。
#pragma once

#include "main_window.h"   // 提供 State 全局状态（比赛阶段、双方单位、日志等）
#include "io/mqtt_client.h"   // 提供 MqttClient，用于下发哨兵/飞镖/地图标记指令

// 功能：绘制全屏综合指挥台。仅当用户以"指挥手"身份登录并进入指挥台时由主窗口调用。
// 参数：state 提供全部比赛数据；mqtt_client 用于下发指令；
//       video_texture_ref/video_size/video_ready 为当前主图传（指挥台里做现场画面预览）；
//       map_texture_ref/map_texture_size 为战术地图底图纹理及其像素尺寸。
void ShowCommanderPanel(const State &state,
                        RoboGo::io::MqttClient &mqtt_client,
                        ImTextureRef video_texture_ref,
                        const ImVec2 &video_size,
                        bool video_ready,
                        ImTextureRef map_texture_ref,
                        const ImVec2 &map_texture_size);
