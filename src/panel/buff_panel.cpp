// buff_panel.cpp：增益/Buff 状态面板的实现。
#include "buff_panel.h"  // 引入本面板对应的头文件（含函数声明与依赖）

// 功能：根据 Buff 类型与等级生成展示名称（如「攻击增益 +1」「防御增益 -1」）。
// 思路：先按类型给出基础名，再追加符号化的等级与单位（% 或 /s）。
std::string GetBuffName(uint32_t buff_type, int32_t buff_level) {  // buff_type：增益种类编号；buff_level：带符号等级
    std::string name;  // 用于逐段拼接最终的展示名称
    switch (buff_type) {  // 按增益类型分支拼接基础名称
        case 1:  // 类型 1：攻击增益
            name = "攻击增益";  // 基础名称为「攻击增益」
            break;  // 跳出 switch
        case 2:  // 类型 2：防御增益（区分正负等级）
            name = buff_level >= 0 ? "防御增益" : "负防御增益";  // 负等级显示「负防御增益」
            break;  // 跳出 switch，进入统一的后缀拼接
        case 3:  // 类型 3：射击热量冷却增益
            name = "射击热量冷却增益";  // 基础名称：射击热量冷却
            break;  // 跳出 switch
        case 4:  // 类型 4：底盘功率增益
            name = "底盘功率增益";  // 基础名称：底盘功率
            break;  // 跳出 switch
        case 5:  // 类型 5：回血增益
            name = "回血增益";  // 基础名称：回血
            break;  // 跳出 switch
        case 6:  // 类型 6：可兑换允许发弹量（直接返回，无等级后缀）
            return "可兑换允许发弹量";  // 该类型不带等级概念，直接返回固定文案
        case 7:  // 类型 7：地形跨越增益（直接返回，无等级后缀）
            return "地形跨越增益";  // 该类型不带等级概念，直接返回固定文案
        default:  // 未知类型兜底
            return "未知增益";  // 未知类型兜底文案，避免返回空串
    }  // switch 分支结束

    int level = std::abs(buff_level);  // 取等级绝对值用于显示
    name.push_back(' ');  // 名称与等级之间加一个空格
    name.push_back(buff_level >= 0 ? '+' : '-');  // 正等级带 +，负等级带 -
    name.append(std::to_string(level));  // 追加等级数值（转字符串）
    if (buff_type == 3) {  // 类型 3 为冷却类，单位用 /s
        // 射击热量冷却增益 - 直接值
        name.append("/s");  // 冷却类为每秒冷却值，单位用 /s
    } else {  // 其余类型均为百分比
        // 百分比类型
        name.push_back('%');  // 其余增益为百分比
    }  // 单位后缀分支结束

    return name;  // 返回拼接完成的名称
}  // 返回拼接完成的完整名称

// 功能：根据 Buff 类型返回对应展示颜色，便于按种类区分。
ImVec4 GetBuffTypeColor(uint32_t buff_type) {  // 参数 buff_type：增益种类编号；返回 ImVec4(R,G,B,A) 颜色（分量 0~1）
    switch (buff_type) {  // 按类型返回固定调色板颜色（RGBA 分量取值 0~1）
        case 1:// 攻击增益 - 红色
            return {1.0f, 0.3f, 0.3f, 1.0f};  // R 通道高 = 红色
        case 2:// 防御增益 - 蓝色
            return {0.3f, 0.5f, 1.0f, 1.0f};  // B 通道高 = 蓝色
        case 3:// 射击热量冷却增益 - 橙色
            return {1.0f, 0.7f, 0.2f, 1.0f};  // R/G 高、B 低 = 橙色
        case 4:// 底盘功率增益 - 黄色
            return {1.0f, 1.0f, 0.3f, 1.0f};  // R/G 高 = 黄色
        case 5:// 回血增益 - 绿色
            return {0.3f, 1.0f, 0.3f, 1.0f};  // G 通道高 = 绿色
        case 6:// 可兑换允许发弹量 - 紫色
            return {0.7f, 0.3f, 1.0f, 1.0f};  // R/B 高 = 紫色
        case 7:// 地形跨越增益 - 青色
            return {0.3f, 1.0f, 1.0f, 1.0f};  // G/B 高 = 青色
        default:  // 未知类型兜底
            return {0.7f, 0.7f, 0.7f, 1.0f};  // 灰色
    }  // switch 分支结束
}  // 返回选中的颜色

