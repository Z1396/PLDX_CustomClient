// buff_panel.h：增益/Buff 状态面板，声明面板绘制与 Buff 辅助函数接口。
#pragma once

// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
// [/pch-split]
#include "main_window.h"  // 引入主窗口头文件（提供 State 类型与 ImGui/ImVec 等依赖）

/**
 * @brief 绘制当前机器人已激活的 Buff 列表及其剩余时间进度条。
 * @note 思路：遍历 State 中 active_buffs，仅显示与当前 robot_id 匹配的条目。
 */
void ShowBuffPanel(const State &state);  // state：全局状态（含静态状态与激活 Buff 列表）

/**
 * @brief 根据 Buff 类型与等级生成展示名称（如「攻击增益 +1」）。
 * @note 参数：buff_type 为 Buff 种类编号，buff_level 为正/负等级值。
 */
std::string GetBuffName(uint32_t buff_type, int32_t buff_level);

/**
 * @brief 根据 Buff 类型返回对应的展示颜色。
 */
ImVec4 GetBuffTypeColor(uint32_t buff_type);
