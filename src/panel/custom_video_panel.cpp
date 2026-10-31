// custom_video_panel.cpp：自定义字节块视频画面面板的实现。
#include "custom_video_panel.h"

// 无名命名空间：本面板内部使用的辅助函数（图像等比缩放、居中文本），不对外暴露。
namespace {

// 功能：在给定可用区域内按比例缩放图像，保持宽高比完整显示（letterbox）。
// 思路：比较图像与区域的纵横比，以较短边为基准缩放，避免拉伸变形。
ImVec2 FitImageSize(const ImVec2 &available_size, const ImVec2 &texture_size) {
    if (available_size.x <= 0.0f || available_size.y <= 0.0f || texture_size.x <= 0.0f || texture_size.y <= 0.0f) {
        return ImVec2(0.0f, 0.0f);  // 任一尺寸非法则返回零尺寸
    }

    const float image_aspect = texture_size.x / texture_size.y;  // 图像宽高比
    const float available_aspect = available_size.x / available_size.y;  // 区域宽高比
    if (image_aspect > available_aspect) {  // 图像更宽 -> 以宽度撑满
        return ImVec2(available_size.x, available_size.x / image_aspect);
    }
    return ImVec2(available_size.y * image_aspect, available_size.y);  // 否则以高度撑满
}

// 功能：在子窗口区域内水平垂直居中绘制一段提示文本。
// 思路：计算文本尺寸并偏移光标到中心，开启自动换行以适应窄区域。
void ShowCenteredText(const char *message) {
    const ImVec2 child_avail = ImGui::GetContentRegionAvail();  // 当前子区域可用尺寸
    const float wrap_width = ImMax(0.0f, child_avail.x - ImGui::GetStyle().WindowPadding.x * 2.0f);  // 换行宽度（扣除内边距）
    const ImVec2 text_size = ImGui::CalcTextSize(message, nullptr, false, wrap_width);  // 按换行宽度测量文本
    ImVec2 cursor = ImGui::GetCursorPos();
    cursor.x += ImMax(0.0f, (child_avail.x - text_size.x) * 0.5f);  // 水平居中偏移
    cursor.y += ImMax(0.0f, (child_avail.y - text_size.y) * 0.5f);  // 垂直居中偏移
    ImGui::SetCursorPos(cursor);
    ImGui::PushTextWrapPos(cursor.x + wrap_width);  // 开启换行
    ImGui::TextDisabled("%s", message);  // 灰色禁用样式文本
    ImGui::PopTextWrapPos();
}

}// namespace

// 功能：显示视频画面并渲染 UDP 翻转切换按钮。详见头文件。
// 思路：顶部标题+按钮，下方子窗口内居中绘制纹理或提示语；按钮点击返回请求标志。
bool ShowCustomVideoPanel(ImTextureRef video_texture_ref,
                          const ImVec2 &video_size,
                          const char *panel_title,
                          const bool video_display_ready,
                          const std::string_view unavailable_message,
                          const bool udp_video_inverted) {
    // 无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("CustomVideoPanel", nullptr, window_flags)) {
        ImGui::End();
        return false;
    }

    bool toggle_udp_video_inversion_requested = false;  // 记录用户是否点击了翻转按钮
    ImGui::Text("%s", panel_title);  // 面板标题
    ImGui::SameLine();
    ImGui::TextDisabled("[H 切换]");  // 键盘 H 切换的提示
    const char *invert_button_label = udp_video_inverted
                                              ? "UDP反转: 开 [I]##UdpVideoInvert"
                                              : "UDP反转: 关 [I]##UdpVideoInvert";  // 按当前状态切换文案，「##」后避免显示快捷键
    const char *visible_label_end = std::strstr(invert_button_label, "##");  // 仅取「##」前的可见标签用于测量宽度
    const ImVec2 visible_label_size = ImGui::CalcTextSize(invert_button_label, visible_label_end);
    const float button_width = visible_label_size.x + ImGui::GetStyle().FramePadding.x * 2.0f;  // 按钮宽度=标签宽+左右内边距
    if (ImGui::GetContentRegionAvail().x >= button_width + ImGui::GetStyle().ItemSpacing.x) {  // 空间足够才与标题同行
        ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Button,
                          udp_video_inverted ? ImVec4(0.12f, 0.45f, 0.26f, 0.82f)
                                             : ImVec4(0.16f, 0.24f, 0.31f, 0.82f));  // 开：绿；关：蓝灰
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          udp_video_inverted ? ImVec4(0.16f, 0.58f, 0.34f, 0.95f)
                                             : ImVec4(0.20f, 0.36f, 0.44f, 0.95f));  // 悬停色
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          udp_video_inverted ? ImVec4(0.20f, 0.70f, 0.42f, 1.00f)
                                             : ImVec4(0.25f, 0.48f, 0.58f, 1.00f));  // 按下色
    toggle_udp_video_inversion_requested = ImGui::SmallButton(invert_button_label);  // 小按钮；点击即请求翻转
    ImGui::PopStyleColor(3);  // 恢复三种按钮颜色
    ImGui::BeginChild("CustomVehicleVideo", ImVec2(0, 0), ImGuiChildFlags_Borders);  // 带边框的视频子窗口
    if (video_display_ready
        && video_texture_ref.GetTexID() != ImTextureID_Invalid
        && video_size.x > 0.0f
        && video_size.y > 0.0f) {  // 视频就绪且纹理有效时才绘制画面
        const ImVec2 child_avail = ImGui::GetContentRegionAvail();  // 子窗口可用区域
        const ImVec2 image_size = FitImageSize(child_avail, video_size);  // 按比例缩放后的图像尺寸
        ImVec2 cursor = ImGui::GetCursorPos();
        cursor.x += (child_avail.x - image_size.x) * 0.5f;  // 水平居中
        cursor.y += (child_avail.y - image_size.y) * 0.5f;  // 垂直居中
        ImGui::SetCursorPos(cursor);
        ImGui::Image(video_texture_ref, image_size);  // 绘制视频纹理
    } else {
        const std::string message(unavailable_message);  // 未就绪时显示提示语
        ShowCenteredText(message.c_str());
    }
    ImGui::EndChild();

    ImGui::End();
    return toggle_udp_video_inversion_requested;  // 返回翻转请求，交由上层处理
}
