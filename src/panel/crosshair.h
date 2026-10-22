// crosshair.h：准星绘制面板，声明 ShowCrosshair 接口。
#pragma once

#include "main_window.h"

// 功能：在屏幕中心绘制十字准星，并环绕热量环实时反映枪管热量。
// 思路：使用 ImDrawList 直接画线/圆弧，热量比例由动态/静态状态计算。
void ShowCrosshair(const State &state);
