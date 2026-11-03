// 顶部状态栏声明：绘制比赛阶段、倒计时、金币与前哨站/基地/各机器人血量与弹药概览。
#pragma once

#include "main_window.h"  // 提供 State 全局状态类型

// 绘制顶部状态栏（顶部 15% 高度的无交互覆盖层）。
void ShowHeaderPanel(const State &state);

// 绘制判罚横幅：顶部居中的无交互提示条，显示判罚类型与本地倒计时；无生效判罚时什么都不画。
void ShowPenaltyBanner(const State &state);

// 绘制顶部事件提示层：把重要 Event 以短暂浮层堆叠显示（飞镖命中、基地遭攻击、装配结果等）。
void ShowEventToasts(const State &state);
