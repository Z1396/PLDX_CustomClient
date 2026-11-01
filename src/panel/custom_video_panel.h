// custom_video_panel.h：自定义字节块视频画面面板，声明 ShowCustomVideoPanel 接口。
#pragma once

#include <pch.h>  // 预编译头（集中引入 ImGui、std 等公共依赖）

// 功能：在面板中显示一路视频画面，并提供 UDP 视频上下翻转切换按钮。
// 参数：video_texture_ref 为视频纹理引用；video_size 为原始分辨率；
//       panel_title 为标题；video_display_ready 表示视频是否就绪；
//       unavailable_message 为未就绪时的提示；udp_video_inverted 为当前反转状态。
// 返回值：当用户点击了翻转按钮时为 true，否则为 false（供上层切换反转状态）。
bool ShowCustomVideoPanel(ImTextureRef video_texture_ref,
                          const ImVec2 &video_size,  // video_size：视频原始分辨率，用于等比缩放
                          const char *panel_title,  // panel_title：面板标题
                          bool video_display_ready,  // video_display_ready：视频是否已就绪（决定画画面还是提示语）
                          std::string_view unavailable_message,  // unavailable_message：未就绪时的提示文本
                          bool udp_video_inverted);  // udp_video_inverted：当前是否上下翻转；返回 true 表示请求翻转
