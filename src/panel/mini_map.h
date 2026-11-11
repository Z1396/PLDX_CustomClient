// mini_map.h：小地图面板，声明 ShowMiniMap 接口。
#pragma once

// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
// [/pch-split]
#include "main_window.h"  // 提供 State（雷达/路径数据）
#include "io/mqtt_client.h"  // 提供 MqttClient（标记模式下下发坐标）

/**
 * @brief 绘制战场小地图，显示双方机器人位置、哨兵路径，并支持标记模式点击下发坐标。
 * @note 参数：state 提供雷达/路径数据；mqtt_client 用于发送地图标记；mini_map_texture_ref/size 为地图底图纹理及其尺寸；mini_map_terrain_texture_ref/size
 *        为地形分层叠加层（2027 高度掩膜上色版，按 M 键开关）。
 */
void ShowMiniMap(const State &state,
                 RoboGo::io::MqttClient &mqtt_client,  // mqtt_client：标记模式下发送地图坐标的 MQTT 客户端
                 ImTextureRef mini_map_texture_ref,  // mini_map_texture_ref：地图底图纹理引用
                 const ImVec2 &mini_map_texture_size,  // mini_map_texture_size：底图原始像素尺寸，用于屏幕坐标换算
                 ImTextureRef mini_map_terrain_texture_ref,  // 地形分层叠加层引用（可空）
                 const ImVec2 &mini_map_terrain_texture_size);  // 叠加层原始像素尺寸
