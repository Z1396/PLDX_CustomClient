// linux_platform_backend.cpp —— 平台抽象层的 Linux 实现：基于 GLFW 窗口 + Vulkan 设备/交换链/纹理，
// 实现纹理上传(Vulkan)、交换链重建、游戏输入捕获切换等，对外行为与 Windows 后端保持一致。
#ifndef _WIN32

#include "platform/platform_backend.h"

#include "app/imgui_setup.h"
#include "logging/logging.h"

// 在调试版本中开启 Vulkan 调试报告（用于捕获验证层告警/错误）。
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
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
public:
    // 功能：返回本后端的平台标识名称。
    // 思路：直接返回编译期字面量 "RoboGo-linux"，用于日志/调试区分当前运行平台。
    // 返回值：C 字符串形式的应用名（静态存储，生命周期安全）。
    [[nodiscard]] const char *application_name() const override {
        return "RoboGo-linux";
    }

    // 功能：初始化 Linux 后端（GLFW 窗口、Vulkan 实例/设备、交换链、ImGui 上下文）。
    // 思路：初始化 GLFW、按 DPI 缩放创建无 API 窗口，建立 Vulkan 实例/设备/表面/交换链，最后初始化 ImGui 的 GLFW/Vulkan 后端。
    bool Initialize() override {
        // 设置 GLFW 错误回调并初始化 GLFW 库。
        glfwSetErrorCallback(GlfwErrorCallback);
        if (!glfwInit()) {
            SPDLOG_CRITICAL("glfwInit失败");
            return false;
        }

        // 告知 GLFW 不使用 OpenGL（由 Vulkan 自行管理图形 API），并按主显示器缩放创建窗口尺寸。
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        main_scale_ = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
        window_ = glfwCreateWindow(static_cast<int>(1280 * main_scale_),
                                   static_cast<int>(800 * main_scale_),
                                   "RoboGo",
                                   nullptr,
                                   nullptr);
        if (window_ == nullptr) {
            SPDLOG_CRITICAL("创建GLFW窗口失败");
            glfwTerminate();
            return false;
        }
        if (!glfwVulkanSupported()) {
            SPDLOG_CRITICAL("当前环境不支持Vulkan");
            glfwDestroyWindow(window_);
            window_ = nullptr;
            glfwTerminate();
            return false;
        }
        SPDLOG_INFO("GLFW窗口创建成功 size={}x{} scale={}",
                    static_cast<int>(1280 * main_scale_),
                    static_cast<int>(800 * main_scale_),
                    main_scale_);

        // 收集 GLFW 要求的 Vulkan 实例扩展，供后续创建 Vulkan 实例使用。
        ImVector<const char *> extensions;
        uint32_t extensions_count = 0;
        const char **glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
        for (uint32_t i = 0; i < extensions_count; ++i) {
            extensions.push_back(glfw_extensions[i]);
        }

        // 创建 Vulkan 实例、物理/逻辑设备、描述符池、上传命令池与采样器。
        SetupVulkan(extensions);

        // 由 GLFW 窗口创建 Vulkan 表面，并据此建立交换链窗口数据。
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult err = glfwCreateWindowSurface(instance_, window_, allocator_, &surface);
        CheckVkResult(err);

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        SetupVulkanWindow(surface, width, height);

        // 初始化通用 ImGui 配置，再初始化 GLFW 与 Vulkan 两个 ImGui 后端。
        RoboGo::app::InitializeCommonImGui(main_scale_);

        ImGui_ImplGlfw_InitForVulkan(window_, true);
        // 组装 Vulkan 后端初始化信息并初始化。
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = instance_;
        init_info.PhysicalDevice = physical_device_;
        init_info.Device = device_;
        init_info.QueueFamily = queue_family_;
        init_info.Queue = queue_;
        init_info.DescriptorPool = descriptor_pool_;
        init_info.MinImageCount = min_image_count_;
        init_info.ImageCount = main_window_data_.ImageCount;
        init_info.Allocator = allocator_;
        init_info.PipelineInfoMain.RenderPass = main_window_data_.RenderPass;
        init_info.PipelineInfoMain.Subpass = 0;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.CheckVkResultFn = CheckVkResult;
        ImGui_ImplVulkan_Init(&init_info);

        RoboGo::app::LoadEmbeddedImGuiFont();
        initialized_ = true;
        return true;
    }

    // 功能：释放 Linux 后端所有资源（纹理、ImGui、Vulkan 设备/窗口、GLFW）。
    // 思路：先解除输入捕获并等待设备空闲，再释放视频纹理与 ImGui，最后清理 Vulkan 与 GLFW。
    void Shutdown() override {
        if (!initialized_) {
            return;
        }

        SetGameInputCapture(false);

        // 等待设备空闲，确保没有在途命令后再释放资源。
        VkResult err = vkDeviceWaitIdle(device_);
        CheckVkResult(err);

        DestroyDynamicTextureSet(udp_video_textures_);
        DestroyDynamicTextureSet(custom_video_textures_);
        DestroyVulkanTexture(mini_map_texture_);

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        CleanupVulkanWindow();
        CleanupVulkan();

        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
        initialized_ = false;
    }

    // 功能：处理 GLFW 事件并返回是否继续运行。
    // 思路：轮询 GLFW 事件，若窗口被请求关闭则返回 false。
    [[nodiscard]] bool ProcessEvents() override {
        glfwPollEvents();
        return !glfwWindowShouldClose(window_);
    }

    // 功能：开始新的一帧，按需重建交换链并处理窗口最小化暂停。
    // 思路：当帧缓冲尺寸变化或被标记重建时重新创建/调整交换链窗口；窗口最小化则返回 false 跳过本帧。
    [[nodiscard]] bool BeginFrame() override {
        int fb_width = 0;
        int fb_height = 0;
        glfwGetFramebufferSize(window_, &fb_width, &fb_height);
        if (fb_width > 0
            && fb_height > 0
            && (swap_chain_rebuild_
                || main_window_data_.Width != fb_width
                || main_window_data_.Height != fb_height)) {
            ImGui_ImplVulkan_SetMinImageCount(min_image_count_);
            ImGui_ImplVulkanH_CreateOrResizeWindow(instance_,
                                                  physical_device_,
                                                  device_,
                                                  &main_window_data_,
                                                  queue_family_,
                                                  allocator_,
                                                  fb_width,
                                                  fb_height,
                                                  min_image_count_,
                                                  0);
            main_window_data_.FrameIndex = 0;
            swap_chain_rebuild_ = false;
        }
        if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED) != 0) {
            ImGui_ImplGlfw_Sleep(10);
            return false;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        return true;
    }

    // 功能：渲染并提交本帧，同时清理颜色为不透明黑。
    // 思路：尺寸非法时直接返回；否则设置清屏色并调用 FrameRender/FramePresent。
    // 参数：draw_data —— 当前帧 ImGui 绘制数据。
    void RenderFrame(ImDrawData *draw_data) override {
        if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) {
            return;
        }

        // 清屏色设为不透明黑色（RGBA）。
        main_window_data_.ClearValue.color.float32[0] = 0.0f;
        main_window_data_.ClearValue.color.float32[1] = 0.0f;
        main_window_data_.ClearValue.color.float32[2] = 0.0f;
        main_window_data_.ClearValue.color.float32[3] = 1.0f;
        FrameRender(draw_data);
        FramePresent();
    }

    // 功能：切换游戏输入捕获模式（隐藏并锁定鼠标）。
    // 思路：enabled 为真时把 GLFW 光标模式设为 DISABLED（隐藏并捕获），否则恢复为 NORMAL。
    // 参数：enabled —— true 隐藏并锁定光标（游戏操控），false 恢复普通光标。
    void SetGameInputCapture(const bool enabled) override {
        if (window_ == nullptr || game_input_captured_ == enabled) {
            return;
        }

        glfwSetInputMode(window_, GLFW_CURSOR, enabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        if (enabled) {
            SPDLOG_DEBUG("已启用Linux隐藏并锁定鼠标模式");
        } else {
            SPDLOG_DEBUG("已恢复Linux鼠标模式");
        }
        game_input_captured_ = enabled;
    }

    // 功能：从视频帧上传纹理（按纹理槽分流到动态或静态纹理）。
    // 思路：UDP/自定义视频走双缓冲动态纹理，小地图走静态纹理。
    // 参数：slot —— 目标纹理槽；frame —— 视频帧像素数据（RGBA 缓冲 + 宽高）。
    // 返回值：true 上传成功，false 槽位不支持或上传失败。
    [[nodiscard]] bool UploadTexture(const RoboGo::app::TextureSlot slot, const RoboGo::app::VideoFrame &frame) override {
        switch (slot) {
            case RoboGo::app::TextureSlot::UdpVideo:
                return UploadDynamicTexture(udp_video_textures_, frame);
            case RoboGo::app::TextureSlot::CustomVideo:
                return UploadDynamicTexture(custom_video_textures_, frame);
            case RoboGo::app::TextureSlot::MiniMap:
                return UploadStaticTexture(mini_map_texture_, frame.buffer.data(), frame.width, frame.height);
        }
        return false;
    }

    // 功能：从解码后的图像上传纹理（仅小地图槽支持该重载）。
    // 思路：仅 MiniMap 槽接受 DecodedImage，其余槽直接拒绝。
    // 参数：slot —— 目标纹理槽（仅 MiniMap）；image —— 已解码图像像素数据。
    // 返回值：true 上传成功，false 槽位不支持或上传失败。
    [[nodiscard]] bool UploadTexture(const RoboGo::app::TextureSlot slot, const RoboGo::resource::DecodedImage &image) override {
        if (slot != RoboGo::app::TextureSlot::MiniMap) {
            return false;
        }
        return UploadStaticTexture(mini_map_texture_, image.pixels.data(), image.width, image.height);
    }

    // 功能：获取指定纹理槽当前应显示的纹理视图。
    // 思路：动态纹理取 current_index 指向的一张，静态纹理直接取 mini_map_texture_。
    // 参数：slot —— 目标纹理槽。
    // 返回值：TextureView（描述符集句柄 + 尺寸）；若无有效纹理返回空视图。
    [[nodiscard]] RoboGo::app::TextureView GetTextureView(const RoboGo::app::TextureSlot slot) const override {
        switch (slot) {
            case RoboGo::app::TextureSlot::UdpVideo:
                return BuildTextureView(udp_video_textures_.textures[udp_video_textures_.current_index]);
            case RoboGo::app::TextureSlot::CustomVideo:
                return BuildTextureView(custom_video_textures_.textures[custom_video_textures_.current_index]);
            case RoboGo::app::TextureSlot::MiniMap:
                return BuildTextureView(mini_map_texture_);
        }
        return {};
    }

private:
    // 功能：GLFW 错误回调，将 GLFW 报错输出到日志。
    static void GlfwErrorCallback(const int error, const char *description) {
        SPDLOG_ERROR("GLFW错误 code={} description={}", error, description);
    }

    // 功能：检查 Vulkan 调用结果，非成功则记录错误，错误(<0)时直接终止进程。
    // 参数：err —— Vulkan 返回码；VK_SUCCESS 直接返回，非成功记日志，<0（错误）时调用 std::abort 终止。
    static void CheckVkResult(const VkResult err) {
        if (err == VK_SUCCESS) {
            return;
        }
        SPDLOG_ERROR("Vulkan错误 result={}", static_cast<int>(err));
        if (err < 0) {
            std::abort();
        }
    }

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    // 功能：Vulkan 调试报告回调，将验证层/性能告警输出到日志。
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugReportCallback(VkDebugReportFlagsEXT flags,
                                                              VkDebugReportObjectTypeEXT object_type,
                                                              uint64_t object,
                                                              size_t location,
                                                              int32_t message_code,
                                                              const char *layer_prefix,
                                                              const char *message,
                                                              void *user_data) {
        (void) flags;
        (void) object_type;
        (void) object;
        (void) location;
        (void) message_code;
        (void) layer_prefix;
        (void) user_data;
        SPDLOG_WARN("Vulkan调试报告 message={}", message);
        return VK_FALSE;
    }
#endif

    // 功能：判断某 Vulkan 扩展是否在给定的扩展属性列表中可用。
    [[nodiscard]] static bool IsExtensionAvailable(const ImVector<VkExtensionProperties> &properties, const char *extension) {
        for (const VkExtensionProperties &property: properties) {
            if (strcmp(property.extensionName, extension) == 0) {
                return true;
            }
        }
        return false;
    }

    // 功能：依据类型过滤位与所需内存属性，找到合适的物理设备内存类型索引。
    // 思路：遍历物理设备内存类型，同时满足属性标志与类型过滤位则返回索引。
    // 参数：type_filter —— 资源要求的内存类型位掩码；properties —— 期望的内存属性（如 DEVICE_LOCAL / HOST_VISIBLE）。
    // 返回值：匹配的内存类型索引；若都不匹配返回 (uint32_t)-1（调用方应视为错误）。
    [[nodiscard]] uint32_t FindMemoryType(const uint32_t type_filter, const VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memory_properties;
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((memory_properties.memoryTypes[i].propertyFlags & properties) == properties
                && (type_filter & (1u << i))) {
                return i;
            }
        }
        return static_cast<uint32_t>(-1);
    }

    // 功能：创建 Vulkan 实例、物理/逻辑设备、描述符池、纹理上传命令池与采样器。
    // 思路：枚举可用扩展后创建实例（按需加验证层/可移植位），选取物理设备与队列族，再创建逻辑设备及相关资源。
    // 参数：instance_extensions —— GLFW 要求的实例级扩展列表（会被按需追加校验扩展），以值传递并在内部补充。
    void SetupVulkan(ImVector<const char *> instance_extensions) {
        VkResult err;

        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        // 枚举实例级扩展，据此补充必要的可选扩展（如属性查询 2、可移植枚举）。
        uint32_t properties_count = 0;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        CheckVkResult(err);

        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        }
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        create_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.Size);
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        // 创建 Vulkan 实例（含前述收集的扩展与验证层）。
        err = vkCreateInstance(&create_info, allocator_, &instance_);
        CheckVkResult(err);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        // 通过实例函数指针创建调试报告回调（捕获验证层日志）。
        auto *create_debug_report_callback =
                reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugReportCallbackEXT"));
        IM_ASSERT(create_debug_report_callback != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT
                                | VK_DEBUG_REPORT_WARNING_BIT_EXT
                                | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = DebugReportCallback;
        debug_report_ci.pUserData = this;
        err = create_debug_report_callback(instance_, &debug_report_ci, allocator_, &debug_report_);
        CheckVkResult(err);
#endif

        physical_device_ = ImGui_ImplVulkanH_SelectPhysicalDevice(instance_);
        IM_ASSERT(physical_device_ != VK_NULL_HANDLE);

        queue_family_ = ImGui_ImplVulkanH_SelectQueueFamilyIndex(physical_device_);
        IM_ASSERT(queue_family_ != static_cast<uint32_t>(-1));

        ImVector<const char *> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        properties_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &properties_count, properties.Data);
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        }
#endif

        const float queue_priority[] = {1.0f};
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = queue_priority;

        VkDeviceCreateInfo device_create_info = {};
        device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_create_info.queueCreateInfoCount = 1;
        device_create_info.pQueueCreateInfos = &queue_info;
        device_create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.Size);
        device_create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(physical_device_, &device_create_info, allocator_, &device_);
        CheckVkResult(err);
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        // 创建 ImGui 所需的描述符池（采样图像 + 采样器），允许按需释放描述符集。
        VkDescriptorPoolSize pool_sizes[] = {
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE},
                {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.poolSizeCount = static_cast<uint32_t>(IM_ARRAYSIZE(pool_sizes));
        pool_info.pPoolSizes = pool_sizes;
        pool_info.maxSets = 0;
        for (const auto &pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        err = vkCreateDescriptorPool(device_, &pool_info, allocator_, &descriptor_pool_);
        CheckVkResult(err);

        // 创建用于纹理上传的专用命令池（可重置命令缓冲）。
        VkCommandPoolCreateInfo command_pool_info = {};
        command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_pool_info.queueFamilyIndex = queue_family_;
        err = vkCreateCommandPool(device_, &command_pool_info, allocator_, &texture_upload_command_pool_);
        CheckVkResult(err);

        // 创建线性过滤、钳制边缘的视频纹理采样器。
        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.minLod = -1000.0f;
        sampler_info.maxLod = 1000.0f;
        sampler_info.maxAnisotropy = 1.0f;
        err = vkCreateSampler(device_, &sampler_info, allocator_, &video_texture_sampler_);
        CheckVkResult(err);
    }

    // 功能：校验表面支持并创建交换链窗口数据（选择表面格式/呈现模式/建立交换链）。
    // 思路：先确认队列族支持该表面，再选择 RGBA 表面格式与 FIFO 呈现模式，最后创建或调整交换链窗口。
    // 参数：surface —— 由 GLFW 窗口创建的 Vulkan 表面；width/height —— 初始帧缓冲尺寸。
    void SetupVulkanWindow(const VkSurfaceKHR surface, const int width, const int height) {
        VkBool32 result = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_, surface, &result);
        if (result != VK_TRUE) {
            SPDLOG_CRITICAL("当前GPU不支持Vulkan窗口表面");
            std::exit(-1);
        }

        // 优先尝试 B8G8R8A8 / R8G8B8A8 等常用格式，配合 sRGB 非线性色彩空间。
        const VkFormat request_surface_image_format[] = {
                VK_FORMAT_B8G8R8A8_UNORM,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_FORMAT_B8G8R8_UNORM,
                VK_FORMAT_R8G8B8_UNORM,
        };
        const VkColorSpaceKHR request_surface_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
        main_window_data_.Surface = surface;
        main_window_data_.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
                physical_device_,
                main_window_data_.Surface,
                request_surface_image_format,
                static_cast<size_t>(IM_ARRAYSIZE(request_surface_image_format)),
                request_surface_color_space);

        // 呈现模式使用 FIFO（垂直同步），创建/调整交换链窗口数据。
        VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
        main_window_data_.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
                physical_device_,
                main_window_data_.Surface,
                &present_modes[0],
                IM_ARRAYSIZE(present_modes));

        ImGui_ImplVulkanH_CreateOrResizeWindow(instance_,
                                              physical_device_,
                                              device_,
                                              &main_window_data_,
                                              queue_family_,
                                              allocator_,
                                              width,
                                              height,
                                              min_image_count_,
                                              0);
    }

    // 功能：销毁交换链窗口数据并销毁 Vulkan 表面。
    void CleanupVulkanWindow() {
        ImGui_ImplVulkanH_DestroyWindow(instance_, device_, &main_window_data_, allocator_);
        if (main_window_data_.Surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, main_window_data_.Surface, allocator_);
            main_window_data_.Surface = VK_NULL_HANDLE;
        }
    }

    // 功能：释放 Vulkan 级资源（描述符池、采样器、命令池、调试回调、设备、实例）。
    // 思路：按创建逆序销毁，调试报告回调经由实例函数指针销毁。
    void CleanupVulkan() {
        vkDestroyDescriptorPool(device_, descriptor_pool_, allocator_);
        vkDestroySampler(device_, video_texture_sampler_, allocator_);
        vkDestroyCommandPool(device_, texture_upload_command_pool_, allocator_);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto *destroy_debug_report_callback =
                reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugReportCallbackEXT"));
        destroy_debug_report_callback(instance_, debug_report_, allocator_);
#endif

        vkDestroyDevice(device_, allocator_);
        vkDestroyInstance(instance_, allocator_);
    }

    // 功能：渲染一帧——获取交换链图像、录制命令缓冲、提交渲染命令。
    // 思路：获取下一图像（过期则标记重建并返回），等待/重置 fence 与命令池，开渲染通道绘制后提交命令。
    // 参数：draw_data —— 当前帧 ImGui 绘制数据。
    void FrameRender(ImDrawData *draw_data) {
        // 取当前信号量，并请求交换链中下一帧可用图像。
        VkSemaphore image_acquired_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].ImageAcquiredSemaphore;
        VkSemaphore render_complete_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].RenderCompleteSemaphore;
        VkResult err = vkAcquireNextImageKHR(device_,
                                             main_window_data_.Swapchain,
                                             UINT64_MAX,
                                             image_acquired_semaphore,
                                             VK_NULL_HANDLE,
                                             &main_window_data_.FrameIndex);
        // 交换链过期或次优时标记重建；仅过期则本帧直接返回。
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            swap_chain_rebuild_ = true;
        }
        if (err == VK_ERROR_OUT_OF_DATE_KHR) {
            return;
        }
        if (err != VK_SUBOPTIMAL_KHR) {
            CheckVkResult(err);
        }

        ImGui_ImplVulkanH_Frame *frame = &main_window_data_.Frames[main_window_data_.FrameIndex];
        // 等待该帧的 fence（上一轮渲染完成），再重置以便本帧复用。
        err = vkWaitForFences(device_, 1, &frame->Fence, VK_TRUE, UINT64_MAX);
        CheckVkResult(err);
        err = vkResetFences(device_, 1, &frame->Fence);
        CheckVkResult(err);

        // 重置命令池后开始录制一次性命令缓冲。
        err = vkResetCommandPool(device_, frame->CommandPool, 0);
        CheckVkResult(err);

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(frame->CommandBuffer, &begin_info);
        CheckVkResult(err);

        // 配置渲染通道（渲染目标、区域、清屏值）并开/关渲染通道，由 ImGui Vulkan 后端绘制。
        VkRenderPassBeginInfo render_pass_info = {};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_info.renderPass = main_window_data_.RenderPass;
        render_pass_info.framebuffer = frame->Framebuffer;
        render_pass_info.renderArea.extent.width = main_window_data_.Width;
        render_pass_info.renderArea.extent.height = main_window_data_.Height;
        render_pass_info.clearValueCount = 1;
        render_pass_info.pClearValues = &main_window_data_.ClearValue;
        vkCmdBeginRenderPass(frame->CommandBuffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

        ImGui_ImplVulkan_RenderDrawData(draw_data, frame->CommandBuffer);
        vkCmdEndRenderPass(frame->CommandBuffer);

        // 提交命令：等待“图像获取”信号后于色彩输出阶段开始，完成时发出“渲染完成”信号（供呈现等待）。
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &image_acquired_semaphore;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &frame->CommandBuffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(frame->CommandBuffer);
        CheckVkResult(err);
        err = vkQueueSubmit(queue_, 1, &submit_info, frame->Fence);
        CheckVkResult(err);
    }

    // 功能：将已渲染完成的帧呈现到屏幕，并轮转信号量索引。
    // 思路：若需重建交换链则跳过；否则用“渲染完成”信号量提交呈现，处理过期/次优，并递增 SemaphoreIndex。
    void FramePresent() {
        if (swap_chain_rebuild_) {
            return;
        }

        VkSemaphore render_complete_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].RenderCompleteSemaphore;
        VkPresentInfoKHR present_info = {};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &render_complete_semaphore;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &main_window_data_.Swapchain;
        present_info.pImageIndices = &main_window_data_.FrameIndex;
        VkResult err = vkQueuePresentKHR(queue_, &present_info);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            swap_chain_rebuild_ = true;
        }
        if (err == VK_ERROR_OUT_OF_DATE_KHR) {
            return;
        }
        if (err != VK_SUBOPTIMAL_KHR) {
            CheckVkResult(err);
        }
        main_window_data_.SemaphoreIndex = (main_window_data_.SemaphoreIndex + 1) % main_window_data_.SemaphoreCount;
    }

    // 功能：从内存 RGBA 像素创建 Vulkan 图像并上传（经暂存缓冲 + 命令缓冲拷贝，含布局屏障转换）。
    // 思路：建设备本地图像并分配显存，建图像视图；再用可映射的暂存缓冲写入像素，提交拷贝与屏障命令后等待完成。
    // 参数：data —— 源像素（RGBA8）；width/height —— 尺寸；texture —— 输出纹理（引用，成功时填充图像/视图/显存）。
    // 返回值：true 创建并上传成功，false 任一步（图像/显存/视图/缓冲/映射）失败。
    [[nodiscard]] bool CreateVulkanImage(const uint8_t *data, const int width, const int height, LinuxVideoTexture &texture) {
        VkResult err;

        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent.width = static_cast<uint32_t>(width);
        image_info.extent.height = static_cast<uint32_t>(height);
        image_info.extent.depth = 1;
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;

        // 创建设备本地图像（最优瓦片布局，初态未定义，作为采样与传输目标）。
        err = vkCreateImage(device_, &image_info, allocator_, &texture.image);
        if (err != VK_SUCCESS) {
            return false;
        }

        // 查询图像内存需求，为其在设备本地内存中分配并绑定显存。
        VkMemoryRequirements image_memory_requirements;
        vkGetImageMemoryRequirements(device_, texture.image, &image_memory_requirements);

        VkMemoryAllocateInfo image_alloc_info = {};
        image_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        image_alloc_info.allocationSize = image_memory_requirements.size;
        image_alloc_info.memoryTypeIndex = FindMemoryType(image_memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        err = vkAllocateMemory(device_, &image_alloc_info, allocator_, &texture.memory);
        if (err != VK_SUCCESS) {
            vkDestroyImage(device_, texture.image, allocator_);
            texture.image = VK_NULL_HANDLE;
            return false;
        }

        vkBindImageMemory(device_, texture.image, texture.memory, 0);

        // 为图像创建 2D 图像视图（仅颜色面、单层 Mip），供采样使用。
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = texture.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        err = vkCreateImageView(device_, &view_info, allocator_, &texture.view);
        if (err != VK_SUCCESS) {
            vkDestroyImage(device_, texture.image, allocator_);
            vkFreeMemory(device_, texture.memory, allocator_);
            texture.image = VK_NULL_HANDLE;
            texture.memory = VK_NULL_HANDLE;
            return false;
        }

        // 创建可映射的暂存缓冲（主机可见），用于把像素从 CPU 侧送入 GPU。
        VkBufferCreateInfo staging_buffer_info = {};
        staging_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        staging_buffer_info.size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4u;
        staging_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        staging_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        err = vkCreateBuffer(device_, &staging_buffer_info, allocator_, &staging_buffer);
        if (err != VK_SUCCESS) {
            DestroyVulkanTexture(texture);
            return false;
        }

        VkMemoryRequirements staging_memory_requirements;
        vkGetBufferMemoryRequirements(device_, staging_buffer, &staging_memory_requirements);

        VkMemoryAllocateInfo staging_alloc_info = {};
        staging_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        staging_alloc_info.allocationSize = staging_memory_requirements.size;
        staging_alloc_info.memoryTypeIndex = FindMemoryType(staging_memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        err = vkAllocateMemory(device_, &staging_alloc_info, allocator_, &staging_memory);
        if (err != VK_SUCCESS) {
            vkDestroyBuffer(device_, staging_buffer, allocator_);
            DestroyVulkanTexture(texture);
            return false;
        }

        vkBindBufferMemory(device_, staging_buffer, staging_memory, 0);

        void *mapped_data = nullptr;
        err = vkMapMemory(device_, staging_memory, 0, staging_buffer_info.size, 0, &mapped_data);
        if (err != VK_SUCCESS) {
            vkFreeMemory(device_, staging_memory, allocator_);
            vkDestroyBuffer(device_, staging_buffer, allocator_);
            DestroyVulkanTexture(texture);
            return false;
        }
        memcpy(mapped_data, data, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        vkUnmapMemory(device_, staging_memory);

        VkCommandBufferAllocateInfo command_buffer_alloc_info = {};
        command_buffer_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_buffer_alloc_info.commandPool = texture_upload_command_pool_;
        command_buffer_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_buffer_alloc_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        err = vkAllocateCommandBuffers(device_, &command_buffer_alloc_info, &command_buffer);
        if (err != VK_SUCCESS) {
            vkFreeMemory(device_, staging_memory, allocator_);
            vkDestroyBuffer(device_, staging_buffer, allocator_);
            DestroyVulkanTexture(texture);
            return false;
        }

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(command_buffer, &begin_info);

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command_buffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        vkCmdCopyBufferToImage(command_buffer,
                               staging_buffer,
                               texture.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);

        vkEndCommandBuffer(command_buffer);

        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;

        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fence_info = {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(device_, &fence_info, allocator_, &fence);

        vkQueueSubmit(queue_, 1, &submit_info, fence);
        vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, fence, allocator_);

        vkFreeCommandBuffers(device_, texture_upload_command_pool_, 1, &command_buffer);
        vkFreeMemory(device_, staging_memory, allocator_);
        vkDestroyBuffer(device_, staging_buffer, allocator_);

        texture.width = width;
        texture.height = height;
        return true;
    }

    // 功能：销毁一张 Vulkan 纹理占用的全部资源（描述符集、视图、图像、显存），并复位结构体字段。
    // 思路：逐项判空后调用对应 vkDestroy*/vkFree*，最后把句柄置回 VK_NULL_HANDLE、宽高归零、ready 置 false，
    //       保证 texture 回到「未创建」状态可安全复用。
    // 参数：texture —— 目标纹理（引用，所有句柄会被复位为空句柄）。
    void DestroyVulkanTexture(LinuxVideoTexture &texture) {
        if (texture.descriptor_set != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(texture.descriptor_set);
        }
        if (texture.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, texture.view, allocator_);
        }
        if (texture.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, texture.image, allocator_);
        }
        if (texture.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, texture.memory, allocator_);
        }

        texture.image = VK_NULL_HANDLE;
        texture.view = VK_NULL_HANDLE;
        texture.memory = VK_NULL_HANDLE;
        texture.descriptor_set = VK_NULL_HANDLE;
        texture.width = 0;
        texture.height = 0;
        texture.ready = false;
    }

    // 功能：为已创建好的 Vulkan 图像视图登记 ImGui 描述符集，使其可作为纹理被 ImGui 绘制。
    // 思路：调用 ImGui_ImplVulkan_AddTexture，指定图像布局为 SHADER_READ_ONLY_OPTIMAL（只读采样），
    //       返回的 VkDescriptorSet 即 ImGui 绘制时使用的纹理句柄。
    // 参数：texture —— 已创建图像视图的纹理（引用，其 descriptor_set 会被写入）。
    void CreateVideoDescriptorSet(LinuxVideoTexture &texture) {
        texture.descriptor_set = ImGui_ImplVulkan_AddTexture(
                texture.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // 功能：向动态纹理集合的“下一张”写入新视频帧，并轮换 current/next 索引（Vulkan 版）。
    // 思路：先销毁 next 槽旧图像，创建新 Vulkan 图像并上传，登记 ImGui 描述符集；成功后切换 current/next 索引。
    // 参数：texture_set —— 目标双缓冲纹理集合（引用，会被修改）；frame —— 新视频帧数据。
    // 返回值：true 上传与轮换成功，false 创建/上传失败。
    [[nodiscard]] bool UploadDynamicTexture(LinuxDynamicTextureSet &texture_set, const RoboGo::app::VideoFrame &frame) {
        LinuxVideoTexture &next_texture = texture_set.textures[texture_set.next_index];
        if (next_texture.image != VK_NULL_HANDLE) {
            DestroyVulkanTexture(next_texture);
        }
        if (!CreateVulkanImage(frame.buffer.data(), frame.width, frame.height, next_texture)) {
            return false;
        }
        CreateVideoDescriptorSet(next_texture);
        next_texture.ready = true;
        texture_set.current_index = texture_set.next_index;
        texture_set.next_index = (texture_set.next_index + 1) % 2;
        next_texture.ready = false;
        return true;
    }

    // 功能：向静态纹理（小地图）上传一次像素数据（Vulkan 版）。
    // 思路：先销毁旧图像与视图，创建新 Vulkan 图像并上传，再登记 ImGui 描述符集。
    // 参数：texture —— 目标纹理（引用，会被更新）；data —— 源像素；width/height —— 尺寸。
    // 返回值：true 上传成功，false 创建/上传失败。
    [[nodiscard]] bool UploadStaticTexture(LinuxVideoTexture &texture,
                                           const uint8_t *data,
                                           const int width,
                                           const int height) {
        if (texture.image != VK_NULL_HANDLE) {
            DestroyVulkanTexture(texture);
        }
        if (!CreateVulkanImage(data, width, height, texture)) {
            return false;
        }
        CreateVideoDescriptorSet(texture);
        return true;
    }

    // 功能：销毁纹理组中的全部纹理（双缓冲的两张），用于后端析构或重建时释放显存。
    // 思路：遍历 textures 数组逐个调用 DestroyVulkanTexture，不重置索引（对象即将被丢弃）。
    // 参数：texture_set —— 目标双缓冲纹理集合（引用，其两张纹理均被销毁）。
    void DestroyDynamicTextureSet(LinuxDynamicTextureSet &texture_set) {
        for (auto &texture: texture_set.textures) {
            DestroyVulkanTexture(texture);
        }
    }

    // 功能：由视频纹理构建供 ImGui 使用的 TextureView（描述符集句柄 + 像素尺寸）。
    // 思路：把 ImGui 描述符集（VkDescriptorSet）转为 ImTextureID 填入纹理引用，并携带宽高。
    // 参数：texture —— 源视频纹理（含描述符集与宽高）。
    // 返回值：TextureView；若描述符集为空则返回仅含尺寸 0 的视图。
    [[nodiscard]] static RoboGo::app::TextureView BuildTextureView(const LinuxVideoTexture &texture) {
        RoboGo::app::TextureView texture_view;
        if (texture.descriptor_set != VK_NULL_HANDLE) {
            texture_view.texture_ref = ImTextureRef(reinterpret_cast<ImTextureID>(texture.descriptor_set));
        }
        texture_view.size = ImVec2(static_cast<float>(texture.width), static_cast<float>(texture.height));
        return texture_view;
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
#ifdef APP_USE_VULKAN_DEBUG_REPORT
    VkDebugReportCallbackEXT debug_report_ = VK_NULL_HANDLE;  // 调试报告回调，用于接收验证层的告警与错误
#endif
    GLFWwindow *window_ = nullptr;  // GLFW 主窗口句柄
    ImGui_ImplVulkanH_Window main_window_data_ = {};  // 交换链窗口数据，含表面、交换链、渲染通道与帧缓冲
    uint32_t min_image_count_ = 2;  // 交换链最小图像数，2 表示双缓冲
    bool swap_chain_rebuild_ = false;  // 交换链需重建标志，窗口尺寸变化或呈现失败时置位
    float main_scale_ = 1.0f;  // 主显示器 DPI 内容缩放因子，用于窗口尺寸与字号缩放
    LinuxDynamicTextureSet udp_video_textures_;  // UDP 图传视频流的双缓冲纹理
    LinuxDynamicTextureSet custom_video_textures_;  // 自定义客户端视频流的双缓冲纹理
    LinuxVideoTexture mini_map_texture_;  // 小地图静态纹理
    bool initialized_ = false;  // 后端已完成 Vulkan/ImGui 初始化并可销毁资源的标志
    bool game_input_captured_ = false;  // 游戏输入捕获状态，启用时窗口独占键鼠输入
};

}// namespace

namespace RoboGo::platform {

// 功能：平台后端工厂函数，创建 Linux 平台对应的后端实例（Linux 构建下返回 LinuxPlatformBackend）。
// 思路：返回基类 unique_ptr 以隐藏具体实现；Windows 构建由 windows_platform_backend.cpp 提供同名函数，
//       主程序只依赖本接口，从而实现跨平台同源代码。
std::unique_ptr<PlatformBackend> CreatePlatformBackend() {
    return std::make_unique<LinuxPlatformBackend>();
}

}// namespace RoboGo::platform

#endif
