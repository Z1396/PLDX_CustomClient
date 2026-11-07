// log_panel.cpp：运行日志面板的实现。
// 2027-11-06 按「主界面定稿」：去掉「本车日志 + 系统日志」标题，事件直接浮在背景上；
// 每行加一条按级别取色的细竖标，替代原来的灰面板与标题，其余日志合并/去重/自动滚动逻辑不变。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
// [/pch-split]
#include "log_panel.h"
#include "ui/theme.h"

namespace {

// 功能：根据日志类型返回展示颜色（0=普通，1=警告，2=错误）。
ImVec4 GetLogTextColor(const LogEntry &log) {
    switch (log.type) {
        case 0:  return RoboGo::ui::V(RoboGo::ui::kTextPrimary);
        case 1:  return RoboGo::ui::V(RoboGo::ui::kWarning);
        case 2:  return RoboGo::ui::V(RoboGo::ui::kTeamRed);
        default: return RoboGo::ui::V(RoboGo::ui::kTextPrimary);
    }
}

// 功能：行首细竖标的颜色，与文字同级别但更克制。
ImU32 GetLogMarkColor(const LogEntry &log) {
    switch (log.type) {
        case 1:  return RoboGo::ui::WithAlpha(RoboGo::ui::kWarning, 210);
        case 2:  return RoboGo::ui::WithAlpha(RoboGo::ui::kTeamRed, 220);
        default: return RoboGo::ui::WithAlpha(RoboGo::ui::kTextTertiary, 170);
    }
}

// 合并后的日志行视图：不拷贝日志内容，仅持有一条日志的指针 + 来源标签。
struct CombinedLogLine {
    const LogEntry *entry = nullptr;
    const char *source = nullptr;
};

}  // namespace

// 功能：合并本车日志与系统日志并按序号排序后滚动展示。
void ShowLogPanel(const State &state) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration
                                    | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground
                                    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
    if (!ImGui::Begin("LogPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    std::vector<CombinedLogLine> combined_logs;
    combined_logs.reserve(state.vehicle_logs.size() + state.system_logs.size());
    for (const auto &log: state.vehicle_logs) {
        combined_logs.push_back({&log, "本车"});
    }
    for (const auto &log: state.system_logs) {
        combined_logs.push_back({&log, "系统"});
    }
    std::ranges::sort(combined_logs, [](const CombinedLogLine &lhs, const CombinedLogLine &rhs) {
        return lhs.entry->sequence < rhs.entry->sequence;
    });

    // 子窗口只用来让新日志自动滚到底部，不承担视觉容器作用。
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("CombinedLogs", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    if (combined_logs.empty()) {
        ImGui::TextColored(RoboGo::ui::V(RoboGo::ui::kTextSecondary), "暂无日志");
    } else {
        static uint64_t last_rendered_sequence = 0;
        const uint64_t newest_sequence = combined_logs.back().entry->sequence;
        ImGui::PushTextWrapPos(0.0f);
        const float s = ImGui::GetScale();
        ImDrawList *draw_list = ImGui::GetWindowDrawList();
        for (const CombinedLogLine &line: combined_logs) {
            const ImVec4 color = GetLogTextColor(*line.entry);
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            draw_list->AddRectFilled(ImVec2(cursor.x, cursor.y + 3.0f * s),
                                     ImVec2(cursor.x + 3.0f * s, cursor.y + 15.0f * s),
                                     GetLogMarkColor(*line.entry), 1.5f * s);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f * s);
            if (line.entry->repeat_count > 1) {
                ImGui::TextColored(color, "[%s][%s] %s  x%u", line.entry->time, line.source,
                                   line.entry->text.c_str(), line.entry->repeat_count);
            } else {
                ImGui::TextColored(color, "[%s][%s] %s", line.entry->time, line.source,
                                   line.entry->text.c_str());
            }
        }
        ImGui::PopTextWrapPos();
        if (newest_sequence != last_rendered_sequence) {
            ImGui::SetScrollHereY(1.0f);
            last_rendered_sequence = newest_sequence;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::End();
}
