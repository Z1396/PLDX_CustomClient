// linux_platform_backend.cpp —— 平台抽象层的 Linux 实现：基于 GLFW 窗口 + Vulkan 设备/交换链/纹理，
// 实现纹理上传(Vulkan)、交换链重建、游戏输入捕获切换等，对外行为与 Windows 后端保持一致。
#ifndef _WIN32

// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#ifndef _WIN32
#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN  // 让 GLFW 头同时引入 Vulkan 头
#endif
#include <GLFW/glfw3.h>
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#endif
// [/pch-split]
#include "platform/platform_backend.h"  // 包含平台后端抽象接口声明（PlatformBackend 基类、TextureView、TextureSlot 等）

#include "app/imgui_setup.h"  // 包含 ImGui 通用初始化与字体加载函数声明
#include "logging/logging.h"  // 包含 spdlog 日志宏，用于输出运行期诊断信息

// 在调试版本中开启 Vulkan 调试报告（用于捕获验证层告警/错误）。
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT  // 调试构建下定义该宏，开启 Vulkan 调试报告
#endif

// 匿名命名空间 —— 仅本编译单元可见的实现细节：视频纹理结构体、LinuxPlatformBackend 类定义及所有 Vulkan/GLFW
// 辅助工具函数（如扩展查询、内存类型选择、实例/交换链创建等）；不对外暴露，避免污染全局符号。
namespace {

// LinuxVideoTexture —— 单张视频纹理：Vulkan 图像、图像视图、显存、ImGui 描述符集与尺寸状态。
struct LinuxVideoTexture {
    VkImage image = VK_NULL_HANDLE;  // Vulkan 图像对象，纹理像素数据的载体
    VkImageView view = VK_NULL_HANDLE;  // 图像视图，着色器访问图像时的解释方式
    VkDeviceMemory memory = VK_NULL_HANDLE;  // 绑定到图像的设备显存，需手动释放
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;  // ImGui 描述符集，作为 ImTextureID 供 UI 采样
    int width = 0;  // 纹理宽度（像素）
    int height = 0;  // 纹理高度（像素）
    bool ready = false;  // 纹理已创建且有有效帧数据、可供渲染使用的标志
};

// LinuxDynamicTextureSet —— 双缓冲视频纹理集合：轮换使用两张纹理避免上传与渲染相互阻塞。
struct LinuxDynamicTextureSet {
    std::array<LinuxVideoTexture, 2> textures;  // 两张轮换使用的视频纹理（双缓冲）
    int current_index = 0;  // 当前用于渲染显示的那张纹理下标
    int next_index = 1;  // 下一帧待写入上传数据的纹理下标
};

// LinuxPlatformBackend —— PlatformBackend 的 Linux 实现：GLFW 窗口 + Vulkan，承载所有 Vulkan 资源管理。
class LinuxPlatformBackend final : public RoboGo::platform::PlatformBackend {
public:  // 以下为 PlatformBackend 接口的公开实现
    /**
     * @brief 返回本后端的平台标识名称。
     * @note 思路：直接返回编译期字面量 "RoboGo-linux"，用于日志/调试区分当前运行平台。
     * @return C 字符串形式的应用名（静态存储，生命周期安全）。
     */
    [[nodiscard]] const char *application_name() const override {
        return "RoboGo-linux";  // 返回 Linux 平台标识字符串，供日志与调试区分运行平台
    }

    /**
     * @brief 初始化 Linux 后端（GLFW 窗口、Vulkan 实例/设备、交换链、ImGui 上下文）。
     * @note 思路：初始化 GLFW、按 DPI 缩放创建无 API 窗口，建立 Vulkan 实例/设备/表面/交换链，最后初始化 ImGui 的 GLFW/Vulkan 后端。
     */
    bool Initialize() override {
        // 设置 GLFW 错误回调并初始化 GLFW 库。
        glfwSetErrorCallback(GlfwErrorCallback);  // 设置 GLFW 全局错误回调
        if (!glfwInit()) {  // 初始化 GLFW 库，失败则无法继续
            SPDLOG_CRITICAL("glfwInit失败");  // 记录 GLFW 初始化失败
            return false;  // 初始化失败，终止
        }

        // 告知 GLFW 不使用 OpenGL（由 Vulkan 自行管理图形 API），并按主显示器缩放创建窗口尺寸。
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // 告知 GLFW 不使用 OpenGL，图形 API 由 Vulkan 自行管理
        main_scale_ = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());  // 取主显示器内容缩放，用于窗口与字号
        window_ = glfwCreateWindow(static_cast<int>(1280 * main_scale_),  // 窗口宽（按缩放换算）
                                   static_cast<int>(800 * main_scale_),  // 窗口高（按缩放换算）
                                   "RoboGo",  // 窗口标题
                                   nullptr,   // 共享窗口，无
                                   nullptr);  // 指定显示器，无（用默认）
        if (window_ == nullptr) {  // 检查窗口是否创建成功
            SPDLOG_CRITICAL("创建GLFW窗口失败");  // 记录窗口创建失败
            glfwTerminate();  // 终止 GLFW
            return false;  // 窗口创建失败，终止
        }
        if (!glfwVulkanSupported()) {  // 检查当前环境是否提供 Vulkan 支持
            SPDLOG_CRITICAL("当前环境不支持Vulkan");  // 记录 Vulkan 不可用
            glfwDestroyWindow(window_);  // 销毁已创建的窗口
            window_ = nullptr;  // 清空窗口句柄
            glfwTerminate();  // 终止 GLFW
            return false;  // 不支持 Vulkan，终止
        }  // 结束“不支持 Vulkan”分支
        SPDLOG_INFO("GLFW窗口创建成功 size={}x{} scale={}",  // 记录窗口创建成功的日志
                    static_cast<int>(1280 * main_scale_),  // 日志参数：窗口宽度（已按缩放换算）
                    static_cast<int>(800 * main_scale_),  // 日志参数：窗口高度（已按缩放换算）
                    main_scale_);  // 日志参数：DPI 内容缩放因子

        // 收集 GLFW 要求的 Vulkan 实例扩展，供后续创建 Vulkan 实例使用。
        ImVector<const char *> extensions;
        uint32_t extensions_count = 0;  // 输出参数：GLFW 要求的实例扩展数量
        const char **glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);  // 取 GLFW 必需的实例扩展名列表（含窗口系统集成所需的扩展）
        for (uint32_t i = 0; i < extensions_count; ++i) {  // 把每个扩展名逐个收集到 ImVector 中
            extensions.push_back(glfw_extensions[i]);  // 收集当前扩展名
        }

        // 创建 Vulkan 实例、物理/逻辑设备、描述符池、上传命令池与采样器。
        SetupVulkan(extensions);  // 创建 Vulkan 实例/设备/描述符池/采样器等核心对象

        // 由 GLFW 窗口创建 Vulkan 表面，并据此建立交换链窗口数据。
        VkSurfaceKHR surface = VK_NULL_HANDLE;  // 由 GLFW 窗口创建出的 Vulkan 表面
        VkResult err = glfwCreateWindowSurface(instance_, window_, allocator_, &surface);  // 把窗口挂到 Vulkan 表面
        CheckVkResult(err);  // 检查表面创建结果

        int width = 0;  // 帧缓冲宽
        int height = 0;  // 帧缓冲高
        glfwGetFramebufferSize(window_, &width, &height);  // 查询当前帧缓冲像素尺寸
        SetupVulkanWindow(surface, width, height);  // 按表面与尺寸建立交换链窗口数据

        // 初始化通用 ImGui 配置，再初始化 GLFW 与 Vulkan 两个 ImGui 后端。
        RoboGo::app::InitializeCommonImGui(main_scale_);  // 初始化通用 ImGui 样式/配置

