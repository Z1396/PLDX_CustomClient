// crosshair.h：准星绘制面板，声明 ShowCrosshair 接口。
#pragma once

// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
// [/pch-split]
#include "main_window.h"  // 引入主窗口头文件（提供 State 类型与 ImGui/ImVec 等依赖）

// 功能：在屏幕中心绘制十字准星，并用官方分档的环形读条实时反映枪管热量（含数值）。
// 思路：ImDrawList 直接画底环 + 从 12 点逆时针的填充弧；热量冷却时弧平滑倒退。
void ShowCrosshair(const State &state);
