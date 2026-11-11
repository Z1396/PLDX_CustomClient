// crosshair.h：准星绘制面板，声明 ShowCrosshair 接口。
#pragma once

// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
// [/pch-split]
#include "main_window.h"  // 引入主窗口头文件（提供 State 类型与 ImGui/ImVec 等依赖）

/**
 * @brief 在屏幕中心绘制十字准星 + 单圈枪管热量环 + 热量数值。
 * @note 思路：ImDrawList 直接画固定的白色底环 + 从 12 点逆时针的热量弧，按 1/2、3/4 上限分档换色；内层血量环、中层缓冲环、棕色余辉、火花线与事件涟漪均已移除。
 */
void ShowCrosshair(const State &state);
