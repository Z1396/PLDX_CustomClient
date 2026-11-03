#pragma once  // 头文件包含守卫：防止重复包含
// 说明：本头只放纯数据结构与轻量内联判断，不引入任何实现依赖，便于各层共享。

#include <pch.h>  // 预编译头(PCH)：集中包含常用标准库与第三方库(std::string/vector、ImGui 类型等)

// 应用层公共数据结构：定义视频帧、UI 日志、纹理槽/视图及启动参数等类型。
namespace RoboGo::app {  // 进入应用层命名空间

// 视频帧：承载一帧已解码为 RGBA 的像素数据及其尺寸，用于纹理上传。
struct VideoFrame {  // 视频帧结构：后台线程写入，经无锁队列移动语义交给 UI 线程消费
    std::vector<uint8_t> buffer; // 连续的 RGBA 像素数据(宽*高*4 字节)
    int width = 0; // 帧宽度(像素)
    int height = 0; // 帧高度(像素)
};  // VideoFrame 定义结束

// UI 日志消息：由后台线程产生、供界面日志面板显示、带类型标记的文本内容。
struct UiLogMessage {  // 单条待显示的日志：从解码 worker 线程送到 UI 线程
    int type = 0; // 日志类型/级别(0 普通，1 警告，2 错误等，语义由调用方约定)
    std::string text; // 日志文本内容
};  // UiLogMessage 定义结束

// 纹理槽位：标识不同用途的 GPU 纹理，供平台后端按槽上传与取用。
enum class TextureSlot {  // 用 enum class 限定作用域，避免与其它枚举的枚举项重名
    UdpVideo,  // 来自 UDP 图传的视频纹理
    CustomVideo,  // 来自自定义字节块解码的视频纹理
    MiniMap,  // 内嵌小地图背景纹理
};  // TextureSlot 定义结束

// 纹理视图：封装某纹理槽对应的纹理句柄与尺寸，供渲染时引用。
struct TextureView {  // 每帧从平台后端取出一次的快照，供绘制代码读取
    ImTextureRef texture_ref; // 平台后端提供的纹理引用句柄
    ImVec2 size = ImVec2(0.0f, 0.0f); // 纹理的像素尺寸

    // 判断纹理视图是否可用：句柄有效且宽高均大于 0。
    [[nodiscard]] bool IsReady() const {  // const 成员函数：只读查询，不改变视图内容
        return texture_ref.GetTexID() != ImTextureID_Invalid // 句柄必须不是"无效纹理"这个哨兵值
               && size.x > 0.0f // 宽度必须为正，否则无法采样
               && size.y > 0.0f; // 高度必须为正，三者同时成立才算就绪
    }  // IsReady 结束
};  // TextureView 定义结束

// 启动选项：汇总命令行解析出的运行参数与解析后的网络端点。
struct LaunchOptions {  // 原始字符串参数与解析结果并存，便于日志打印时对照排查
    std::string mqtt_server; // 原始 MQTT 服务器地址(形如 tcp://host:port；也接受不带前缀的 host:port)
    std::string video_port; // 原始 UDP 视频端口字符串
    std::string mqtt_host; // 解析出的 MQTT 主机地址
    std::string mqtt_port; // 解析出的 MQTT 端口字符串
    int parsed_video_port = 0; // 解析出的 UDP 视频端口(整数)
    bool invert_udp_video = false; // 是否将 UDP 视频做 180 度旋转
    std::string capture_mode = "mqtt"; // 网络抓包档位(off/mqtt/all)，默认只抓 MQTT
};  // LaunchOptions 定义结束

}// namespace RoboGo::app  // 应用层命名空间结束
