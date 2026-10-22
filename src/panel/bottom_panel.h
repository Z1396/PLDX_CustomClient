// bottom_panel.h：底部信息条面板，声明 ShowBottomPanel 接口。
#pragma once

#include "main_window.h"

// 功能：在屏幕底部绘制一条居中信息条，汇总复活、装配、空中支援等状态。
// 思路：纯展示型函数，只读 State，由 main_window 在每帧末尾调用。
void ShowBottomPanel(const State &state);