        ImGui_ImplGlfw_InitForVulkan(window_, true);  // 初始化 ImGui 的 GLFW 后端（true=安装事件回调）
        // 组装 Vulkan 后端初始化信息并初始化。
        ImGui_ImplVulkan_InitInfo init_info = {};  // 组装 ImGui 的 Vulkan 后端初始化信息
        init_info.Instance = instance_;  // Vulkan 实例
        init_info.PhysicalDevice = physical_device_;  // 物理设备（GPU）
        init_info.Device = device_;  // 逻辑设备
        init_info.QueueFamily = queue_family_;  // 使用的队列族索引
        init_info.Queue = queue_;  // 图形/呈现队列
        init_info.DescriptorPool = descriptor_pool_;  // ImGui 描述符池
        init_info.MinImageCount = min_image_count_;  // 交换链最小图像数
        init_info.ImageCount = main_window_data_.ImageCount;  // 实际图像数
        init_info.Allocator = allocator_;  // Vulkan 分配器回调（默认空）
        init_info.PipelineInfoMain.RenderPass = main_window_data_.RenderPass;  // 渲染通道
        init_info.PipelineInfoMain.Subpass = 0;  // 子通道索引
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;  // 多重采样 1x（关闭）
        init_info.CheckVkResultFn = CheckVkResult;  // Vulkan 结果检查回调
        ImGui_ImplVulkan_Init(&init_info);  // 初始化 ImGui 的 Vulkan 后端

        RoboGo::app::LoadEmbeddedImGuiFont();  // 加载内嵌的 ImGui 字体
        initialized_ = true;  // 标记后端初始化完成
        return true;  // 初始化成功
    }

    /**
     * @brief 释放 Linux 后端所有资源（纹理、ImGui、Vulkan 设备/窗口、GLFW）。
     * @note 思路：先解除输入捕获并等待设备空闲，再释放视频纹理与 ImGui，最后清理 Vulkan 与 GLFW。
     */
    void Shutdown() override {
        if (!initialized_) {  // 未初始化则没有需要释放的资源
            return;  // 提前返回，保证 Shutdown 可重复调用
        }

        SetGameInputCapture(false);  // 退出输入捕获（恢复鼠标）

        // 等待设备空闲，确保没有在途命令后再释放资源。
        VkResult err = vkDeviceWaitIdle(device_);  // 阻塞直到设备完成所有命令
        CheckVkResult(err);  // 检查等待结果

        DestroyDynamicTextureSet(udp_video_textures_);  // 销毁 UDP 双缓冲纹理
        DestroyVulkanTexture(mini_map_texture_);  // 销毁小地图纹理

        ImGui_ImplVulkan_Shutdown();  // 关闭 ImGui Vulkan 后端
        ImGui_ImplGlfw_Shutdown();  // 关闭 ImGui GLFW 后端
        ImGui::DestroyContext();  // 销毁 ImGui 上下文

        CleanupVulkanWindow();  // 销毁交换链窗口数据与表面
        CleanupVulkan();  // 释放 Vulkan 级资源

        glfwDestroyWindow(window_);  // 销毁 GLFW 窗口
        window_ = nullptr;  // 清空窗口句柄
        glfwTerminate();  // 终止 GLFW 库
        initialized_ = false;  // 复位初始化标志
    }

    /**
     * @brief 处理 GLFW 事件并返回是否继续运行。
     * @note 思路：轮询 GLFW 事件，若窗口被请求关闭则返回 false。
     */
    [[nodiscard]] bool ProcessEvents() override {
        glfwPollEvents();  // 处理所有待处理窗口事件
        return !glfwWindowShouldClose(window_);  // 窗口未请求关闭则继续主循环
    }

    /**
     * @brief 开始新的一帧，按需重建交换链并处理窗口最小化暂停。
     * @note 思路：当帧缓冲尺寸变化或被标记重建时重新创建/调整交换链窗口；窗口最小化则返回 false 跳过本帧。
     */
    [[nodiscard]] bool BeginFrame() override {
        int fb_width = 0;  // 帧缓冲宽
        int fb_height = 0;  // 帧缓冲高
        glfwGetFramebufferSize(window_, &fb_width, &fb_height);  // 查询当前帧缓冲尺寸
        if (fb_width > 0  // 尺寸有效时才可能重建交换链
            && fb_height > 0  // 高度也必须有效
            && (swap_chain_rebuild_  // 被标记为需要重建
                || main_window_data_.Width != fb_width  // 或者窗口宽度发生变化
                || main_window_data_.Height != fb_height)) {  // 或者窗口高度发生变化
            ImGui_ImplVulkan_SetMinImageCount(min_image_count_);  // 告知 ImGui 交换链最小图像数，重建时保持一致
            ImGui_ImplVulkanH_CreateOrResizeWindow(instance_,  // 重建或调整交换链窗口数据（Vulkan 实例）
                                                  physical_device_,  // 物理设备
                                                  device_,  // 逻辑设备
                                                  &main_window_data_,  // 交换链窗口数据结构（表面/交换链/渲染通道都在其中）
                                                  queue_family_,  // 使用该队列族重建
                                                  allocator_,  // 内存分配回调
                                                  fb_width,  // 新的帧缓冲宽度
                                                  fb_height,  // 新的帧缓冲高度
                                                  min_image_count_,  // 最小图像数
                                                  0);  // 额外创建标志 0
            main_window_data_.FrameIndex = 0;  // 重建后帧索引归零，避免索引越界
            swap_chain_rebuild_ = false;  // 清除重建标志
        }
        if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED) != 0) {  // 窗口被最小化
            ImGui_ImplGlfw_Sleep(10);  // 暂停 10ms 避免空转
            return false;  // 本帧跳过渲染
        }

        ImGui_ImplVulkan_NewFrame();  // 启动 ImGui 的 Vulkan 后端帧（准备描述符与缓冲）
        ImGui_ImplGlfw_NewFrame();  // 启动 ImGui 的 GLFW 后端帧（采集鼠标键盘输入）
        ImGui::NewFrame();  // 开始新的 ImGui 逻辑帧（重置绘制列表）
        return true;  // 本帧可以正常渲染，返回 true
    }

    /**
     * @brief 渲染并提交本帧，同时清理颜色为不透明黑。
     * @param draw_data 当前帧 ImGui 绘制数据。
     * @note 思路：尺寸非法时直接返回；否则设置清屏色并调用 FrameRender/FramePresent。
     */
    void RenderFrame(ImDrawData *draw_data) override {
        if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) {  // 尺寸非法（窗口不可见）
            return;  // 直接跳过本帧渲染
        }

        // 清屏色设为不透明黑色（RGBA）。
        main_window_data_.ClearValue.color.float32[0] = 0.0f;
        main_window_data_.ClearValue.color.float32[1] = 0.0f;  // 清屏色 G 分量
        main_window_data_.ClearValue.color.float32[2] = 0.0f;  // 清屏色 B 分量
        main_window_data_.ClearValue.color.float32[3] = 1.0f;  // 清屏色 A 分量（1.0 表示完全不透明）
        FrameRender(draw_data);  // 录制并提交本帧渲染命令
        FramePresent();  // 将完成的帧呈现到屏幕
    }

    /**
     * @brief 切换游戏输入捕获模式（隐藏并锁定鼠标）。
     * @param enabled true 隐藏并锁定光标（游戏操控），false 恢复普通光标。
     * @note 思路：enabled 为真时把 GLFW 光标模式设为 DISABLED（隐藏并捕获），否则恢复为 NORMAL。
     */
    void SetGameInputCapture(const bool enabled) override {
        if (window_ == nullptr || game_input_captured_ == enabled) {  // 窗口无效或状态未变化时无需处理
            return;  // 窗口无效或状态已一致，无需处理
        }

        glfwSetInputMode(window_, GLFW_CURSOR, enabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);  // 按开关设置光标模式：DISABLED 隐藏并捕获，NORMAL 恢复普通光标
        if (enabled) {  // 开启捕获时的日志分支
            SPDLOG_DEBUG("已启用Linux隐藏并锁定鼠标模式");  // 调试日志：已进入隐藏并锁定鼠标的模式
        } else {  // 关闭捕获的分支
            SPDLOG_DEBUG("已恢复Linux鼠标模式");  // 调试日志：已恢复普通鼠标模式
        }
        game_input_captured_ = enabled;  // 记录当前捕获状态
    }

    /**
     * @brief 从视频帧上传纹理（按纹理槽分流到动态或静态纹理）。
     * @param slot 目标纹理槽；frame —— 视频帧像素数据（RGBA 缓冲 + 宽高）。
     * @note 思路：UDP/自定义视频走双缓冲动态纹理，小地图走静态纹理。
     * @return true 上传成功，false 槽位不支持或上传失败。
     */
    [[nodiscard]] bool UploadTexture(const RoboGo::app::TextureSlot slot, const RoboGo::app::VideoFrame &frame) override {
        switch (slot) {  // 按纹理槽类型分流到不同的上传实现
            case RoboGo::app::TextureSlot::UdpVideo:  // UDP 图传视频槽
                return UploadDynamicTexture(udp_video_textures_, frame);  // UDP 视频走双缓冲动态纹理上传
            case RoboGo::app::TextureSlot::MiniMap:  // 小地图槽
                return UploadStaticTexture(mini_map_texture_, frame.buffer.data(), frame.width, frame.height);  // 小地图内容变化不频繁，走静态纹理上传
        }
        return false;  // 未知槽位或不支持，返回失败
    }  // 结束 UploadTexture（视频帧重载）

    /**
     * @brief 从解码后的图像上传纹理（仅小地图槽支持该重载）。
     * @param slot 目标纹理槽（仅 MiniMap）；image —— 已解码图像像素数据。
     * @note 思路：仅 MiniMap 槽接受 DecodedImage，其余槽直接拒绝。
     * @return true 上传成功，false 槽位不支持或上传失败。
     */
    [[nodiscard]] bool UploadTexture(const RoboGo::app::TextureSlot slot, const RoboGo::resource::DecodedImage &image) override {
        if (slot != RoboGo::app::TextureSlot::MiniMap) {  // 仅小地图接受 DecodedImage
            return false;  // 槽位不支持，返回失败
        }
        return UploadStaticTexture(mini_map_texture_, image.pixels.data(), image.width, image.height);  // 把解码出的像素上传为小地图静态纹理
    }  // 结束 UploadTexture（解码图像重载）

    /**
     * @brief 获取指定纹理槽当前应显示的纹理视图。
     * @param slot 目标纹理槽。
     * @note 思路：动态纹理取 current_index 指向的一张，静态纹理直接取 mini_map_texture_。
     * @return TextureView（描述符集句柄 + 尺寸）；若无有效纹理返回空视图。
     */
    [[nodiscard]] RoboGo::app::TextureView GetTextureView(const RoboGo::app::TextureSlot slot) const override {
        switch (slot) {  // 按槽位返回对应的纹理视图
            case RoboGo::app::TextureSlot::UdpVideo:  // UDP 视频槽
                return BuildTextureView(udp_video_textures_.textures[udp_video_textures_.current_index]);  // 取当前正在显示的那一张 UDP 纹理
            case RoboGo::app::TextureSlot::MiniMap:  // 小地图槽
                return BuildTextureView(mini_map_texture_);  // 小地图是静态纹理，直接取它
        }
        return {};  // 未命中任何槽位，返回空视图
    }

