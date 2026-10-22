// buff_panel.cpp：增益/Buff 状态面板的实现。
#include "buff_panel.h"

// 功能：根据 Buff 类型与等级生成展示名称（如「攻击增益 +1」「防御增益 -1」）。
// 思路：先按类型给出基础名，再追加符号化的等级与单位（% 或 /s）。
std::string GetBuffName(uint32_t buff_type, int32_t buff_level) {
    std::string name;
    switch (buff_type) {
        case 1:
            name = "攻击增益";
            break;
        case 2:
            name = buff_level >= 0 ? "防御增益" : "负防御增益";
            break;
        case 3:
            name = "射击热量冷却增益";
            break;
        case 4:
            name = "底盘功率增益";
            break;
        case 5:
            name = "回血增益";
            break;
        case 6:
            return "可兑换允许发弹量";
        case 7:
            return "地形跨越增益";
        default:
            return "未知增益";
    }

    int level = std::abs(buff_level);  // 取等级绝对值用于显示
    name.push_back(' ');
    name.push_back(buff_level >= 0 ? '+' : '-');  // 正等级带 +，负等级带 -
    name.append(std::to_string(level));
    if (buff_type == 3) {
        // 射击热量冷却增益 - 直接值
        name.append("/s");  // 冷却类为每秒冷却值，单位用 /s
    } else {
        // 百分比类型
        name.push_back('%');  // 其余增益为百分比
    }

    return name;
}

// 功能：根据 Buff 类型返回对应展示颜色，便于按种类区分。
ImVec4 GetBuffTypeColor(uint32_t buff_type) {
    switch (buff_type) {
        case 1:// 攻击增益 - 红色
            return {1.0f, 0.3f, 0.3f, 1.0f};
        case 2:// 防御增益 - 蓝色
            return {0.3f, 0.5f, 1.0f, 1.0f};
        case 3:// 射击热量冷却增益 - 橙色
            return {1.0f, 0.7f, 0.2f, 1.0f};
        case 4:// 底盘功率增益 - 黄色
            return {1.0f, 1.0f, 0.3f, 1.0f};
        case 5:// 回血增益 - 绿色
            return {0.3f, 1.0f, 0.3f, 1.0f};
        case 6:// 可兑换允许发弹量 - 紫色
            return {0.7f, 0.3f, 1.0f, 1.0f};
        case 7:// 地形跨越增益 - 青色
            return {0.3f, 1.0f, 1.0f, 1.0f};
        default:
            return {0.7f, 0.7f, 0.7f, 1.0f};
    }
}

// 功能：绘制当前机器人已激活 Buff 列表，含名称、剩余时间进度条与秒数。
// 思路：取当前 robot_id 过滤 active_buffs，按本地经过时间推算剩余秒数并绘制进度条。
void ShowBuffPanel(const State &state) {
    // 无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("BuffPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    ImVec2 content_avail = ImGui::GetContentRegionAvail();  // 可用内容宽度，用于进度条满宽
    float progress_bar_height = 3.0f;  // 进度条高度（细条）

    // Panel title
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Buff 状态");
    ImGui::Separator();

    ImGui::Spacing();

    // Get current robot ID
    uint32_t current_robot_id = state.robot_static_status.has_value() ? state.robot_static_status->robot_id() : 0;  // 无静态状态则视为 0

    if (state.active_buffs.empty()) {  // 没有任何激活 Buff 时给出占位提示
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "无激活的 Buff");
        ImGui::End();
        return;
    }

    // Get current time for countdown
    auto now = std::chrono::steady_clock::now();  // 以当前时刻计算已流逝时间

    // Display each buff for current robot only
    for (const auto &buff_entry: state.active_buffs) {
        const Buff &buff = buff_entry.buff;

        // Only show buffs for current robot
        if (buff.robot_id() != current_robot_id) {  // 仅显示属于本车的 Buff
            continue;
        }

        std::string buff_name = GetBuffName(buff.buff_type(), buff.buff_level());  // 生成带等级的名称
        ImVec4 buff_color = GetBuffTypeColor(buff.buff_type());  // 取类型对应颜色

        // Calculate elapsed time and remaining time
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - buff_entry.update_time).count();  // 自上次更新至今的秒数
        int remaining_time = static_cast<int>(buff.buff_left_time()) - static_cast<int>(elapsed);  // 剩余时间 = 原始剩余 - 流逝

        // Skip if expired (should have been removed above, but double check)
        if (remaining_time <= 0) {  // 已过期则跳过（双保险）
            continue;
        }

        float time_ratio = static_cast<float>(remaining_time) /
                           static_cast<float>(buff.buff_max_time());  // 剩余/最大，作为进度比例

        // Buff name with level included in the name
        ImGui::TextColored(buff_color, "%s", buff_name.c_str());  // 带颜色显示名称

        // Time progress bar
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, buff_color);  // 进度条使用 Buff 颜色
        ImGui::ProgressBar(time_ratio, ImVec2(content_avail.x, progress_bar_height), "");
        ImGui::PopStyleColor();

        // Time text
        ImGui::Text("%d/%d 秒", remaining_time, buff.buff_max_time());  // 显示剩余/最大秒数

        ImGui::Spacing();
    }

    ImGui::End();
}
