// bottom_panel.h：底部控制度带，声明 ShowBottomPanel 接口。
#pragma once

#include "main_window.h"  // 引入主窗口头文件（提供 State 类型与 ImGui/ImVec 等依赖）

// 功能：在屏幕底部居中绘制双色控制度带（大数字 + 总伤害/存活一行），并在下方显示复活/装配状态。
// 思路：纯展示型函数，只读 State，由 main_window 在每帧末尾调用。
void ShowBottomPanel(const State &state);

// 功能：返回底部条上一帧实际占用的高度，供其它贴底面板让位。
float GetBottomPanelHeight();