private:  // 以下为内部实现细节，不对外暴露
    /**
     * @brief GLFW 错误回调，将 GLFW 报错输出到日志。
     */
    static void GlfwErrorCallback(const int error, const char *description) {
        SPDLOG_ERROR("GLFW错误 code={} description={}", error, description);  // 输出 GLFW 报错
    }

    /**
     * @brief 检查 Vulkan 调用结果，非成功则记录错误，错误(<0)时直接终止进程。
     * @param err Vulkan 返回码；VK_SUCCESS 直接返回，非成功记日志，<0（错误）时调用 std::abort 终止。
     */
    static void CheckVkResult(const VkResult err) {
        if (err == VK_SUCCESS) {  // 成功则无需任何处理
            return;  // 直接返回
        }
        SPDLOG_ERROR("Vulkan错误 result={}", static_cast<int>(err));  // 记录非成功返回码
        if (err < 0) {  // 负值表示致命错误，无法继续运行
            std::abort();  // 错误级直接终止进程
        }
    }

#ifdef APP_USE_VULKAN_DEBUG_REPORT  // 调试构建下才注册调试报告回调
    /**
     * @brief Vulkan 调试报告回调，将验证层/性能告警输出到日志。
     */
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugReportCallback(VkDebugReportFlagsEXT flags,
                                                              VkDebugReportObjectTypeEXT object_type,  // 出问题的对象类型（图像/缓冲/管线等）
                                                              uint64_t object,  // 对象句柄的数值
                                                              size_t location,  // 触发位置
                                                              int32_t message_code,  // 消息编码
                                                              const char *layer_prefix,  // 产生消息的层或扩展前缀
                                                              const char *message,  // 消息正文
                                                              void *user_data) {  // 用户数据（此处为本后端指针）
        (void) flags;  // 显式忽略未使用的参数，避免编译告警
        (void) object_type;  // 忽略对象类型参数
        (void) object;  // 忽略对象句柄参数
        (void) location;  // 忽略位置参数
        (void) message_code;  // 忽略消息码参数
        (void) layer_prefix;  // 忽略层前缀参数
        (void) user_data;  // 忽略用户数据参数
        SPDLOG_WARN("Vulkan调试报告 message={}", message);  // 把校验层/驱动的告警写入日志
        return VK_FALSE;  // 返回 VK_FALSE 表示不中断引发该消息的 Vulkan 调用
    }  // 回调必须返回 VK_FALSE，表示不中断调用
#endif

    /**
     * @brief 判断某 Vulkan 扩展是否在给定的扩展属性列表中可用。
     */
    [[nodiscard]] static bool IsExtensionAvailable(const ImVector<VkExtensionProperties> &properties, const char *extension) {
        for (const VkExtensionProperties &property: properties) {  // 遍历可用的扩展属性列表
            if (strcmp(property.extensionName, extension) == 0) {  // 比较扩展名是否完全一致
                return true;  // 找到该扩展
            }
        }
        return false;  // 未找到
    }

    /**
     * @brief 依据类型过滤位与所需内存属性，找到合适的物理设备内存类型索引。
     * @param type_filter 资源要求的内存类型位掩码；properties —— 期望的内存属性（如 DEVICE_LOCAL / HOST_VISIBLE）。
     * @note 思路：遍历物理设备内存类型，同时满足属性标志与类型过滤位则返回索引。
     * @return 匹配的内存类型索引；若都不匹配返回 (uint32_t)-1（调用方应视为错误）。
     */
    [[nodiscard]] uint32_t FindMemoryType(const uint32_t type_filter, const VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memory_properties;  // 物理设备内存属性
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);  // 查询设备可用内存类型
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {  // 遍历物理设备的全部内存类型
            if ((memory_properties.memoryTypes[i].propertyFlags & properties) == properties  // 属性位必须完整包含期望的属性
                && (type_filter & (1u << i))) {  // 并且该内存类型必须在资源允许的掩码内
                return i;  // 命中则返回该内存类型索引
            }
        }
        return static_cast<uint32_t>(-1);  // 无匹配时返回 -1，调用方应视为错误
    }

    /**
     * @brief 创建 Vulkan 实例、物理/逻辑设备、描述符池、纹理上传命令池与采样器。
     * @param instance_extensions GLFW 要求的实例级扩展列表（会被按需追加校验扩展），以值传递并在内部补充。
     * @note 思路：枚举可用扩展后创建实例（按需加验证层/可移植位），选取物理设备与队列族，再创建逻辑设备及相关资源。
     */
    void SetupVulkan(ImVector<const char *> instance_extensions) {
        VkResult err;  // Vulkan 调用返回码

        VkInstanceCreateInfo create_info = {};  // 实例创建信息（清零）
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;  // 填写结构体类型，Vulkan 依赖它做校验

        // 枚举实例级扩展，据此补充必要的可选扩展（如属性查询 2、可移植枚举）。
        uint32_t properties_count = 0;
        ImVector<VkExtensionProperties> properties;  // 用于存放实例级扩展属性列表
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);  // 第一次调用只查询可用扩展的数量
        properties.resize(properties_count);  // 按数量预分配空间
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);  // 第二次调用把扩展属性真正写入缓冲
        CheckVkResult(err);  // 检查枚举结果

        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {  // 可用则启用“设备属性查询 2”扩展（后续查询特性需要它）
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);  // 把它加入启用的扩展列表
        }
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME  // 平台相关：可移植性枚举扩展（部分平台的 Vulkan 实现需要）
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {  // 该平台提供此扩展时才启用
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);  // 加入扩展列表
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;  // 打开可移植设备枚举标志
        }
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT  // 调试构建：启用校验层与调试报告扩展
        const char *layers[] = {"VK_LAYER_KHRONOS_validation"};  // 要启用的层：Khronos 官方校验层
        create_info.enabledLayerCount = 1;  // 启用 1 个层
        create_info.ppEnabledLayerNames = layers;  // 指向层名数组
        instance_extensions.push_back("VK_EXT_debug_report");  // 追加调试报告扩展，用于接收校验层消息
