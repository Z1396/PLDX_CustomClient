// robot_status_panel.h：己方机器人状态面板，声明 ShowRobotStatusPanel 接口。
#pragma once

#include "main_window.h"  // 提供 State 全局状态类型

// 功能：显示己方机器人的血量、能量、经验、发弹量、模式指示灯与设备连接状态。
// 思路：从 State 读取静态/动态状态，使用进度条与彩色文本逐项展示。
void ShowRobotStatusPanel(const State &state);
