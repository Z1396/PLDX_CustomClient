// log_panel.cpp：运行日志面板的实现。
#include "log_panel.h"  // 引入本面板对应的头文件（含函数声明与依赖）

// 无名命名空间：本面板内部使用的辅助函数（日志配色）与合并日志行结构，不对外暴露。
namespace {

// 功能：根据日志类型返回展示颜色（0=普通，1=警告，2=错误）。
ImVec4 GetLogTextColor(const LogEntry &log) {  // log：单条日志条目
    switch (log.type) {  // 按日志类型取色
        case 0:  // 普通日志
            return ImVec4(221.0f / 255.0f, 221.0f / 255.0f, 221.0f / 255.0f, 221.0f / 255.0f);  // RGB 221 归一化（约 0.87）浅灰
        case 1:  // 警告
            return ImVec4(230.0f / 255.0f, 162.0f / 255.0f, 60.0f / 255.0f, 1.0f);  // 橙黄（警告）
        case 2:  // 错误
            return ImVec4(245.0f / 255.0f, 108.0f / 255.0f, 108.0f / 255.0f, 1.0f);  // 红（错误）
        default:  // 兜底为普通
            return ImVec4(221.0f / 255.0f, 221.0f / 255.0f, 221.0f / 255.0f, 1.0f);  // 浅灰（不透明）
    }  // switch 分支结束
}  // 返回该日志类型对应的颜色

// 合并后的日志行视图：不拷贝日志内容，仅持有一条日志的指针 + 来源标签，用于统一排序与渲染。
struct CombinedLogLine {
    const LogEntry *entry = nullptr;  // 指向 vehicle_logs 或 system_logs 中的原始条目
    const char *source = nullptr;     // 来源标签："本车" 或 "系统"
};  // CombinedLogLine 定义结束

}// namespace

// 功能：合并本车日志与系统日志并按序号排序后滚动展示。详见头文件。
// 思路：预留容量避免反复分配，按 sequence 升序合并，新日志到达时自动滚到底部。
void ShowLogPanel(const State &state) {  // state：全局状态（含本车/系统日志）
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;  // 无导航/无标题栏/不可拖动/透明背景/不保存布局，作为纯覆盖层
    if (!ImGui::Begin("LogPanel", nullptr, window_flags)) {  // 窗口创建失败则结束
        ImGui::End();  // 配对 End
        return;  // 不绘制
    }  // 窗口折叠分支结束

    ImGui::Text("本车日志 + 系统日志");  // 面板标题

    std::vector<CombinedLogLine> combined_logs;  // 合并后的日志行视图容器
    combined_logs.reserve(state.vehicle_logs.size() + state.system_logs.size());  // 预估容量，减少重分配
    for (const auto &log: state.vehicle_logs) {  // 遍历本车日志
        combined_logs.push_back({&log, "本车"});  // 标记为本车来源
    }  // 本车日志收集结束
    for (const auto &log: state.system_logs) {  // 遍历系统日志
        combined_logs.push_back({&log, "系统"});  // 标记为系统来源
    }  // 系统日志收集结束
    std::ranges::sort(combined_logs, [](const CombinedLogLine &lhs, const CombinedLogLine &rhs) {  // 按 sequence 升序排序；比较器返回 lhs 是否排在 rhs 之前
        return lhs.entry->sequence < rhs.entry->sequence;  // 按全局序号升序，保证时间顺序
    });  // sort 结束

    ImGui::BeginChild("CombinedLogs", ImVec2(0, 0), ImGuiChildFlags_Borders);  // 带边框的滚动子窗口
    if (combined_logs.empty()) {  // 合并后无日志
        ImGui::TextDisabled("暂无日志");  // 空列表占位
    } else {  // 有日志则渲染
        static uint64_t last_rendered_sequence = 0;  // 记录上次渲染的最新序号，用于判断是否需滚动
        const uint64_t newest_sequence = combined_logs.back().entry->sequence;  // 当前最新序号
        ImGui::PushTextWrapPos(0.0f);  // 开启自动换行（0 表示按内容区域宽度）
        for (const CombinedLogLine &line: combined_logs) {  // 逐行绘制合并日志
            const ImVec4 color = GetLogTextColor(*line.entry);  // 按类型取色
            ImGui::TextColored(color, "[%s][%s] %s", line.entry->time, line.source, line.entry->text.c_str());  // 时间 + 来源 + 内容
        }  // 逐行绘制结束
        ImGui::PopTextWrapPos();  // 恢复换行设置
        if (newest_sequence != last_rendered_sequence) {  // 有新日志时才滚到底部
            ImGui::SetScrollHereY(1.0f);  // 将滚动条定位到最底部
            last_rendered_sequence = newest_sequence;  // 更新已渲染序号
        }  // 滚动判断结束
    }  // 空/非空分支结束
    ImGui::EndChild();  // 结束滚动子窗口

    ImGui::End();  // 结束面板窗口
}  // 函数结束
