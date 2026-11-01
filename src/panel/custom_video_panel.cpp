// custom_video_panel.cpp：自定义字节块视频画面面板的实现。
#include "custom_video_panel.h"  // 引入本面板对应的头文件（含函数声明与依赖）

// 无名命名空间：本面板内部使用的辅助函数（图像等比缩放、居中文本），不对外暴露。
namespace {

// 功能：在给定可用区域内按比例缩放图像，保持宽高比完整显示（letterbox）。
// 思路：比较图像与区域的纵横比，以较短边为基准缩放，避免拉伸变形。
ImVec2 FitImageSize(const ImVec2 &available_size, const ImVec2 &texture_size) {  // available_size：可用区域；texture_size：纹理原始尺寸
    if (available_size.x <= 0.0f || available_size.y <= 0.0f || texture_size.x <= 0.0f || texture_size.y <= 0.0f) {  // 任一维度 <= 0（未完成布局或纹理无效）时直接判为不可绘制
        return ImVec2(0.0f, 0.0f);  // 任一尺寸非法则返回零尺寸
    }  // 非法尺寸分支结束

    const float image_aspect = texture_size.x / texture_size.y;  // 图像宽高比
    const float available_aspect = available_size.x / available_size.y;  // 区域宽高比
    if (image_aspect > available_aspect) {  // 图像更宽 -> 以宽度撑满
        return ImVec2(available_size.x, available_size.x / image_aspect);  // 宽度=区域宽，高度按比例
    }  // 图像更宽分支结束
    return ImVec2(available_size.y * image_aspect, available_size.y);  // 否则以高度撑满
}  // 返回按比例缩放后的尺寸

// 功能：在子窗口区域内水平垂直居中绘制一段提示文本。
// 思路：计算文本尺寸并偏移光标到中心，开启自动换行以适应窄区域。
void ShowCenteredText(const char *message) {  // message：要居中显示的提示文本
    const ImVec2 child_avail = ImGui::GetContentRegionAvail();  // 当前子区域可用尺寸
    const float wrap_width = ImMax(0.0f, child_avail.x - ImGui::GetStyle().WindowPadding.x * 2.0f);  // 换行宽度（扣除内边距）
    const ImVec2 text_size = ImGui::CalcTextSize(message, nullptr, false, wrap_width);  // 按换行宽度测量文本
    ImVec2 cursor = ImGui::GetCursorPos();  // 取得当前光标位置
    cursor.x += ImMax(0.0f, (child_avail.x - text_size.x) * 0.5f);  // 水平居中偏移
    cursor.y += ImMax(0.0f, (child_avail.y - text_size.y) * 0.5f);  // 垂直居中偏移
    ImGui::SetCursorPos(cursor);  // 应用居中后的光标位置
    ImGui::PushTextWrapPos(cursor.x + wrap_width);  // 开启换行（按计算宽度）
    ImGui::TextDisabled("%s", message);  // 灰色禁用样式文本
    ImGui::PopTextWrapPos();  // 恢复换行设置
}  // ShowCenteredText 结束

}// namespace