#endif

        create_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.Size);  // 启用扩展的数量
        create_info.ppEnabledExtensionNames = instance_extensions.Data;  // 指向扩展名数组
        // 创建 Vulkan 实例（含前述收集的扩展与验证层）。
        err = vkCreateInstance(&create_info, allocator_, &instance_);
        CheckVkResult(err);  // 检查实例创建结果

#ifdef APP_USE_VULKAN_DEBUG_REPORT  // 调试构建：注册调试报告回调
        // 通过实例函数指针创建调试报告回调（捕获验证层日志）。
        auto *create_debug_report_callback =
                reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugReportCallbackEXT"));  // 通过实例函数指针取得扩展函数地址（该扩展不在核心头文件中声明）
        IM_ASSERT(create_debug_report_callback != nullptr);  // 断言函数地址获取成功
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};  // 调试回调的创建信息（清零）
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;  // 填写结构体类型
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT  // 只关心错误级消息
                                | VK_DEBUG_REPORT_WARNING_BIT_EXT  // 以及警告级消息
                                | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;  // 以及性能警告级消息
        debug_report_ci.pfnCallback = DebugReportCallback;  // 指定回调函数
        debug_report_ci.pUserData = this;  // 把后端指针作为用户数据传入回调
        err = create_debug_report_callback(instance_, &debug_report_ci, allocator_, &debug_report_);  // 创建调试报告回调对象
        CheckVkResult(err);  // 检查创建结果
#endif

        physical_device_ = ImGui_ImplVulkanH_SelectPhysicalDevice(instance_);  // 由 ImGui 辅助函数选择一块合适的 GPU
        IM_ASSERT(physical_device_ != VK_NULL_HANDLE);  // 断言确实选中了物理设备

        queue_family_ = ImGui_ImplVulkanH_SelectQueueFamilyIndex(physical_device_);  // 选择同时支持图形与呈现的队列族
        IM_ASSERT(queue_family_ != static_cast<uint32_t>(-1));  // 断言队列族索引有效

        ImVector<const char *> device_extensions;  // 逻辑设备要启用的扩展列表
        device_extensions.push_back("VK_KHR_swapchain");  // 交换链支持是呈现所必需的扩展
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME  // 平台相关：可移植性子集扩展
        properties_count = 0;  // 复用变量，先清零
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &properties_count, nullptr);  // 查询设备级扩展数量
        properties.resize(properties_count);  // 按数量预分配空间
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &properties_count, properties.Data);  // 把设备扩展属性写入缓冲
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {  // 设备支持该扩展时才启用
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);  // 加入设备扩展列表
        }
