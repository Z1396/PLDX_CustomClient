// platform_backend.h —— 平台抽象层统一接口定义：声明 PlatformBackend 抽象基类与跨平台后端工厂 CreatePlatformBackend()。
#pragma once

#include <pch.h>

#include "app/application_types.h"
#include "resource/resources.h"

namespace RoboGo::platform {

// PlatformBackend —— 平台后端抽象基类，定义跨平台（Windows/Linux）统一接口，主程序仅依赖此接口编程。
class PlatformBackend {
public:
    virtual ~PlatformBackend() = default;

    // 功能：返回当前后端的名称标识（用于日志/调试区分平台）。
    // 思路：纯虚函数，由各后端返回固定字符串（如 "RoboGo-windows" / "RoboGo-linux"）。
    [[nodiscard]] virtual const char *application_name() const = 0;

    // 功能：初始化平台后端（创建窗口、图形设备、ImGui 上下文等）。
    // 思路：纯虚函数，返回是否初始化成功。
    virtual bool Initialize() = 0;

    // 功能：释放后端所有资源（销毁窗口、设备、ImGui 上下文等）。
    // 思路：纯虚函数，需保证可重复/安全调用。
    virtual void Shutdown() = 0;

    // 功能：处理并分发平台事件（窗口消息、输入等）。
    // 思路：纯虚函数，返回 false 表示应退出主循环。
    [[nodiscard]] virtual bool ProcessEvents() = 0;

    // 功能：开始新的一帧（准备 ImGui 帧、处理窗口遮挡/最小化）。
    // 思路：纯虚函数，返回 false 表示本帧应跳过渲染（如窗口最小化为 0 尺寸）。
    [[nodiscard]] virtual bool BeginFrame() = 0;

    // 功能：执行本帧渲染，将 ImGui 绘制数据提交到屏幕。
    // 思路：纯虚函数，draw_data 为 ImGui 当前帧的绘制数据。
    virtual void RenderFrame(ImDrawData *draw_data) = 0;

    // 功能：切换“游戏输入捕获”模式（锁定鼠标到窗口、隐藏光标等）。
    // 思路：纯虚函数，enabled 为 true 时进入捕获模式，false 时退出。
    virtual void SetGameInputCapture(bool enabled) = 0;

    // 功能：从视频帧（VideoFrame）上传纹理到指定纹理槽。
    // 思路：纯虚函数，slot 指定用途（UDP 视频/自定义视频/小地图），frame 含像素缓冲与尺寸。
    [[nodiscard]] virtual bool UploadTexture(app::TextureSlot slot, const app::VideoFrame &frame) = 0;

    // 功能：从解码后的图像（DecodedImage）上传纹理到指定纹理槽。
    // 思路：纯虚函数，目前仅小地图(MiniMap)槽使用此重载。
    [[nodiscard]] virtual bool UploadTexture(app::TextureSlot slot, const RoboGo::resource::DecodedImage &image) = 0;

    // 功能：获取指定纹理槽的纹理视图（供 ImGui 采样显示）。
    // 思路：纯虚函数，返回包含 GPU 句柄与尺寸的 TextureView。
    [[nodiscard]] virtual app::TextureView GetTextureView(app::TextureSlot slot) const = 0;
};

// 功能：根据当前编译平台创建对应平台后端实例。
// 思路：Windows 下返回 WindowsPlatformBackend，Linux 下返回 LinuxPlatformBackend（实现在各自 .cpp 中）。
std::unique_ptr<PlatformBackend> CreatePlatformBackend();

}// namespace RoboGo::platform
