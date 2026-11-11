// commander_panel.h：指挥手全屏综合指挥台。
// 视觉定稿：一条连续顶栏 + 透明三栏（双方阵容与建筑 / 战术地图与 KPI / 指挥指令与链路），
// 沿用主 HUD 定稿的视觉令牌；战术地图绘制与交互保持项目原设计。
#pragma once

// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
// [/pch-split]
#include "main_window.h"   // 提供 State 全局状态（比赛阶段、双方单位、日志等）
#include "io/mqtt_client.h"   // 提供 MqttClient，用于下发哨兵/飞镖/地图标记指令

/**
 * @brief 绘制全屏综合指挥台。指挥手登录后自动进入；其它身份也可从开局面板打开。
 * @note 参数：state 提供全部比赛数据；mqtt_client 用于下发指令；video_texture_ref/video_size/video_ready 为当前主图传（指挥台里做现场画面预览）
 *        ；map_texture_ref/map_texture_size 为战术地图底图纹理及其像素尺寸。
 */
void ShowCommanderPanel(const State &state,
                        RoboGo::io::MqttClient &mqtt_client,
                        ImTextureRef video_texture_ref,
                        const ImVec2 &video_size,
                        bool video_ready,
                        ImTextureRef map_texture_ref,
                        const ImVec2 &map_texture_size,
                        ImTextureRef map_terrain_texture_ref,
                        const ImVec2 &map_terrain_texture_size);