#endif

        const float queue_priority[] = {1.0f};  // 队列优先级：单队列取最高
        VkDeviceQueueCreateInfo queue_info = {};  // 队列创建信息（清零）
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;  // 填写结构体类型
        queue_info.queueFamilyIndex = queue_family_;  // 使用前面选定的队列族
        queue_info.queueCount = 1;  // 只创建一个队列
        queue_info.pQueuePriorities = queue_priority;  // 指向优先级数组

        VkDeviceCreateInfo device_create_info = {};  // 逻辑设备创建信息（清零）
        device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;  // 填写结构体类型
        device_create_info.queueCreateInfoCount = 1;  // 创建 1 个队列
        device_create_info.pQueueCreateInfos = &queue_info;  // 指向队列创建信息
        device_create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.Size);  // 启用扩展的数量
        device_create_info.ppEnabledExtensionNames = device_extensions.Data;  // 指向扩展名数组
        err = vkCreateDevice(physical_device_, &device_create_info, allocator_, &device_);  // 创建逻辑设备
        CheckVkResult(err);  // 检查创建结果
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);  // 取出该队列族第 0 个队列的句柄

        // 创建 ImGui 所需的描述符池（采样图像 + 采样器），允许按需释放描述符集。
        VkDescriptorPoolSize pool_sizes[] = {
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE},  // 池中预留的“采样图像”描述符数量
                {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE},  // 池中预留的“采样器”描述符数量
        };
        VkDescriptorPoolCreateInfo pool_info = {};  // 描述符池创建信息（清零）
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;  // 填写结构体类型
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;  // 允许单独释放描述符集（纹理销毁时需要归还）
        pool_info.poolSizeCount = static_cast<uint32_t>(IM_ARRAYSIZE(pool_sizes));  // 池尺寸条目数量
        pool_info.pPoolSizes = pool_sizes;  // 指向池尺寸数组
        pool_info.maxSets = 0;  // 最大描述符集数量，下面累加得到
        for (const auto &pool_size : pool_sizes)  // 遍历各描述符类型的数量
            pool_info.maxSets += pool_size.descriptorCount;  // 累加得到池可分配的描述符集总数
        err = vkCreateDescriptorPool(device_, &pool_info, allocator_, &descriptor_pool_);  // 创建描述符池
        CheckVkResult(err);  // 检查创建结果

        // 创建用于纹理上传的专用命令池（可重置命令缓冲）。
        VkCommandPoolCreateInfo command_pool_info = {};
        command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;  // 填写结构体类型
        command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;  // 允许单独重置命令缓冲，便于反复录制上传命令
        command_pool_info.queueFamilyIndex = queue_family_;  // 命令池归属的队列族
        err = vkCreateCommandPool(device_, &command_pool_info, allocator_, &texture_upload_command_pool_);  // 创建纹理上传专用的命令池
        CheckVkResult(err);  // 检查创建结果

        // 创建线性过滤、钳制边缘的视频纹理采样器。
        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;  // 填写结构体类型
        sampler_info.magFilter = VK_FILTER_LINEAR;  // 放大时线性插值，视频缩放更平滑
        sampler_info.minFilter = VK_FILTER_LINEAR;  // 缩小时同样线性插值
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;  // Mip 之间线性混合（本纹理只有一级，影响很小）
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // U 方向越界采样钳制到边缘，避免出现重复内容
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // V 方向同样钳制
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // W 方向同样钳制
        sampler_info.minLod = -1000.0f;  // 最小 LOD 设得很小，等价于不做下限限制
        sampler_info.maxLod = 1000.0f;  // 最大 LOD 设得很大，等价于不做上限限制
        sampler_info.maxAnisotropy = 1.0f;  // 关闭各向异性过滤
        err = vkCreateSampler(device_, &sampler_info, allocator_, &video_texture_sampler_);  // 创建视频纹理采样器
        CheckVkResult(err);  // 检查创建结果
    }

    /**
     * @brief 校验表面支持并创建交换链窗口数据（选择表面格式/呈现模式/建立交换链）。
     * @param surface 由 GLFW 窗口创建的 Vulkan 表面；width/height —— 初始帧缓冲尺寸。
     * @note 思路：先确认队列族支持该表面，再选择 RGBA 表面格式与 FIFO 呈现模式，最后创建或调整交换链窗口。
     */
    void SetupVulkanWindow(const VkSurfaceKHR surface, const int width, const int height) {
        VkBool32 result = VK_FALSE;  // 表面支持查询结果
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_, surface, &result);  // 查询队列族是否支持该表面
        if (result != VK_TRUE) {  // 队列族不支持该表面则无法呈现，属于致命错误
            SPDLOG_CRITICAL("当前GPU不支持Vulkan窗口表面");  // 记录错误日志
            std::exit(-1);  // 无法补救，直接终止进程
        }

        // 优先尝试 B8G8R8A8 / R8G8B8A8 等常用格式，配合 sRGB 非线性色彩空间。
        const VkFormat request_surface_image_format[] = {
                VK_FORMAT_B8G8R8A8_UNORM,  // 首选 BGRA8（多数桌面平台的交换链原生格式）
                VK_FORMAT_R8G8B8A8_UNORM,  // 其次 RGBA8
                VK_FORMAT_B8G8R8_UNORM,  // 再次无 Alpha 的 BGR8
                VK_FORMAT_R8G8B8_UNORM,  // 最后 RGB8
        };
        const VkColorSpaceKHR request_surface_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR;  // 期望的 sRGB 非线性色彩空间
        main_window_data_.Surface = surface;  // 把表面记录到交换链窗口数据
        main_window_data_.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(  // 由 ImGui 辅助函数从候选中选出实际支持的格式
                physical_device_,  // 物理设备
                main_window_data_.Surface,  // 目标表面
                request_surface_image_format,  // 候选格式数组
                static_cast<size_t>(IM_ARRAYSIZE(request_surface_image_format)),  // 候选格式数量
                request_surface_color_space);  // 期望的色彩空间

        // 呈现模式使用 FIFO（垂直同步），创建/调整交换链窗口数据。
        VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};  // FIFO 即垂直同步，任何实现都支持，最稳妥
        main_window_data_.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(  // 选出实际使用的呈现模式
                physical_device_,  // 物理设备
                main_window_data_.Surface,  // 目标表面
                &present_modes[0],  // 候选呈现模式数组
                IM_ARRAYSIZE(present_modes));  // 候选数量

        ImGui_ImplVulkanH_CreateOrResizeWindow(instance_,  // 首次创建交换链窗口数据（Vulkan 实例）
                                              physical_device_,  // 物理设备
                                              device_,  // 逻辑设备
                                              &main_window_data_,  // 交换链窗口数据结构
                                              queue_family_,  // 队列族索引
                                              allocator_,  // 内存分配回调
                                              width,  // 初始宽度
                                              height,  // 初始高度
                                              min_image_count_,  // 最小图像数
                                              0);  // 额外创建标志 0
    }

    /**
     * @brief 销毁交换链窗口数据并销毁 Vulkan 表面。
     */
    void CleanupVulkanWindow() {
        ImGui_ImplVulkanH_DestroyWindow(instance_, device_, &main_window_data_, allocator_);  // 销毁交换链窗口数据
        if (main_window_data_.Surface != VK_NULL_HANDLE) {  // 表面句柄存在才需要销毁
            vkDestroySurfaceKHR(instance_, main_window_data_.Surface, allocator_);  // 销毁 Vulkan 表面
            main_window_data_.Surface = VK_NULL_HANDLE;  // 复位表面句柄
        }
    }

    /**
     * @brief 释放 Vulkan 级资源（描述符池、采样器、命令池、调试回调、设备、实例）。
     * @note 思路：按创建逆序销毁，调试报告回调经由实例函数指针销毁。
     */
    void CleanupVulkan() {
        vkDestroyDescriptorPool(device_, descriptor_pool_, allocator_);  // 销毁描述符池
        vkDestroySampler(device_, video_texture_sampler_, allocator_);  // 销毁采样器
        vkDestroyCommandPool(device_, texture_upload_command_pool_, allocator_);  // 销毁上传命令池

#ifdef APP_USE_VULKAN_DEBUG_REPORT  // 调试构建：销毁调试报告回调
        auto *destroy_debug_report_callback =  // 声明销毁回调的函数指针变量
                reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugReportCallbackEXT"));  // 通过实例函数指针取得扩展销毁函数地址
        destroy_debug_report_callback(instance_, debug_report_, allocator_);  // 销毁调试报告回调对象
#endif

        vkDestroyDevice(device_, allocator_);  // 销毁逻辑设备（必须晚于所有由它创建的资源）
        vkDestroyInstance(instance_, allocator_);  // 销毁 Vulkan 实例，断开与驱动的连接
    }

    /**
     * @brief 渲染一帧——获取交换链图像、录制命令缓冲、提交渲染命令。
     * @param draw_data 当前帧 ImGui 绘制数据。
     * @note 思路：获取下一图像（过期则标记重建并返回），等待/重置 fence 与命令池，开渲染通道绘制后提交命令。
     */
    void FrameRender(ImDrawData *draw_data) {
        // 取当前信号量，并请求交换链中下一帧可用图像。
        VkSemaphore image_acquired_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].ImageAcquiredSemaphore;  // 图像获取信号量
        VkSemaphore render_complete_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].RenderCompleteSemaphore;  // 渲染完成信号量
        VkResult err = vkAcquireNextImageKHR(device_,  // 向交换链请求一张可用图像
                                             main_window_data_.Swapchain,  // 目标交换链
                                             UINT64_MAX,  // 超时时间：无限等待
                                             image_acquired_semaphore,  // 图像可用时触发该信号量，供后续提交阶段等待
                                             VK_NULL_HANDLE,  // 不使用围栏，这里依靠信号量做同步
                                             &main_window_data_.FrameIndex);  // 输出本次取得的图像下标
        // 交换链过期或次优时标记重建；仅过期则本帧直接返回。
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            swap_chain_rebuild_ = true;  // 标记需要重建交换链
        }
        if (err == VK_ERROR_OUT_OF_DATE_KHR) {  // 交换链已过期，本帧无法渲染
            return;  // 本帧直接返回，等下帧重建后再画
        }
        if (err != VK_SUBOPTIMAL_KHR) {  // 次优属于可接受结果，其它错误才需要检查
            CheckVkResult(err);  // 检查并上报真正的错误
        }

        ImGui_ImplVulkanH_Frame *frame = &main_window_data_.Frames[main_window_data_.FrameIndex];  // 取当前帧数据（含命令缓冲/围栏/帧缓冲）
        // 等待该帧的 fence（上一轮渲染完成），再重置以便本帧复用。
        err = vkWaitForFences(device_, 1, &frame->Fence, VK_TRUE, UINT64_MAX);
        CheckVkResult(err);  // 检查等待围栏的结果
        err = vkResetFences(device_, 1, &frame->Fence);  // 复位围栏，允许本帧重新提交时再次发信号
        CheckVkResult(err);  // 检查复位结果

        // 重置命令池后开始录制一次性命令缓冲。
        err = vkResetCommandPool(device_, frame->CommandPool, 0);
        CheckVkResult(err);  // 检查命令池重置结果

        VkCommandBufferBeginInfo begin_info = {};  // 命令缓冲开始信息（清零）
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;  // 填写结构体类型
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;  // 一次性提交：录完立即使用，不重复录制
        err = vkBeginCommandBuffer(frame->CommandBuffer, &begin_info);  // 开始录制命令缓冲
        CheckVkResult(err);  // 检查开始结果

        // 配置渲染通道（渲染目标、区域、清屏值）并开/关渲染通道，由 ImGui Vulkan 后端绘制。
        VkRenderPassBeginInfo render_pass_info = {};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;  // 填写结构体类型
        render_pass_info.renderPass = main_window_data_.RenderPass;  // 使用交换链窗口的渲染通道
        render_pass_info.framebuffer = frame->Framebuffer;  // 绑定本帧对应的帧缓冲
        render_pass_info.renderArea.extent.width = main_window_data_.Width;  // 渲染区域宽度等于窗口宽度
        render_pass_info.renderArea.extent.height = main_window_data_.Height;  // 渲染区域高度等于窗口高度
        render_pass_info.clearValueCount = 1;  // 只有一个颜色附件需要清屏
        render_pass_info.pClearValues = &main_window_data_.ClearValue;  // 指向清屏颜色（前面设为不透明黑）
        vkCmdBeginRenderPass(frame->CommandBuffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);  // 进入渲染通道，之后的绘制都写入本帧帧缓冲

        ImGui_ImplVulkan_RenderDrawData(draw_data, frame->CommandBuffer);  // 由 ImGui 的 Vulkan 后端录制实际的绘制命令
        vkCmdEndRenderPass(frame->CommandBuffer);  // 退出渲染通道

        // 提交命令：等待“图像获取”信号后于色彩输出阶段开始，完成时发出“渲染完成”信号（供呈现等待）。
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info = {};  // 提交信息（清零）
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;  // 填写结构体类型
        submit_info.waitSemaphoreCount = 1;  // 等待 1 个信号量
        submit_info.pWaitSemaphores = &image_acquired_semaphore;  // 等待“图像已获取”信号量
        submit_info.pWaitDstStageMask = &wait_stage;  // 在色彩输出阶段之前等待该信号量
        submit_info.commandBufferCount = 1;  // 提交 1 个命令缓冲
        submit_info.pCommandBuffers = &frame->CommandBuffer;  // 指向本帧命令缓冲
        submit_info.signalSemaphoreCount = 1;  // 完成时发出 1 个信号量
        submit_info.pSignalSemaphores = &render_complete_semaphore;  // 发出“渲染完成”信号量，供呈现阶段等待

        err = vkEndCommandBuffer(frame->CommandBuffer);
        CheckVkResult(err);  // 检查录制结果
        err = vkQueueSubmit(queue_, 1, &submit_info, frame->Fence);  // 提交到队列，GPU 完成时给本帧围栏发信号
        CheckVkResult(err);  // 检查提交结果
    }

    /**
     * @brief 将已渲染完成的帧呈现到屏幕，并轮转信号量索引。
     * @note 思路：若需重建交换链则跳过；否则用“渲染完成”信号量提交呈现，处理过期/次优，并递增 SemaphoreIndex。
     */
    void FramePresent() {
        if (swap_chain_rebuild_) {  // 需要重建交换链时跳过呈现
            return;  // 直接返回，等下一帧重建后再呈现
        }

        VkSemaphore render_complete_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].RenderCompleteSemaphore;  // 本帧渲染完成信号量
        VkPresentInfoKHR present_info = {};  // 呈现信息（清零）
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;  // 填写结构体类型
        present_info.waitSemaphoreCount = 1;  // 呈现前等待 1 个信号量
        present_info.pWaitSemaphores = &render_complete_semaphore;  // 等待渲染完成信号量
        present_info.swapchainCount = 1;  // 只呈现 1 个交换链
        present_info.pSwapchains = &main_window_data_.Swapchain;  // 指向交换链
        present_info.pImageIndices = &main_window_data_.FrameIndex;  // 要呈现的图像下标
        VkResult err = vkQueuePresentKHR(queue_, &present_info);  // 提交呈现请求
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {  // 过期或次优都标记需要重建
            swap_chain_rebuild_ = true;  // 置重建标志
        }
        if (err == VK_ERROR_OUT_OF_DATE_KHR) {  // 过期则本帧到此结束
            return;  // 直接返回
        }
        if (err != VK_SUBOPTIMAL_KHR) {  // 非次优的其它错误才需要上报
            CheckVkResult(err);  // 检查并上报错误
        }
        main_window_data_.SemaphoreIndex = (main_window_data_.SemaphoreIndex + 1) % main_window_data_.SemaphoreCount;  // 轮转信号量索引（双缓冲）
    }

    /**
     * @brief 从内存 RGBA 像素创建 Vulkan 图像并上传（经暂存缓冲 + 命令缓冲拷贝，含布局屏障转换）。
     * @param data 源像素（RGBA8）；width/height —— 尺寸；texture —— 输出纹理（引用，成功时填充图像/视图/显存）。
     * @note 思路：建设备本地图像并分配显存，建图像视图；再用可映射的暂存缓冲写入像素，提交拷贝与屏障命令后等待完成。
     * @return true 创建并上传成功，false 任一步（图像/显存/视图/缓冲/映射）失败。
     */
    [[nodiscard]] bool CreateVulkanImage(const uint8_t *data, const int width, const int height, LinuxVideoTexture &texture) {
        VkResult err;  // Vulkan 调用返回码

        VkImageCreateInfo image_info = {};  // 图像创建信息（清零）
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;  // 填写结构体类型
        image_info.imageType = VK_IMAGE_TYPE_2D;  // 2D 纹理
        image_info.extent.width = static_cast<uint32_t>(width);  // 图像宽度
        image_info.extent.height = static_cast<uint32_t>(height);  // 图像高度
        image_info.extent.depth = 1;  // 深度为 1（2D 图像）
        image_info.mipLevels = 1;  // 只有一级 Mip
        image_info.arrayLayers = 1;  // 只有一层数组
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;  // RGBA8 无符号归一化格式，与上传的像素数据一致
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;  // 最优瓦片布局，采样效率最高
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // 初始布局未定义，原有内容不需要保留
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // 用途：可被采样 + 可作为传输目标写入
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;  // 独占队列族，不需要跨队列共享
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;  // 不做多重采样

        // 创建设备本地图像（最优瓦片布局，初态未定义，作为采样与传输目标）。
        err = vkCreateImage(device_, &image_info, allocator_, &texture.image);
        if (err != VK_SUCCESS) {  // 创建失败则返回
            return false;  // 上报失败
        }

        // 查询图像内存需求，为其在设备本地内存中分配并绑定显存。
        VkMemoryRequirements image_memory_requirements;
        vkGetImageMemoryRequirements(device_, texture.image, &image_memory_requirements);  // 查询图像的内存大小与允许的内存类型掩码

        VkMemoryAllocateInfo image_alloc_info = {};  // 显存分配信息（清零）
        image_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;  // 填写结构体类型
        image_alloc_info.allocationSize = image_memory_requirements.size;  // 分配大小取图像的内存需求
        image_alloc_info.memoryTypeIndex = FindMemoryType(image_memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);  // 选择设备本地（显存）内存类型

        err = vkAllocateMemory(device_, &image_alloc_info, allocator_, &texture.memory);  // 分配显存
        if (err != VK_SUCCESS) {  // 分配失败则回收已创建的图像
            vkDestroyImage(device_, texture.image, allocator_);  // 销毁刚创建的图像
            texture.image = VK_NULL_HANDLE;  // 复位句柄
            return false;  // 上报失败
        }

        vkBindImageMemory(device_, texture.image, texture.memory, 0);  // 把显存绑定到图像

        // 为图像创建 2D 图像视图（仅颜色面、单层 Mip），供采样使用。
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;  // 填写结构体类型
        view_info.image = texture.image;  // 指明为哪张图像创建视图
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;  // 视图类型为 2D
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;  // 视图格式与图像格式一致
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;  // 只访问颜色面
        view_info.subresourceRange.levelCount = 1;  // 覆盖 1 级 Mip
        view_info.subresourceRange.layerCount = 1;  // 覆盖 1 层数组
        err = vkCreateImageView(device_, &view_info, allocator_, &texture.view);  // 创建图像视图
        if (err != VK_SUCCESS) {  // 创建失败则回收图像与显存
            vkDestroyImage(device_, texture.image, allocator_);  // 销毁图像
            vkFreeMemory(device_, texture.memory, allocator_);  // 释放显存
            texture.image = VK_NULL_HANDLE;  // 复位图像句柄
            texture.memory = VK_NULL_HANDLE;  // 复位显存句柄
            return false;  // 上报失败
        }

        // 创建可映射的暂存缓冲（主机可见），用于把像素从 CPU 侧送入 GPU。
        VkBufferCreateInfo staging_buffer_info = {};
        staging_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;  // 填写结构体类型
        staging_buffer_info.size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4u;  // 缓冲大小 = 宽 × 高 × 4（RGBA 每像素 4 字节）
        staging_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;  // 作为拷贝源使用
        staging_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;  // 独占队列族

        VkBuffer staging_buffer = VK_NULL_HANDLE;  // 暂存缓冲句柄
        err = vkCreateBuffer(device_, &staging_buffer_info, allocator_, &staging_buffer);  // 创建暂存缓冲
        if (err != VK_SUCCESS) {  // 创建失败则回收纹理相关资源
            DestroyVulkanTexture(texture);  // 销毁已创建的图像、视图与显存
            return false;  // 上报失败
        }

        VkMemoryRequirements staging_memory_requirements;  // 暂存缓冲的内存需求
        vkGetBufferMemoryRequirements(device_, staging_buffer, &staging_memory_requirements);  // 查询缓冲的内存大小与允许的内存类型

        VkMemoryAllocateInfo staging_alloc_info = {};  // 显存分配信息（清零）
        staging_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;  // 填写结构体类型
        staging_alloc_info.allocationSize = staging_memory_requirements.size;  // 分配大小取缓冲的内存需求
        staging_alloc_info.memoryTypeIndex = FindMemoryType(staging_memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);  // 选择主机可见内存，CPU 才能映射写入

        VkDeviceMemory staging_memory = VK_NULL_HANDLE;  // 暂存缓冲的显存句柄
        err = vkAllocateMemory(device_, &staging_alloc_info, allocator_, &staging_memory);  // 分配主机可见显存
        if (err != VK_SUCCESS) {  // 分配失败则回收已创建资源
            vkDestroyBuffer(device_, staging_buffer, allocator_);  // 销毁暂存缓冲
            DestroyVulkanTexture(texture);  // 销毁纹理相关资源
            return false;  // 上报失败
        }

        vkBindBufferMemory(device_, staging_buffer, staging_memory, 0);  // 把显存绑定到暂存缓冲

        void *mapped_data = nullptr;  // 映射后 CPU 侧可写指针
        err = vkMapMemory(device_, staging_memory, 0, staging_buffer_info.size, 0, &mapped_data);  // 映射显存到 CPU 地址空间
        if (err != VK_SUCCESS) {  // 映射失败则回收全部临时资源
            vkFreeMemory(device_, staging_memory, allocator_);  // 释放显存
            vkDestroyBuffer(device_, staging_buffer, allocator_);  // 销毁缓冲
            DestroyVulkanTexture(texture);  // 销毁纹理相关资源
            return false;  // 上报失败
        }
        memcpy(mapped_data, data, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);  // 把 RGBA 像素拷入暂存缓冲
        vkUnmapMemory(device_, staging_memory);  // 取消映射（数据已落入显存）

        VkCommandBufferAllocateInfo command_buffer_alloc_info = {};  // 命令缓冲分配信息（清零）
        command_buffer_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;  // 填写结构体类型
        command_buffer_alloc_info.commandPool = texture_upload_command_pool_;  // 从上传专用的命令池分配
        command_buffer_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;  // 主命令缓冲（可直接提交给队列）
        command_buffer_alloc_info.commandBufferCount = 1;  // 只分配 1 个

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;  // 命令缓冲句柄
        err = vkAllocateCommandBuffers(device_, &command_buffer_alloc_info, &command_buffer);  // 分配命令缓冲
        if (err != VK_SUCCESS) {  // 分配失败则回收临时资源
            vkFreeMemory(device_, staging_memory, allocator_);  // 释放暂存显存
            vkDestroyBuffer(device_, staging_buffer, allocator_);  // 销毁暂存缓冲
            DestroyVulkanTexture(texture);  // 销毁纹理相关资源
            return false;  // 上报失败
        }

        VkCommandBufferBeginInfo begin_info = {};  // 命令缓冲开始信息（清零）
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;  // 填写结构体类型
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;  // 一次性提交
        vkBeginCommandBuffer(command_buffer, &begin_info);  // 开始录制命令

        VkImageMemoryBarrier barrier = {};  // 图像内存屏障（清零）
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;  // 填写结构体类型
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // 旧布局未定义，旧内容不需要保留
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;  // 新布局：作为拷贝目标
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;  // 不涉及队列族之间的所有权转移
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;  // 不涉及队列族之间的所有权转移
        barrier.image = texture.image;  // 屏障作用的目标图像
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;  // 只涉及颜色面
        barrier.subresourceRange.levelCount = 1;  // 覆盖 1 级 Mip
        barrier.subresourceRange.layerCount = 1;  // 覆盖 1 层数组
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;  // 之后该图像将被传输写入
        vkCmdPipelineBarrier(command_buffer,  // 提交屏障，把图像转为传输目标布局
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,  // 源阶段：管线最前端
                             VK_PIPELINE_STAGE_TRANSFER_BIT,  // 目的阶段：传输阶段
                             0,  // 不使用依赖标志
                             0, nullptr,  // 无全局内存屏障
                             0, nullptr,  // 无缓冲内存屏障
                             1, &barrier);  // 1 个图像内存屏障

        VkBufferImageCopy region = {};  // 拷贝区域描述（清零）
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;  // 目标为颜色面
        region.imageSubresource.layerCount = 1;  // 拷贝 1 层
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};  // 拷贝范围等于整张图像
        vkCmdCopyBufferToImage(command_buffer,  // 把缓冲中的数据拷贝到图像
                               staging_buffer,  // 拷贝源：暂存缓冲
                               texture.image,  // 拷贝目标：图像
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,  // 目标图像当前必须处于传输目标布局
                               1,  // 1 个拷贝区域
                               &region);  // 指向区域描述

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;  // 旧布局：传输目标
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // 新布局：着色器只读采样
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;  // 之前是传输写入操作
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;  // 之后是着色器读取操作
        vkCmdPipelineBarrier(command_buffer,  // 提交屏障，把图像转为可采样布局
                             VK_PIPELINE_STAGE_TRANSFER_BIT,  // 源阶段：传输
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,  // 目的阶段：片元着色器
                             0,  // 不使用依赖标志
                             0, nullptr,  // 无全局内存屏障
                             0, nullptr,  // 无缓冲内存屏障
                             1, &barrier);  // 1 个图像内存屏障

        vkEndCommandBuffer(command_buffer);

        VkSubmitInfo submit_info = {};  // 提交信息（清零）
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;  // 填写结构体类型
        submit_info.commandBufferCount = 1;  // 提交 1 个命令缓冲
        submit_info.pCommandBuffers = &command_buffer;  // 指向该命令缓冲

        VkFence fence = VK_NULL_HANDLE;  // 临时围栏句柄
        VkFenceCreateInfo fence_info = {};  // 围栏创建信息（清零）
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;  // 填写结构体类型
        vkCreateFence(device_, &fence_info, allocator_, &fence);  // 创建围栏，用于等待上传完成

        vkQueueSubmit(queue_, 1, &submit_info, fence);  // 提交命令并绑定围栏
        vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);  // 阻塞等待上传完成
        vkDestroyFence(device_, fence, allocator_);  // 销毁临时围栏

        vkFreeCommandBuffers(device_, texture_upload_command_pool_, 1, &command_buffer);  // 把命令缓冲归还命令池
        vkFreeMemory(device_, staging_memory, allocator_);  // 释放暂存显存
        vkDestroyBuffer(device_, staging_buffer, allocator_);  // 销毁暂存缓冲

        texture.width = width;  // 记录纹理宽度
        texture.height = height;  // 记录纹理高度
        return true;  // 创建并上传成功
    }

    /**
     * @brief 销毁一张 Vulkan 纹理占用的全部资源（描述符集、视图、图像、显存），并复位结构体字段。
     * @param texture 目标纹理（引用，所有句柄会被复位为空句柄）。
     * @note 思路：逐项判空后调用对应的 vkDestroy / vkFree 系列函数，最后把句柄置回 VK_NULL_HANDLE、宽高归零、ready 置 false，保证 texture 回到「未创建」状态可安全复用。
     */
    void DestroyVulkanTexture(LinuxVideoTexture &texture) {
        if (texture.descriptor_set != VK_NULL_HANDLE) {  // 有描述符集则先从 ImGui 注销
            ImGui_ImplVulkan_RemoveTexture(texture.descriptor_set);  // 把描述符集归还给 ImGui 的描述符池
        }
        if (texture.view != VK_NULL_HANDLE) {  // 图像视图存在则销毁
            vkDestroyImageView(device_, texture.view, allocator_);  // 销毁图像视图
        }
        if (texture.image != VK_NULL_HANDLE) {  // 图像存在则销毁
            vkDestroyImage(device_, texture.image, allocator_);  // 销毁图像
        }
        if (texture.memory != VK_NULL_HANDLE) {  // 显存存在则释放
            vkFreeMemory(device_, texture.memory, allocator_);  // 释放显存
        }  // 结束显存释放

        texture.image = VK_NULL_HANDLE;  // 复位图像句柄
        texture.view = VK_NULL_HANDLE;  // 复位视图句柄
        texture.memory = VK_NULL_HANDLE;  // 复位显存句柄
        texture.descriptor_set = VK_NULL_HANDLE;  // 复位描述符集句柄
        texture.width = 0;  // 宽度归零
        texture.height = 0;  // 高度归零
        texture.ready = false;  // 标记为不可用
    }

    /**
     * @brief 为已创建好的 Vulkan 图像视图登记 ImGui 描述符集，使其可作为纹理被 ImGui 绘制。
     * @param texture 已创建图像视图的纹理（引用，其 descriptor_set 会被写入）。
     * @note 思路：调用 ImGui_ImplVulkan_AddTexture，指定图像布局为 SHADER_READ_ONLY_OPTIMAL（只读采样），返回的 VkDescriptorSet
     *        即 ImGui 绘制时使用的纹理句柄。
     */
    void CreateVideoDescriptorSet(LinuxVideoTexture &texture) {
        texture.descriptor_set = ImGui_ImplVulkan_AddTexture(  // 向 ImGui 登记纹理，返回可用的描述符集
                texture.view,  // 被采样的图像视图
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);  // 采样时图像必须处于只读布局
    }

    /**
     * @brief 向动态纹理集合的“下一张”写入新视频帧，并轮换 current/next 索引（Vulkan 版）。
     * @param texture_set 目标双缓冲纹理集合（引用，会被修改）；frame —— 新视频帧数据。
     * @note 思路：先销毁 next 槽旧图像，创建新 Vulkan 图像并上传，登记 ImGui 描述符集；成功后切换 current/next 索引。
     * @return true 上传与轮换成功，false 创建/上传失败。
     */
    [[nodiscard]] bool UploadDynamicTexture(LinuxDynamicTextureSet &texture_set, const RoboGo::app::VideoFrame &frame) {
        LinuxVideoTexture &next_texture = texture_set.textures[texture_set.next_index];  // 取“下一张”作为写入目标
        if (next_texture.image != VK_NULL_HANDLE) {  // 该槽已有旧图像则先销毁
            DestroyVulkanTexture(next_texture);  // 销毁旧图像以复用
        }
        if (!CreateVulkanImage(frame.buffer.data(), frame.width, frame.height, next_texture)) {  // 创建并上传新的 Vulkan 图像
            return false;  // 创建/上传失败，返回失败
        }
        CreateVideoDescriptorSet(next_texture);  // 为新图像登记 ImGui 描述符集
        next_texture.ready = true;  // 标记已写入有效帧
        texture_set.current_index = texture_set.next_index;  // 切换当前显示指针
        texture_set.next_index = (texture_set.next_index + 1) % 2;  // 轮转下一张写入槽
        next_texture.ready = false;  // 刚写入的这帧尚未就绪（待切换）
        return true;  // 动态纹理上传与轮换成功
    }

    /**
     * @brief 向静态纹理（小地图）上传一次像素数据（Vulkan 版）。
     * @param texture 目标纹理（引用，会被更新）；data —— 源像素；width/height —— 尺寸。
     * @note 思路：先销毁旧图像与视图，创建新 Vulkan 图像并上传，再登记 ImGui 描述符集。
     * @return true 上传成功，false 创建/上传失败。
     */
    [[nodiscard]] bool UploadStaticTexture(LinuxVideoTexture &texture,
                                           const uint8_t *data,  // 源像素数据
                                           const int width,  // 纹理宽度
                                           const int height) {  // 纹理高度
        if (texture.image != VK_NULL_HANDLE) {  // 旧图像存在则先销毁
            DestroyVulkanTexture(texture);  // 销毁旧图像
        }
        if (!CreateVulkanImage(data, width, height, texture)) {  // 创建并上传新的 Vulkan 图像
            return false;  // 创建/上传失败
        }
        CreateVideoDescriptorSet(texture);  // 登记 ImGui 描述符集
        return true;  // 静态纹理上传成功
    }

    /**
     * @brief 销毁纹理组中的全部纹理（双缓冲的两张），用于后端析构或重建时释放显存。
     * @param texture_set 目标双缓冲纹理集合（引用，其两张纹理均被销毁）。
     * @note 思路：遍历 textures 数组逐个调用 DestroyVulkanTexture，不重置索引（对象即将被丢弃）。
     */
    void DestroyDynamicTextureSet(LinuxDynamicTextureSet &texture_set) {
        for (auto &texture: texture_set.textures) {  // 遍历集合中的两张纹理
            DestroyVulkanTexture(texture);  // 销毁集合中的每张纹理
        }
    }

    /**
     * @brief 由视频纹理构建供 ImGui 使用的 TextureView（描述符集句柄 + 像素尺寸）。
     * @param texture 源视频纹理（含描述符集与宽高）。
     * @note 思路：把 ImGui 描述符集（VkDescriptorSet）转为 ImTextureID 填入纹理引用，并携带宽高。
     * @return TextureView；若描述符集为空则返回仅含尺寸 0 的视图。
     */
    [[nodiscard]] static RoboGo::app::TextureView BuildTextureView(const LinuxVideoTexture &texture) {
        RoboGo::app::TextureView texture_view;  // 构造供 ImGui 使用的纹理视图
        if (texture.descriptor_set != VK_NULL_HANDLE) {  // 描述符集有效时才提供可用的纹理引用
            texture_view.texture_ref = ImTextureRef(reinterpret_cast<ImTextureID>(texture.descriptor_set));  // 描述符集转为 ImTextureID
        }
        texture_view.size = ImVec2(static_cast<float>(texture.width), static_cast<float>(texture.height));  // 携带像素尺寸
        return texture_view;  // 返回纹理视图
    }

    VkAllocationCallbacks *allocator_ = nullptr;  // Vulkan 内存分配回调，为空表示使用驱动默认分配器
    VkInstance instance_ = VK_NULL_HANDLE;  // Vulkan 实例，应用与 Vulkan 驱动的顶层连接句柄
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;  // 选中的物理设备（GPU）
    VkDevice device_ = VK_NULL_HANDLE;  // 逻辑设备，创建/销毁各类 Vulkan 资源的句柄
    uint32_t queue_family_ = static_cast<uint32_t>(-1);  // 选中队列族的索引，static_cast<uint32_t>(-1) 表示尚未选定
    VkQueue queue_ = VK_NULL_HANDLE;  // 图形/呈现队列，用于提交命令缓冲
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;  // 描述符池，分配 ImGui 与视频纹理的描述符集
    VkCommandPool texture_upload_command_pool_ = VK_NULL_HANDLE;  // 命令池，分配视频纹理上传用的临时命令缓冲
    VkSampler video_texture_sampler_ = VK_NULL_HANDLE;  // 采样器，定义视频纹理的过滤与寻址方式