// 功能：显示视频画面并渲染 UDP 翻转切换按钮。详见头文件。
// 思路：顶部标题+按钮，下方子窗口内居中绘制纹理或提示语；按钮点击返回请求标志。
bool ShowCustomVideoPanel(ImTextureRef video_texture_ref,
                          const ImVec2 &video_size,  // video_size：纹理原始分辨率，用于等比缩放
                          const char *panel_title,  // panel_title：面板标题文本
                          const bool video_display_ready,  // video_display_ready：视频管线是否已就绪
                          const std::string_view unavailable_message,  // unavailable_message：未就绪时显示的提示语
                          const bool udp_video_inverted) {  // udp_video_inverted：当前是否上下翻转 UDP 画面
    // 无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("CustomVideoPanel", nullptr, window_flags)) {  // 窗口创建失败则结束
        ImGui::End();  // 配对 End
        return false;  // 返回未请求翻转
    }  // 窗口折叠分支结束

    bool toggle_udp_video_inversion_requested = false;  // 记录用户是否点击了翻转按钮
    ImGui::Text("%s", panel_title);  // 面板标题
    ImGui::SameLine();  // 提示紧接标题同行
    ImGui::TextDisabled("[H 切换]");  // 键盘 H 切换的提示
    const char *invert_button_label = udp_video_inverted  // 按当前反转状态选择按钮文案（## 之后为 ImGui ID，不显示）
                                              ? "UDP反转: 开 [I]##UdpVideoInvert"  // 已反转：显示「开」
                                              : "UDP反转: 关 [I]##UdpVideoInvert";  // 按当前状态切换文案，「##」后避免显示快捷键
    const char *visible_label_end = std::strstr(invert_button_label, "##");  // 仅取「##」前的可见标签用于测量宽度
    const ImVec2 visible_label_size = ImGui::CalcTextSize(invert_button_label, visible_label_end);  // 测量可见标签尺寸
    const float button_width = visible_label_size.x + ImGui::GetStyle().FramePadding.x * 2.0f;  // 按钮宽度=标签宽+左右内边距
    if (ImGui::GetContentRegionAvail().x >= button_width + ImGui::GetStyle().ItemSpacing.x) {  // 空间足够才与标题同行
        ImGui::SameLine();  // 翻转按钮与标题同行
    }  // 同行布局判定结束
    ImGui::PushStyleColor(ImGuiCol_Button,  // 压入按钮常态底色（ImGuiCol_Button）
                          udp_video_inverted ? ImVec4(0.12f, 0.45f, 0.26f, 0.82f)  // 开启反转用绿色，直观表达「已翻转」
                                             : ImVec4(0.16f, 0.24f, 0.31f, 0.82f));  // 开：绿；关：蓝灰
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  // 压入按钮悬停色（ImGuiCol_ButtonHovered）
                          udp_video_inverted ? ImVec4(0.16f, 0.58f, 0.34f, 0.95f)  // 悬停色：开启时更亮的绿
                                             : ImVec4(0.20f, 0.36f, 0.44f, 0.95f));  // 悬停色
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  // 压入按钮按下色（ImGuiCol_ButtonActive）
                          udp_video_inverted ? ImVec4(0.20f, 0.70f, 0.42f, 1.00f)  // 按下色：开启时最亮的绿
                                             : ImVec4(0.25f, 0.48f, 0.58f, 1.00f));  // 按下色
    toggle_udp_video_inversion_requested = ImGui::SmallButton(invert_button_label);  // 小按钮；点击即请求翻转
    ImGui::PopStyleColor(3);  // 恢复三种按钮颜色
    ImGui::BeginChild("CustomVehicleVideo", ImVec2(0, 0), ImGuiChildFlags_Borders);  // 带边框的视频子窗口
    if (video_display_ready  // 条件一：视频管线已就绪
        && video_texture_ref.GetTexID() != ImTextureID_Invalid  // 条件二：纹理句柄有效（Invalid 时 Image 会画不出内容）
        && video_size.x > 0.0f  // 条件三：宽高均为正，避免 FitImageSize 中除零
        && video_size.y > 0.0f) {  // 视频就绪且纹理有效时才绘制画面
        const ImVec2 child_avail = ImGui::GetContentRegionAvail();  // 子窗口可用区域
        const ImVec2 image_size = FitImageSize(child_avail, video_size);  // 按比例缩放后的图像尺寸
        ImVec2 cursor = ImGui::GetCursorPos();  // 取得当前光标位置
        cursor.x += (child_avail.x - image_size.x) * 0.5f;  // 水平居中偏移
        cursor.y += (child_avail.y - image_size.y) * 0.5f;  // 垂直居中偏移
        ImGui::SetCursorPos(cursor);  // 应用居中光标
        ImGui::Image(video_texture_ref, image_size);  // 绘制视频纹理（居中、等比缩放）
    } else {  // 视频未就绪时显示提示语
        const std::string message(unavailable_message);  // 未就绪时显示提示语
        ShowCenteredText(message.c_str());  // 居中绘制提示
    }  // 绘制画面 / 显示占位提示的分支结束
    ImGui::EndChild();  // 结束视频子窗口

    ImGui::End();  // 结束面板窗口
    return toggle_udp_video_inversion_requested;  // 返回翻转请求，交由上层处理
}  // 返回本帧是否请求翻转，由上层翻转状态并持久化