// 功能：绘制当前机器人已激活 Buff 列表，含名称、剩余时间进度条与秒数。
// 思路：取当前 robot_id 过滤 active_buffs，按本地经过时间推算剩余秒数并绘制进度条。
void ShowBuffPanel(const State &state) {  // state：全局状态（含静态状态与激活 Buff 列表）
    // 无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |  // 不可拖动 + 透明背景，作为覆盖层使用
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;  // 不写入 imgui.ini，布局完全由代码控制

    if (!ImGui::Begin("BuffPanel", nullptr, window_flags)) {  // 创建窗口失败（被折叠）则结束
        ImGui::End();  // 配对 End，避免状态不一致
        return;  // 直接返回，不绘制内容
    }  // 窗口折叠分支结束

    ImVec2 content_avail = ImGui::GetContentRegionAvail();  // 可用内容宽度，用于进度条满宽
    float progress_bar_height = 3.0f;  // 进度条高度（细条）

    // Panel title
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Buff 状态");  // 白色标题文字
    ImGui::Separator();  // 标题下方画一条分隔线

    ImGui::Spacing();  // 标题与列表之间留空

    // Get current robot ID
    uint32_t current_robot_id = state.robot_static_status.has_value() ? state.robot_static_status->robot_id() : 0;  // 无静态状态则视为 0

    if (state.active_buffs.empty()) {  // 没有任何激活 Buff 时给出占位提示
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "无激活的 Buff");  // 灰色提示文字
        ImGui::End();  // 结束窗口
        return;  // 无内容可绘制
    }  // 空列表分支结束

    // Get current time for countdown
    auto now = std::chrono::steady_clock::now();  // 以当前时刻计算已流逝时间

    // Display each buff for current robot only
    for (const auto &buff_entry: state.active_buffs) {  // 遍历所有激活 Buff
        const Buff &buff = buff_entry.buff;  // 取出本次 Buff 数据

        // Only show buffs for current robot
        if (buff.robot_id() != current_robot_id) {  // 仅显示属于本车的 Buff
            continue;  // 非本车则跳过
        }  // 非本车 Buff 过滤结束

        std::string buff_name = GetBuffName(buff.buff_type(), buff.buff_level());  // 生成带等级的名称
        ImVec4 buff_color = GetBuffTypeColor(buff.buff_type());  // 取类型对应颜色

        // Calculate elapsed time and remaining time
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - buff_entry.update_time).count();  // 自上次更新至今的秒数
        int remaining_time = static_cast<int>(buff.buff_left_time()) - static_cast<int>(elapsed);  // 剩余时间 = 原始剩余 - 流逝

        // Skip if expired (should have been removed above, but double check)
        if (remaining_time <= 0) {  // 已过期则跳过（双保险）
            continue;  // 不绘制过期 Buff
        }  // 过期 Buff 过滤结束

        float time_ratio = static_cast<float>(remaining_time) /  // 剩余比例 = 剩余秒数 / 最大秒数，取值 0~1
                           static_cast<float>(buff.buff_max_time());  // 剩余/最大，作为进度比例

        // Buff name with level included in the name
        ImGui::TextColored(buff_color, "%s", buff_name.c_str());  // 带颜色显示名称

        // Time progress bar
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, buff_color);  // 进度条使用 Buff 颜色
        ImGui::ProgressBar(time_ratio, ImVec2(content_avail.x, progress_bar_height), "");  // 绘制进度条：比例 + 满宽×细高 + 空覆盖文本
        ImGui::PopStyleColor();  // 恢复进度条颜色样式

        // Time text
        ImGui::Text("%d/%d 秒", remaining_time, buff.buff_max_time());  // 显示剩余/最大秒数

        ImGui::Spacing();  // 各 Buff 条目之间留空
    }  // Buff 遍历结束

    ImGui::End();  // 结束窗口绘制
}  // 函数结束
