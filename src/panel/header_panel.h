// 主 HUD 顶部区声明：绘制状态胶囊、双方队伍条，以及判罚/事件浮层。
#pragma once

#include "main_window.h"  // 提供 State 全局状态类型

// 绘制主 HUD 顶部区（全视口无交互覆盖层，元素按定稿绝对坐标自绘）。
void ShowHeaderPanel(const State &state);

// 绘制判罚横幅：顶部居中的无交互提示条，显示判罚类型与本地倒计时；无生效判罚时什么都不画。
void ShowPenaltyBanner(const State &state);

// 绘制顶部事件提示层：把重要 Event 以短暂浮层堆叠显示（飞镖命中、基地遭攻击、装配结果等）。
void ShowEventToasts(const State &state);
