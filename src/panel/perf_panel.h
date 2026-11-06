// perf_panel.h：性能浮层声明。
#pragma once

#include <pch.h>

namespace RoboGo::panel {

// 功能：绘制性能浮层（默认隐藏，F12 呼出）。
// 说明：内部自行判断可见性，调用方可以每帧无条件调用。
void ShowPerformancePanel();

}  // namespace RoboGo::panel
