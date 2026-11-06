// 比赛展示文案：阶段码、建筑名等「协议数值 -> 中文」的唯一来源。
// 各面板只读这里，避免同一个阶段码在顶部状态栏和指挥台各写一份、改文案时漏改。
#pragma once

#include <cstdint>

namespace RoboGo::app::match_display {

// 比赛阶段码 -> 中文名（协议 2.2.3 current_stage）。
// 0 未开始 / 1 准备 / 2 自检 / 3 五秒倒计时 / 4 比赛中 / 5 结算 / 其它未知。
inline const char *StageName(uint32_t stage) {
    switch (stage) {
        case 0: return "未开始比赛";
        case 1: return "准备阶段";
        case 2: return "裁判系统自检";
        case 3: return "五秒倒计时";
        case 4: return "比赛中";
        case 5: return "比赛结算中";
        default: return "未知阶段";
    }
}

}  // namespace RoboGo::app::match_display