#ifdef APP_USE_VULKAN_DEBUG_REPORT  // 调试构建下才存在调试回调成员
    VkDebugReportCallbackEXT debug_report_ = VK_NULL_HANDLE;  // 调试报告回调，用于接收验证层的告警与错误
#endif
    GLFWwindow *window_ = nullptr;  // GLFW 主窗口句柄
    ImGui_ImplVulkanH_Window main_window_data_ = {};  // 交换链窗口数据，含表面、交换链、渲染通道与帧缓冲
    uint32_t min_image_count_ = 2;  // 交换链最小图像数，2 表示双缓冲
    bool swap_chain_rebuild_ = false;  // 交换链需重建标志，窗口尺寸变化或呈现失败时置位
    float main_scale_ = 1.0f;  // 主显示器 DPI 内容缩放因子，用于窗口尺寸与字号缩放
    LinuxDynamicTextureSet udp_video_textures_;  // UDP 图传视频流的双缓冲纹理
    LinuxVideoTexture mini_map_texture_;  // 小地图静态纹理
    bool initialized_ = false;  // 后端已完成 Vulkan/ImGui 初始化并可销毁资源的标志
    bool game_input_captured_ = false;  // 游戏输入捕获状态，启用时窗口独占键鼠输入
};

}  // namespace

namespace RoboGo::platform {  // 进入平台命名空间，声明工厂函数

/**
 * @brief 平台后端工厂函数，创建 Linux 平台对应的后端实例（Linux 构建下返回 LinuxPlatformBackend）。
 * @note 思路：返回基类 unique_ptr 以隐藏具体实现；Windows 构建由 windows_platform_backend.cpp 提供同名函数，主程序只依赖本接口，从而实现跨平台同源代码。
 */
std::unique_ptr<PlatformBackend> CreatePlatformBackend() {
    return std::make_unique<LinuxPlatformBackend>();  // 构造 Linux 后端并以基类指针返回
}

}  // namespace RoboGo::platform

#endif
