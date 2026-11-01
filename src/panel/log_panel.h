// log_panel.h：运行日志面板，声明 ShowLogPanel 接口。
#pragma once

#include "main_window.h"  // 引入主窗口头文件（提供 State 与 LogEntry 类型等依赖）

// 功能：将本车日志与系统日志按序号合并排序后，在滚动区域中展示。
// 思路：合并两类日志并按 sequence 升序排列，自动滚动到最新一条。
void ShowLogPanel(const State &state);  // state：全局状态（含本车/系统日志）
