// log_panel.cpp：运行日志面板的实现。
#include "log_panel.h"

namespace {

// 功能：根据日志类型返回展示颜色（0=普通，1=警告，2=错误）。
ImVec4 GetLogTextColor(const LogEntry &log) {
    switch (log.type) {
        case 0:
            return ImVec4(221.0f / 255.0f, 221.0f / 255.0f, 221.0f / 255.0f, 221.0f / 255.0f);
        case 1:
            return ImVec4(230.0f / 255.0f, 162.0f / 255.0f, 60.0f / 255.0f, 1.0f);
        case 2:
            return ImVec4(245.0f / 255.0f, 108.0f / 255.0f, 108.0f / 255.0f, 1.0f);
        default:
            return ImVec4(221.0f / 255.0f, 221.0f / 255.0f, 221.0f / 255.0f, 1.0f);
    }
}

// 合并后的日志行视图：不拷贝日志内容，仅持有一条日志的指针 + 来源标签，用于统一排序与渲染。
struct CombinedLogLine {
    const LogEntry *entry = nullptr;  // 指向 vehicle_logs 或 system_logs 中的原始条目
    const char *source = nullptr;     // 来源标签："本车" 或 "系统"
};

}// namespace

// 功能：合并本车日志与系统日志并按序号排序后滚动展示。详见头文件。
// 思路：预留容量避免反复分配，按 sequence 升序合并，新日志到达时自动滚到底部。
void ShowLogPanel(const State &state) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("LogPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    ImGui::Text("本车日志 + 系统日志");  // 面板标题

    std::vector<CombinedLogLine> combined_logs;
    combined_logs.reserve(state.vehicle_logs.size() + state.system_logs.size());  // 预估容量，减少重分配
    for (const auto &log: state.vehicle_logs) {
        combined_logs.push_back({&log, "本车"});  // 标记为本车来源
    }
    for (const auto &log: state.system_logs) {
        combined_logs.push_back({&log, "系统"});  // 标记为系统来源
    }
    std::ranges::sort(combined_logs, [](const CombinedLogLine &lhs, const CombinedLogLine &rhs) {
        return lhs.entry->sequence < rhs.entry->sequence;  // 按全局序号升序，保证时间顺序
    });

    ImGui::BeginChild("CombinedLogs", ImVec2(0, 0), ImGuiChildFlags_Borders);  // 带边框的滚动子窗口
    if (combined_logs.empty()) {
        ImGui::TextDisabled("暂无日志");  // 空列表占位
    } else {
        static uint64_t last_rendered_sequence = 0;  // 记录上次渲染的最新序号，用于判断是否需滚动
        const uint64_t newest_sequence = combined_logs.back().entry->sequence;  // 当前最新序号
        ImGui::PushTextWrapPos(0.0f);  // 开启自动换行（0 表示按内容区域宽度）
        for (const CombinedLogLine &line: combined_logs) {
            const ImVec4 color = GetLogTextColor(*line.entry);  // 按类型取色
            ImGui::TextColored(color, "[%s][%s] %s", line.entry->time, line.source, line.entry->text.c_str());  // 时间 + 来源 + 内容
        }
        ImGui::PopTextWrapPos();
        if (newest_sequence != last_rendered_sequence) {  // 有新日志时才滚到底部
            ImGui::SetScrollHereY(1.0f);  // 将滚动条定位到最底部
            last_rendered_sequence = newest_sequence;  // 更新已渲染序号
        }
    }
    ImGui::EndChild();

    ImGui::End();
}
