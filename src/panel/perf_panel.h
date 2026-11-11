// perf_panel.h：性能浮层声明。
#pragma once

#include <pch.h>
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
// [/pch-split]

namespace RoboGo::panel {

/**
 * @brief 绘制性能浮层（默认隐藏，F12 呼出）。
 * @note 说明：内部自行判断可见性，调用方可以每帧无条件调用。
 */
void ShowPerformancePanel();

}  // namespace RoboGo::panel
