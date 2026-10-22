// mini_map.h：小地图面板，声明 ShowMiniMap 接口。
#pragma once

#include "main_window.h"
#include "io/mqtt_client.h"

// 功能：绘制战场小地图，显示双方机器人位置、哨兵路径，并支持标记模式点击下发坐标。
// 参数：state 提供雷达/路径数据；mqtt_client 用于发送地图标记；
//       mini_map_texture_ref/size 为地图底图纹理及其尺寸。
void ShowMiniMap(const State &state,
                 RoboGo::io::MqttClient &mqtt_client,
                 ImTextureRef mini_map_texture_ref,
                 const ImVec2 &mini_map_texture_size);
