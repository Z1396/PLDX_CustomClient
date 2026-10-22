// 顶部状态栏声明：绘制比赛阶段、倒计时、金币与前哨站/基地/各机器人血量与弹药概览。
#pragma once

#include "main_window.h"

// 绘制顶部状态栏（顶部 15% 高度的无交互覆盖层）。
void ShowHeaderPanel(const State &state);
