#pragma once

#include <pch.h>

// 应用层公共数据结构：定义视频帧、UI 日志、纹理槽/视图及启动参数等类型。
namespace RoboGo::app {

// 视频帧：承载一帧已解码为 RGBA 的像素数据及其尺寸，用于纹理上传。
struct VideoFrame {
    std::vector<uint8_t> buffer; // 连续的 RGBA 像素数据(宽*高*4 字节)
    int width = 0; // 帧宽度(像素)
    int height = 0; // 帧高度(像素)
};

// UI 日志消息：由后台线程产生、供界面日志面板显示、带类型标记的文本内容。
struct UiLogMessage {
    int type = 0; // 日志类型/级别(0 普通，1 警告，2 错误等，语义由调用方约定)
    std::string text; // 日志文本内容
};

// 纹理槽位：标识不同用途的 GPU 纹理，供平台后端按槽上传与取用。
enum class TextureSlot {
    UdpVideo,  // 来自 UDP 图传的视频纹理
    CustomVideo,  // 来自自定义字节块解码的视频纹理
    MiniMap,  // 内嵌小地图背景纹理
};

// 纹理视图：封装某纹理槽对应的纹理句柄与尺寸，供渲染时引用。
struct TextureView {
    ImTextureRef texture_ref; // 平台后端提供的纹理引用句柄
    ImVec2 size = ImVec2(0.0f, 0.0f); // 纹理的像素尺寸

    // 判断纹理视图是否可用：句柄有效且宽高均大于 0。
    [[nodiscard]] bool IsReady() const {
        return texture_ref.GetTexID() != ImTextureID_Invalid
               && size.x > 0.0f
               && size.y > 0.0f;
    }
};

// 启动选项：汇总命令行解析出的运行参数与解析后的网络端点。
struct LaunchOptions {
    std::string mqtt_server; // 原始 MQTT 服务器地址(形如 tcp://host:port)
    std::string video_port; // 原始 UDP 视频端口字符串
    std::string mqtt_host; // 解析出的 MQTT 主机地址
    std::string mqtt_port; // 解析出的 MQTT 端口字符串
    int parsed_video_port = 0; // 解析出的 UDP 视频端口(整数)
    bool invert_udp_video = false; // 是否将 UDP 视频做 180 度旋转
};

}// namespace RoboGo::app
