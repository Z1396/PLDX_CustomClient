// perf_panel.cpp：性能浮层实现——把 perf_stats 采集到的耗时按"绿/黄/红"三档着色显示。
// 为什么要有这个面板：优化之前先有尺子。所有"预计省 X ms"的判断都必须在这里看到实际变化。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
// [/pch-split]
#include "perf_panel.h"

#include "app/perf_stats.h"  // 指标快照

namespace RoboGo::panel {

namespace {  // 内部辅助

// 耗时对应的颜色：<2ms 绿、<5ms 黄、否则红。
// 阈值依据：60fps 的预算是 16.7ms，单段超过 5ms 就说明它占了三分之一以上，必须先优化它。
ImVec4 ColorForMilliseconds(const double milliseconds) {
    if (milliseconds < 2.0) {  // 很轻
        return ImVec4(0.45f, 0.85f, 0.54f, 1.0f);  // 绿
    }
    if (milliseconds < 5.0) {  // 中等
        return ImVec4(0.95f, 0.78f, 0.35f, 1.0f);  // 黄
    }
    return ImVec4(0.95f, 0.42f, 0.42f, 1.0f);  // 红：这一段已经在吃帧预算
}

// 功能：画一行"标签 + 数值"，数值按阈值着色。
void MetricRow(const char *label, const double milliseconds) {
    ImGui::TextDisabled("%s", label);  // 灰色标签
    ImGui::SameLine(96.0f * ImGui::GetScale());  // 数值统一从固定列开始，便于纵向对比
    const ImVec4 color = ColorForMilliseconds(milliseconds);  // 按耗时取色
    ImGui::TextColored(color, "%6.2f ms", milliseconds);  // 右对齐显示两位小数
}

}  // namespace

void ShowPerformancePanel() {
    if (!RoboGo::app::perf::IsVisible()) {  // 面板未开启
        return;  // 什么都不画
    }

    const RoboGo::app::perf::Metrics metrics = RoboGo::app::perf::Snapshot();  // 取一次快照
    const float scale = ImGui::GetScale();  // 全局 UI 缩放
    const float panel_width = 252.0f * scale;  // 面板宽度（要容下"近10秒 0  累计 223"这一行）
    const ImGuiViewport *viewport = ImGui::GetMainViewport();  // 主视口，用于贴左下角

    // 贴在左侧日志栏顶部（顶部状态栏下方）：这块区域只有日志，被遮住损失最小；
    // 左下角是本车血量、右下角是小地图，都是比赛中要盯的内容，不能压。
    ImGui::SetNextWindowPos(ImVec2(12.0f * scale, viewport->Size.y * 0.15f + 12.0f * scale),
                            ImGuiCond_Always);  // 固定左上角定位，高度变化时不会跳
    // 高度交给 ImGui 按内容算：之前写死 190，加一行就把最后一行切掉了（实测"丢帧"被底边截断）。
    // 固定宽度 + 自适应高度，以后再增删指标都不用重新调数字。
    ImGui::SetNextWindowSizeConstraints(ImVec2(panel_width, 0.0f), ImVec2(panel_width, 10000.0f));
    ImGui::SetNextWindowBgAlpha(0.97f);  // 近乎不透明：调试浮层要能读清数字，透出底下的字反而干扰

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                   | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                                   | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus
                                   | ImGuiWindowFlags_AlwaysAutoResize;  // 宽度受限、高度随内容
    if (ImGui::Begin("##performance_overlay", nullptr, flags)) {  // 匿名窗口：标题不显示
        ImGui::TextColored(ImVec4(0.62f, 0.82f, 0.98f, 1.0f), "性能  F12 关闭");  // 标题兼关闭提示
        ImGui::Separator();  // 分隔线

        MetricRow("解码", metrics.decode_ms);  // HEVC 解码
        MetricRow("转换", metrics.convert_ms);  // YUV -> RGBA
        MetricRow("上传", metrics.upload_ms);  // 纹理上传（含等待）
        MetricRow("界面", metrics.ui_ms);  // ImGui 构建
        MetricRow("整帧", metrics.frame_ms);  // 事件泵到 Present

        ImGui::Separator();  // 分隔线
        // 分辨率必须显示：1080p 和 1440p 的解码/转换耗时能差一倍，不看这个数会误判优化效果。
        ImGui::TextDisabled("分辨率");  // 标签
        ImGui::SameLine(96.0f * scale);  // 对齐数值列
        ImGui::TextColored(ImVec4(0.72f, 0.75f, 0.80f, 1.0f), "%d x %d", metrics.video_width, metrics.video_height);  // 当前图传尺寸

        // 队列深度：>2 说明解码比渲染快，画面在客户端内部开始积压（延迟会变大）。
        ImGui::TextDisabled("帧率");  // 标签
        ImGui::SameLine(96.0f * scale);  // 对齐数值列
        ImGui::TextColored(metrics.fps >= 55.0f ? ImVec4(0.45f, 0.85f, 0.54f, 1.0f)  // 满帧绿
                                                : ImVec4(0.95f, 0.78f, 0.35f, 1.0f),  // 掉帧黄
                           "%.0f fps", metrics.fps);  // 帧率取整显示

        ImGui::TextDisabled("队列");  // 标签
        ImGui::SameLine(96.0f * scale);  // 对齐数值列
        const bool backlog = metrics.udp_queue > 2;  // 有积压
        ImGui::TextColored(backlog ? ImVec4(0.95f, 0.78f, 0.35f, 1.0f)  // 积压标黄
                                   : ImVec4(0.72f, 0.75f, 0.80f, 1.0f),  // 正常灰白
                           "udp %d", metrics.udp_queue);  // UDP 队列深度

        ImGui::TextDisabled("丢帧");  // 标签
        ImGui::SameLine(96.0f * scale);  // 对齐数值列
        // 先给"现在还在不在丢"——最近 10 秒的丢帧数；累计值放后面当参考。
        // 只看累计值会误判：那个数只增不减，哪怕早就不丢了它也不会回落。
        ImGui::TextColored(metrics.recent_drops > 0 ? ImVec4(0.95f, 0.42f, 0.42f, 1.0f)  // 现在还在丢：红
                                                    : ImVec4(0.45f, 0.85f, 0.54f, 1.0f),  // 现在没丢：绿
                           "近10秒 %d", metrics.recent_drops);  // 最近 10 秒丢帧数
        ImGui::SameLine();  // 同行
        ImGui::TextDisabled("累计 %llu", static_cast<unsigned long long>(metrics.dropped_frames));  // 历史总量，灰色不抢眼
    }
    ImGui::End();
}

}  // namespace RoboGo::panel
