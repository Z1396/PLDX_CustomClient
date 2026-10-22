// custom_video_panel.h：自定义字节块视频画面面板，声明 ShowCustomVideoPanel 接口。
#pragma once

#include <pch.h>

// 功能：在面板中显示一路视频画面，并提供 UDP 视频上下翻转切换按钮。
// 参数：video_texture_ref 为视频纹理引用；video_size 为原始分辨率；
//       panel_title 为标题；video_display_ready 表示视频是否就绪；
//       unavailable_message 为未就绪时的提示；udp_video_inverted 为当前反转状态。
// 返回值：当用户点击了翻转按钮时为 true，否则为 false（供上层切换反转状态）。
bool ShowCustomVideoPanel(ImTextureRef video_texture_ref,
                          const ImVec2 &video_size,
                          const char *panel_title,
                          bool video_display_ready,
                          std::string_view unavailable_message,
                          bool udp_video_inverted);
