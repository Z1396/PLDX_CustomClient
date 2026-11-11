// robot_status_panel.h：己方机器人「本车卡」，声明 ShowRobotStatusPanel 接口。
#pragma once

#include "main_window.h"  // 提供 State 全局状态类型

/**
 * @brief 以卡片形式显示血量（大数字）、热量、弹量、缓冲能量、等级、模式与设备连接状态。
 * @note 思路：只读 State，全部用 DrawList 自绘，窗口尺寸由主窗口按定稿固定。
 */
void ShowRobotStatusPanel(const State &state);
