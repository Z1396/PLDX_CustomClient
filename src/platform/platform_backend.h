// platform_backend.h —— 平台抽象层统一接口定义：声明 PlatformBackend 抽象基类与跨平台后端工厂 CreatePlatformBackend()。
#pragma once

#include <pch.h>   // 预编译头（集中包含常用头以加速编译）
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
// [/pch-split]

#include "app/application_types.h"   // 应用层类型（TextureSlot / VideoFrame / TextureView 等）
#include "resource/resources.h"      // 资源层类型（DecodedImage 等）

// RoboGo::platform —— 平台抽象层命名空间：集中存放与渲染/窗口平台无关的后端接口与工厂，
// 主程序与上层模块只通过此命名空间使用后端，从而对 Windows(DX12) / Linux(Vulkan) 实现透明。
namespace RoboGo::platform {

// PlatformBackend —— 平台后端抽象基类，定义跨平台（Windows/Linux）统一接口，主程序仅依赖此接口编程。
class PlatformBackend {
public:  // 公开接口区：主程序仅通过这些虚函数与后端交互
    virtual ~PlatformBackend() = default;  // 虚析构：保证经基类 unique_ptr 删除派生后端时能正确析构（含私有资源）

    // 功能：返回当前后端的名称标识（用于日志/调试区分平台）。
    // 思路：纯虚函数，由各后端返回固定字符串（如 "RoboGo-windows" / "RoboGo-linux"）。
    // 返回值：以 C 字符串形式返回后端名称，生命周期由后端自身保证（通常是字面量）。
    [[nodiscard]] virtual const char *application_name() const = 0;

    // 功能：初始化平台后端（创建窗口、图形设备、ImGui 上下文等）。
    // 思路：纯虚函数，返回是否初始化成功。
    // 返回值：成功返回 true，任一关键资源创建失败返回 false（调用方应立即终止）。
    virtual bool Initialize() = 0;

    // 功能：释放后端所有资源（销毁窗口、设备、ImGui 上下文等）。
    // 思路：纯虚函数，需保证可重复/安全调用。
    virtual void Shutdown() = 0;

    // 功能：处理并分发平台事件（窗口消息、输入等）。
    // 思路：纯虚函数，返回 false 表示应退出主循环。
    // 返回值：true 表示继续运行主循环，false 表示收到退出请求（如 WM_QUIT / 窗口关闭）。
    [[nodiscard]] virtual bool ProcessEvents() = 0;

    // 功能：开始新的一帧（准备 ImGui 帧、处理窗口遮挡/最小化）。
    // 思路：纯虚函数，返回 false 表示本帧应跳过渲染（如窗口最小化为 0 尺寸）。
    // 返回值：true 表示已进入帧、可继续 RenderFrame；false 表示本帧不渲染（应继续循环等待）。
    [[nodiscard]] virtual bool BeginFrame() = 0;

    // 功能：执行本帧渲染，将 ImGui 绘制数据提交到屏幕。
    // 思路：纯虚函数，draw_data 为 ImGui 当前帧的绘制数据。
    // 参数：draw_data —— ImGui::GetDrawData() 返回的当前帧绘制数据，包含所有窗口/控件的顶点与命令。
    virtual void RenderFrame(ImDrawData *draw_data) = 0;

    // 功能：切换“游戏输入捕获”模式（锁定鼠标到窗口、隐藏光标等）。
    // 思路：纯虚函数，enabled 为 true 时进入捕获模式，false 时退出。
    // 参数：enabled —— true 进入捕获（鼠标裁剪到窗口并隐藏），false 恢复普通光标。
    virtual void SetGameInputCapture(bool enabled) = 0;

    // 功能：从视频帧（VideoFrame）上传纹理到指定纹理槽。
    // 思路：纯虚函数，slot 指定用途（UDP 视频/自定义视频/小地图），frame 含像素缓冲与尺寸。
    // 参数：slot —— 目标纹理槽（UdpVideo/MiniMap）；frame —— 解码后的单帧像素数据（RGBA 缓冲+宽高）。
    // 返回值：true 表示纹理上传成功并可被 GetTextureView 取用，false 表示失败（尺寸非法或后端资源不足）。
    [[nodiscard]] virtual bool UploadTexture(app::TextureSlot slot, const app::VideoFrame &frame) = 0;

    // 功能：从解码后的图像（DecodedImage）上传纹理到指定纹理槽。
    // 思路：纯虚函数，目前仅小地图(MiniMap)槽使用此重载。
    // 参数：slot —— 目标纹理槽（仅 MiniMap 接受）；image —— 已解码的图像（像素缓冲+宽高）。
    // 返回值：true 表示上传成功，false 表示槽位不支持或失败。
    [[nodiscard]] virtual bool UploadTexture(app::TextureSlot slot, const RoboGo::resource::DecodedImage &image) = 0;

    // 功能：获取指定纹理槽的纹理视图（供 ImGui 采样显示）。
    // 思路：纯虚函数，返回包含 GPU 句柄与尺寸的 TextureView。
    // 参数：slot —— 目标纹理槽（UdpVideo/MiniMap）。
    // 返回值：TextureView（含纹理引用 ImTextureID 与像素尺寸）；若该槽尚无有效纹理则返回空视图。
    [[nodiscard]] virtual app::TextureView GetTextureView(app::TextureSlot slot) const = 0;
};  // 结束 class PlatformBackend：纯接口，无数据成员，实现全部下放到具体平台后端

// 功能：根据当前编译平台创建对应平台后端实例。
// 思路：Windows 下返回 WindowsPlatformBackend，Linux 下返回 LinuxPlatformBackend（实现在各自 .cpp 中）。
// 返回值：指向具体后端实例的基类 unique_ptr；主程序仅依赖 PlatformBackend 接口，从而与平台解耦。
std::unique_ptr<PlatformBackend> CreatePlatformBackend();

}  // namespace RoboGo::platform
