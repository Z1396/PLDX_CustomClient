// windows_platform_backend.cpp —— 平台抽象层的 Windows 实现：基于 Win32 窗口 + DirectX12 设备/交换链/纹理，
// 实现纹理上传(D3D12)、窗口消息处理、游戏输入捕获切换等，对外行为与 Linux 后端保持一致。
#ifdef _WIN32

#include "platform/platform_backend.h"

#include "app/imgui_setup.h"
#include "logging/logging.h"
#include "resources.h"

// 在调试版本中开启 DX12 调试层，便于发现资源泄漏与 API 误用。
#ifdef _DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif

// 启用调试层时需要链接 dxguid.lib（用于 D3D 调试接口 GUID）。
#ifdef DX12_ENABLE_DEBUG_LAYER
#pragma comment(lib, "dxguid.lib")
#endif

// 声明 ImGui 自带的 Win32 窗口过程处理函数，供本后端在 WndProc 中转发 ImGui 相关消息。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// kFramesInFlight —— 同时“在飞行中”的帧数量（CPU 可提前提交、GPU 异步完成的帧数）。
constexpr int kFramesInFlight = 2;
// kBackBuffers —— 交换链后台缓冲数量（双缓冲）。
constexpr int kBackBuffers = 2;
// kSrvHeapSize —— SRV 描述符堆可容纳的描述符总数（供 ImGui 纹理与视频纹理共用）。
constexpr int kSrvHeapSize = 64;

// WindowsFrameContext —— 每一“在飞行中”帧的上下文：命令分配器与该帧对应的 fence 值（用于判断 GPU 是否完成该帧）。
struct WindowsFrameContext {
    ID3D12CommandAllocator *command_allocator = nullptr;
    UINT64 fence_value = 0;
};

// DescriptorHeapAllocator —— 对一段 D3D12 描述符堆做“子分配”的小型分配器：以空闲索引栈管理 CPU/GPU 描述符句柄。
struct DescriptorHeapAllocator {
    ID3D12DescriptorHeap *heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
    D3D12_CPU_DESCRIPTOR_HANDLE heap_start_cpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE heap_start_gpu = {};
    UINT heap_handle_increment = 0;
    ImVector<int> free_indices;

    // 功能：绑定一个已创建的描述符堆并完成初始化。
    // 思路：记录堆起止句柄、句柄步长，并把所有描述符下标压入空闲栈备用。
    void Create(ID3D12Device *device, ID3D12DescriptorHeap *descriptor_heap) {
        IM_ASSERT(heap == nullptr && free_indices.empty());
        heap = descriptor_heap;
        const D3D12_DESCRIPTOR_HEAP_DESC desc = descriptor_heap->GetDesc();
        heap_type = desc.Type;
        heap_start_cpu = heap->GetCPUDescriptorHandleForHeapStart();
        heap_start_gpu = heap->GetGPUDescriptorHandleForHeapStart();
        heap_handle_increment = device->GetDescriptorHandleIncrementSize(heap_type);
        free_indices.reserve(static_cast<int>(desc.NumDescriptors));
        for (int n = static_cast<int>(desc.NumDescriptors); n > 0; --n) {
            free_indices.push_back(n - 1);
        }
    }

    // 功能：清理分配器状态（仅清空指针与空闲列表，不释放堆本身）。
    void Destroy() {
        heap = nullptr;
        free_indices.clear();
    }

    // 功能：从空闲栈弹出一个描述符下标，计算出对应的 CPU/GPU 句柄返回。
    // 思路：下标乘以句柄步长再加上堆基址，得到连续排列的描述符地址。
    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_desc_handle) {
        IM_ASSERT(free_indices.Size > 0);
        const int index = free_indices.back();
        free_indices.pop_back();
        out_cpu_desc_handle->ptr = heap_start_cpu.ptr + (static_cast<SIZE_T>(index) * heap_handle_increment);
        out_gpu_desc_handle->ptr = heap_start_gpu.ptr + (static_cast<UINT64>(index) * heap_handle_increment);
    }

    // 功能：释放一个描述符，将其下标归还空闲栈。
    // 思路：用句柄地址反推下标（CPU/GPU 下标应一致），压回空闲列表供后续复用。
    void Free(const D3D12_CPU_DESCRIPTOR_HANDLE cpu_desc_handle, const D3D12_GPU_DESCRIPTOR_HANDLE gpu_desc_handle) {
        const int cpu_index = static_cast<int>((cpu_desc_handle.ptr - heap_start_cpu.ptr) / heap_handle_increment);
        const int gpu_index = static_cast<int>((gpu_desc_handle.ptr - heap_start_gpu.ptr) / heap_handle_increment);
        IM_ASSERT(cpu_index == gpu_index);
        free_indices.push_back(cpu_index);
    }
};

// WindowsVideoTexture —— 单张视频纹理：D3D12 资源、SRV 的 CPU/GPU 描述符句柄、尺寸与状态标志。
struct WindowsVideoTexture {
    ID3D12Resource *texture = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = {};
    int width = 0;
    int height = 0;
    bool ready = false;                 // 该纹理是否已上传完成、可安全采样。
    bool descriptor_allocated = false;  // 对应的 SRV 描述符是否已从该后端堆中分配。
};

// WindowsDynamicTextureSet —— 双缓冲视频纹理集合：轮换使用两张纹理避免上传与渲染相互阻塞。
struct WindowsDynamicTextureSet {
    std::array<WindowsVideoTexture, 2> textures;
    int current_index = 0;  // 当前正在被显示/采样的一张。
    int next_index = 1;     // 下一次上传写入的一张（写完成后再切换 current）。
};

// WindowsPlatformBackend —— PlatformBackend 的 Windows 实现：Win32 窗口 + DirectX12，承载所有 DX12 资源管理。
class WindowsPlatformBackend final : public RoboGo::platform::PlatformBackend {
public:
    [[nodiscard]] const char *application_name() const override {
        return "RoboGo-windows";
    }

    // 功能：初始化整个 Windows 后端（DPI、窗口、DX12 设备、ImGui 上下文、视频纹理描述符）。
    // 思路：依次开启 DPI 感知、注册窗口类、创建窗口、创建设备，再初始化 ImGui 的 Win32/DX12 后端。
    bool Initialize() override {
        // 开启进程级 DPI 感知，避免高 DPI 屏幕下界面被拉伸模糊。
        ImGui_ImplWin32_EnableDpiAwareness();
        // 获取主显示器 DPI 缩放比例，用于后续按物理像素正确初始化窗口与 ImGui。
        main_scale_ = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

        // 构造并注册 Win32 窗口类：使用 CS_CLASSDC 共享设备上下文，窗口过程指向静态转发函数 StaticWndProc。
        window_class_ = {sizeof(window_class_),
                         CS_CLASSDC,
                         StaticWndProc,
                         0L,
                         0L,
                         GetModuleHandleW(nullptr),
                         LoadIcon(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)),
                         nullptr,
                         nullptr,
                         nullptr,
                         L"RoboGo",
                         nullptr};
        if (::RegisterClassExW(&window_class_) == 0) {
            SPDLOG_CRITICAL("注册Windows窗口类失败");
            return false;
        }
        window_class_registered_ = true;

        // 以重叠窗口样式在 (100,100) 处创建 1280x800 的 RoboGo 窗口，并将 this 作为创建参数传入以便回取后端指针。
        hwnd_ = ::CreateWindowW(window_class_.lpszClassName,
                                L"RoboGo",
                                WS_OVERLAPPEDWINDOW,
                                100,
                                100,
                                1280,
                                800,
                                nullptr,
                                nullptr,
                                window_class_.hInstance,
                                this);
        if (hwnd_ == nullptr) {
            SPDLOG_CRITICAL("创建Windows窗口失败");
            CleanupWindow();
            return false;
        }

        // 创建 DirectX12 设备、交换链等核心图形资源。
        if (!CreateDeviceD3D()) {
            SPDLOG_CRITICAL("Direct3D初始化失败");
            CleanupDeviceD3D();
            CleanupWindow();
            return false;
        }
        SPDLOG_INFO("Direct3D初始化成功");

        ::ShowWindow(hwnd_, SW_SHOWDEFAULT);
        ::UpdateWindow(hwnd_);

        // 初始化本应用通用的 ImGui 样式/配置，再初始化 Win32 与 DX12 两个 ImGui 后端。
        RoboGo::app::InitializeCommonImGui(main_scale_);

        ImGui_ImplWin32_Init(hwnd_);

        // 组装 DX12 后端初始化信息：填入设备、命令队列、飞行帧数、RTV/SRV 格式以及 SRV 描述符分配回调。
        ImGui_ImplDX12_InitInfo init_info = {};
        init_info.Device = device_;
        init_info.CommandQueue = command_queue_;
        init_info.NumFramesInFlight = kFramesInFlight;
        init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init_info.SrvDescriptorHeap = srv_descriptor_heap_;
        init_info.UserData = this;
        // 提供 SRV 描述符的分配/释放回调：让 ImGui 通过后端自己的堆分配器获取/归还描述符。
        init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *info,
                                            D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_handle,
                                            D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_handle) {
            auto *backend = static_cast<WindowsPlatformBackend *>(info->UserData);
            backend->srv_descriptor_heap_allocator_.Alloc(out_cpu_handle, out_gpu_handle);
        };
        init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *info,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                           const D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
            auto *backend = static_cast<WindowsPlatformBackend *>(info->UserData);
            backend->srv_descriptor_heap_allocator_.Free(cpu_handle, gpu_handle);
        };
        ImGui_ImplDX12_Init(&init_info);

        // 加载内嵌的 ImGui 字体，并预先从 SRV 堆分配各视频纹理所需的描述符。
        RoboGo::app::LoadEmbeddedImGuiFont();
        AllocateVideoTextureDescriptors();
        initialized_ = true;
        return true;
    }

    // 功能：释放 Windows 后端所有资源（纹理、ImGui、DX12 设备、窗口）。
    // 思路：先解除输入捕获并等待 GPU 完成，再依次释放视频纹理、ImGui 上下文、DX12 设备与窗口。
    void Shutdown() override {
        if (!initialized_) {
            return;
        }

        // 退出游戏输入捕获模式（恢复鼠标），并等待所有在飞的 GPU 命令完成。
        SetGameInputCapture(false);
        WaitForPendingOperations();

        // 释放三类视频纹理集合，并归还它们的 SRV 描述符。
        ReleaseDynamicTextureSet(udp_video_textures_, true);
        ReleaseDynamicTextureSet(custom_video_textures_, true);
        ReleaseTexture(mini_map_texture_, true);

        // 关闭 ImGui 的 DX12/Win32 后端并销毁 ImGui 上下文。
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        // 释放 DX12 设备相关资源与窗口，最后复位初始化标志。
        CleanupDeviceD3D();
        CleanupWindow();
        initialized_ = false;
    }

    // 功能：非阻塞地取出并分发所有窗口消息。
    // 思路：用 PeekMessage 轮询消息队列，遇到 WM_QUIT 时标记应关闭；返回是否应继续运行主循环。
    [[nodiscard]] bool ProcessEvents() override {
        MSG message;
        while (::PeekMessage(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessage(&message);
            if (message.message == WM_QUIT) {
                should_close_ = true;
            }
        }
        return !should_close_;
    }

    // 功能：开始新的一帧，处理交换链被遮挡/窗口最小化的暂停情形。
    // 思路：若交换链仍被遮挡或窗口最小化则短暂休眠并返回 false（跳过本帧）；否则依次启动 DX12/Win32/ImGui 帧。
    [[nodiscard]] bool BeginFrame() override {
        // 用 Present(测试) 探测交换链是否仍处于被遮挡状态，或窗口是否最小化；是则休眠 10ms 并跳过本帧。
        if ((swap_chain_occluded_ && swap_chain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) || ::IsIconic(hwnd_)) {
            ::Sleep(10);
            return false;
        }
        swap_chain_occluded_ = false;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        return true;
    }

    // 功能：将本帧 ImGui 绘制数据渲染到后台缓冲并提交呈现。
    // 思路：等待可用帧上下文，把后台缓冲从 PRESENT 态转为 RENDER_TARGET 态后清屏、绘制，再转回 PRESENT 态并呈现。
    void RenderFrame(ImDrawData *draw_data) override {
        // 获取下一帧上下文（必要时等待 GPU 完成上一帧或等待交换链可呈现）。
        WindowsFrameContext *frame_context = WaitForNextFrameContext();
        const UINT back_buffer_index = swap_chain_->GetCurrentBackBufferIndex();
        frame_context->command_allocator->Reset();

        // 构建资源屏障：将当前后台缓冲从“呈现态”切换为“渲染目标态”，以允许向其绘制。
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = main_render_target_resource_[back_buffer_index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

        command_list_->Reset(frame_context->command_allocator, nullptr);
        command_list_->ResourceBarrier(1, &barrier);

        // 以不透明黑色清屏，绑定渲染目标与 SRV 描述符堆，然后由 ImGui 的 DX12 后端执行实际绘制。
        constexpr float clear_color_with_alpha[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        command_list_->ClearRenderTargetView(main_render_target_descriptor_[back_buffer_index], clear_color_with_alpha, 0, nullptr);
        command_list_->OMSetRenderTargets(1, &main_render_target_descriptor_[back_buffer_index], FALSE, nullptr);
        command_list_->SetDescriptorHeaps(1, &srv_descriptor_heap_);
        ImGui_ImplDX12_RenderDrawData(draw_data, command_list_);

        // 绘制完成后将后台缓冲从“渲染目标态”切回“呈现态”，关闭命令列表。
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        command_list_->ResourceBarrier(1, &barrier);
        command_list_->Close();

        // 提交命令列表，向 fence 发出信号记录本帧 fence 值，用于后续帧同步。
        command_queue_->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&command_list_));
        command_queue_->Signal(fence_, ++fence_last_signaled_value_);
        frame_context->fence_value = fence_last_signaled_value_;

        // 呈现后台缓冲；若返回遮挡状态则记录，下一帧 BeginFrame 会据此暂停。帧索引自增以轮换飞行帧。
        const HRESULT present_result = swap_chain_->Present(1, 0);
        swap_chain_occluded_ = (present_result == DXGI_STATUS_OCCLUDED);
        ++frame_index_;
    }

    // 功能：切换游戏输入捕获模式（将鼠标光标限制在窗口客户区内并隐藏光标）。
    // 思路：enabled 为真时把鼠标裁剪到窗口屏幕坐标矩形并隐藏光标；为假时恢复。重复设置直接返回。
    void SetGameInputCapture(const bool enabled) override {
        if (hwnd_ == nullptr || game_input_captured_ == enabled) {
            return;
        }

        if (enabled) {
            // 取窗口客户区，并将其左上/右下角换算为屏幕坐标，得到鼠标裁剪矩形。
            RECT rect;
            GetClientRect(hwnd_, &rect);
            POINT top_left = {rect.left, rect.top};
            POINT bottom_right = {rect.right, rect.bottom};
            ClientToScreen(hwnd_, &top_left);
            ClientToScreen(hwnd_, &bottom_right);
            rect.left = top_left.x;
            rect.top = top_left.y;
            rect.right = bottom_right.x;
            rect.bottom = bottom_right.y;
            // 把鼠标锁定在该矩形内并隐藏光标，避免操作手误移出窗口。
            ClipCursor(&rect);
            ShowCursor(FALSE);
            SPDLOG_DEBUG("已锁定Windows鼠标光标区域 left={} top={} right={} bottom={}",
                         rect.left,
                         rect.top,
                         rect.right,
                         rect.bottom);
        } else {
            // 解除鼠标裁剪并显示光标。
            ClipCursor(nullptr);
            ShowCursor(TRUE);
            SPDLOG_DEBUG("已解除Windows鼠标光标锁定");
        }

        game_input_captured_ = enabled;
    }

    // 功能：从视频帧上传纹理（按纹理槽分流到动态或静态纹理）。
    // 思路：UDP/自定义视频走双缓冲动态纹理，小地图走静态纹理。
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
    [[nodiscard]] bool UploadTexture(const RoboGo::app::TextureSlot slot, const RoboGo::resource::DecodedImage &image) override {
        if (slot != RoboGo::app::TextureSlot::MiniMap) {
            return false;
        }
        return UploadStaticTexture(mini_map_texture_, image.pixels.data(), image.width, image.height);
    }

    // 功能：获取指定纹理槽当前应显示的纹理视图。
    // 思路：动态纹理取 current_index 指向的一张，静态纹理直接取 mini_map_texture_。
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
    // 功能：创建并初始化 DirectX12 核心对象（设备、交换链、描述符堆、命令队列/分配器/列表、fence）。
    // 思路：先填交换链描述并按需开启调试层，再创建设备，随后依次创建 RTV/SRV 堆、命令队列与飞行帧资源。
    bool CreateDeviceD3D() {
        // 配置交换链：双缓冲、RGBA8 格式、可等待延迟对象、翻转丢弃交换效果。
        DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
        swap_chain_desc.BufferCount = kBackBuffers;
        swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc.SampleDesc.Count = 1;
        swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;

#ifdef DX12_ENABLE_DEBUG_LAYER
        // 调试层：获取调试接口并开启，用于运行时校验 D3D12 调用。
        ID3D12Debug *dx12_debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dx12_debug)))) {
            dx12_debug->EnableDebugLayer();
        }
#endif

        // 创建 D3D12 设备（使用默认适配器，特性级别 11.0）。
        if (D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)) != S_OK) {
            return false;
        }

#ifdef DX12_ENABLE_DEBUG_LAYER
        if (dx12_debug != nullptr) {
            // 让信息队列在错误/损坏/警告级别上触发断点，并屏蔽已知的 fence 零等待噪声。
            ID3D12InfoQueue *info_queue = nullptr;
            device_->QueryInterface(IID_PPV_ARGS(&info_queue));
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);

            constexpr int D3D12_MESSAGE_ID_FENCE_ZERO_WAIT_ = 1424;
            D3D12_MESSAGE_ID disabled_messages[] = {static_cast<D3D12_MESSAGE_ID>(D3D12_MESSAGE_ID_FENCE_ZERO_WAIT_)};
            D3D12_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumIDs = 1;
            filter.DenyList.pIDList = disabled_messages;
            info_queue->AddStorageFilterEntries(&filter);

            info_queue->Release();
            dx12_debug->Release();
        }
#endif

        // 创建 RTV（渲染目标视图）描述符堆，数量等于后台缓冲数。
        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
        rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_heap_desc.NumDescriptors = kBackBuffers;
        if (device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_descriptor_heap_)) != S_OK) {
            return false;
        }

        // 连续分配 kBackBuffers 个 RTV 句柄，记录到数组供每帧使用。
        const SIZE_T rtv_descriptor_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kBackBuffers; ++i) {
            main_render_target_descriptor_[i] = rtv_handle;
            rtv_handle.ptr += rtv_descriptor_size;
        }

        // 创建 SRV（着色器资源视图）描述符堆，供 ImGui 与视频纹理共用，并绑定到本后端的小型分配器。
        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
        srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_heap_desc.NumDescriptors = kSrvHeapSize;
        srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (device_->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_descriptor_heap_)) != S_OK) {
            return false;
        }
        srv_descriptor_heap_allocator_.Create(device_, srv_descriptor_heap_);

        // 创建直接命令队列。
        D3D12_COMMAND_QUEUE_DESC command_queue_desc = {};
        command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (device_->CreateCommandQueue(&command_queue_desc, IID_PPV_ARGS(&command_queue_)) != S_OK) {
            return false;
        }

        // 为每一“飞行中”帧创建独立的命令分配器。
        for (UINT i = 0; i < kFramesInFlight; ++i) {
            if (device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame_context_[i].command_allocator)) != S_OK) {
                return false;
            }
        }

        // 创建并立即关闭一个命令列表（后续每帧 Reset 复用）。
        if (device_->CreateCommandList(0,
                                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       frame_context_[0].command_allocator,
                                       nullptr,
                                       IID_PPV_ARGS(&command_list_)) != S_OK
            || command_list_->Close() != S_OK) {
            return false;
        }

        // 创建 fence 与配套的等待事件，用于 CPU/GPU 帧同步。
        if (device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)) != S_OK) {
            return false;
        }

        fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr) {
            return false;
        }

        // 创建 DXGI 工厂，检查是否支持“允许撕裂”呈现，若支持则给交换链加上对应标志。
        IDXGIFactory5 *dxgi_factory = nullptr;
        IDXGISwapChain1 *swap_chain_1 = nullptr;
        if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)) != S_OK) {
            return false;
        }

        BOOL allow_tearing = FALSE;
        dxgi_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));
        swap_chain_tearing_support_ = (allow_tearing == TRUE);
        if (swap_chain_tearing_support_) {
            swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }

        // 为 HWND 创建交换链，并查询为 IDXGISwapChain3 以获取可等待对象与索引。
        if (dxgi_factory->CreateSwapChainForHwnd(command_queue_, hwnd_, &swap_chain_desc, nullptr, nullptr, &swap_chain_1) != S_OK) {
            dxgi_factory->Release();
            return false;
        }
        if (swap_chain_1->QueryInterface(IID_PPV_ARGS(&swap_chain_)) != S_OK) {
            swap_chain_1->Release();
            dxgi_factory->Release();
            return false;
        }
        if (swap_chain_tearing_support_) {
            // 禁用 Alt+Enter 全屏切换，避免打断自定义全屏逻辑。
            dxgi_factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        }
        swap_chain_1->Release();
        dxgi_factory->Release();

        // 限制最大帧延迟为后台缓冲数，获取可等待对象，最后创建渲染目标视图。
        swap_chain_->SetMaximumFrameLatency(kBackBuffers);
        swap_chain_waitable_object_ = swap_chain_->GetFrameLatencyWaitableObject();
        CreateRenderTarget();
        return true;
    }

    // 功能：释放所有 DX12 相关资源（交换链、命令队列/列表、描述符堆、fence、设备）。
    // 思路：先清理渲染目标，再按创建逆序逐个 Release 并置空指针；调试层下额外报告存活对象以便排查泄漏。
    void CleanupDeviceD3D() {
        CleanupRenderTarget();
        if (swap_chain_ != nullptr) {
            swap_chain_->SetFullscreenState(false, nullptr);
            swap_chain_->Release();
            swap_chain_ = nullptr;
        }
        if (swap_chain_waitable_object_ != nullptr) {
            CloseHandle(swap_chain_waitable_object_);
            swap_chain_waitable_object_ = nullptr;
        }
        for (auto &context: frame_context_) {
            if (context.command_allocator != nullptr) {
                context.command_allocator->Release();
                context.command_allocator = nullptr;
            }
        }
        if (command_queue_ != nullptr) {
            command_queue_->Release();
            command_queue_ = nullptr;
        }
        if (command_list_ != nullptr) {
            command_list_->Release();
            command_list_ = nullptr;
        }
        if (rtv_descriptor_heap_ != nullptr) {
            rtv_descriptor_heap_->Release();
            rtv_descriptor_heap_ = nullptr;
        }
        if (srv_descriptor_heap_ != nullptr) {
            srv_descriptor_heap_->Release();
            srv_descriptor_heap_ = nullptr;
        }
        srv_descriptor_heap_allocator_.Destroy();
        if (fence_ != nullptr) {
            fence_->Release();
            fence_ = nullptr;
        }
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        if (device_ != nullptr) {
            device_->Release();
            device_ = nullptr;
        }

#ifdef DX12_ENABLE_DEBUG_LAYER
        // 调试层：汇总报告仍存活的 DXGI 对象，帮助发现未释放的资源。
        IDXGIDebug1 *debug = nullptr;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug)))) {
            debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
            debug->Release();
        }
#endif
    }

    // 功能：销毁窗口并注销窗口类。
    // 思路：先销毁 HWND，再在已注册时注销窗口类并复位标志。
    void CleanupWindow() {
        if (hwnd_ != nullptr) {
            ::DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (window_class_registered_) {
            ::UnregisterClassW(window_class_.lpszClassName, window_class_.hInstance);
            window_class_registered_ = false;
        }
    }

    // 功能：为交换链的每个后台缓冲创建渲染目标视图(RTV)。
    // 思路：依次取交换链缓冲，用预分配的 RTV 句柄创建 RTV 并保存资源指针。
    void CreateRenderTarget() {
        for (UINT i = 0; i < kBackBuffers; ++i) {
            ID3D12Resource *back_buffer = nullptr;
            swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffer));
            device_->CreateRenderTargetView(back_buffer, nullptr, main_render_target_descriptor_[i]);
            main_render_target_resource_[i] = back_buffer;
        }
    }

    // 功能：释放后台缓冲渲染目标资源。
    // 思路：先等待 GPU 完成，再逐个 Release 后台缓冲资源。
    void CleanupRenderTarget() {
        WaitForPendingOperations();
        for (auto &resource: main_render_target_resource_) {
            if (resource != nullptr) {
                resource->Release();
                resource = nullptr;
            }
        }
    }

    // 功能：阻塞等待所有已提交但尚未完成的 GPU 命令执行完毕。
    // 思路：向 fence 发信号、设置完成事件，再用 WaitForSingleObject 等待该事件。
    void WaitForPendingOperations() {
        if (command_queue_ == nullptr || fence_ == nullptr || fence_event_ == nullptr) {
            return;
        }
        command_queue_->Signal(fence_, ++fence_last_signaled_value_);
        fence_->SetEventOnCompletion(fence_last_signaled_value_, fence_event_);
        ::WaitForSingleObject(fence_event_, INFINITE);
    }

    // 功能：获取当前帧对应的帧上下文，并在必要时等待其上一轮 GPU 工作结束或交换链可呈现。
    // 思路：用帧索引对飞行帧数取模选取上下文；若 GPU 未完成该帧则同时等待 fence 与交换链可等待对象。
    WindowsFrameContext *WaitForNextFrameContext() {
        WindowsFrameContext *frame_context = &frame_context_[frame_index_ % kFramesInFlight];
        if (fence_->GetCompletedValue() < frame_context->fence_value) {
            fence_->SetEventOnCompletion(frame_context->fence_value, fence_event_);
            HANDLE waitable_objects[] = {swap_chain_waitable_object_, fence_event_};
            ::WaitForMultipleObjects(2, waitable_objects, TRUE, INFINITE);
        } else {
            ::WaitForSingleObject(swap_chain_waitable_object_, INFINITE);
        }
        return frame_context;
    }

    // 功能：从内存中的 RGBA 像素数据创建 D3D12 纹理并完成上传（含临时上传缓冲与一次性拷贝命令）。
    // 思路：在 DEFAULT 堆建目标纹理（初始 COPY_DEST），在 UPLOAD 堆建暂存缓冲并写入像素，再用独立命令列表拷贝并屏障转态，最后建 SRV。
    [[nodiscard]] bool LoadTextureFromMemory(const uint8_t *data,
                                             const int width,
                                             const int height,
                                             const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle,
                                             ID3D12Resource **out_texture_resource) const {
        // 目标纹理放在默认（显存）堆，初态为 COPY_DEST，等待接收上传数据。
        D3D12_HEAP_PROPERTIES props = {};
        props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        ID3D12Resource *texture = nullptr;
        HRESULT hr = device_->CreateCommittedResource(&props,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &desc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr,
                                                      IID_PPV_ARGS(&texture));
        if (FAILED(hr)) {
            return false;
        }

        // 计算按 D3D12 对齐要求的每行字节跨度(行距)与总上传大小。
        const UINT upload_pitch =
                (static_cast<UINT>(width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)
                & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        const UINT upload_size = static_cast<UINT>(height) * upload_pitch;

        // 描述上传缓冲（行主序的缓冲，放在可映射的 UPLOAD 堆，初态 GENERIC_READ）。
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = upload_size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        props.Type = D3D12_HEAP_TYPE_UPLOAD;

        ID3D12Resource *upload_buffer = nullptr;
        hr = device_->CreateCommittedResource(&props,
                                              D3D12_HEAP_FLAG_NONE,
                                              &desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr,
                                              IID_PPV_ARGS(&upload_buffer));
        if (FAILED(hr)) {
            texture->Release();
            return false;
        }

        // 映射上传缓冲，按行拷贝像素（每行按对齐后的 upload_pitch 间隔，源数据按实际行宽）。
        void *mapped = nullptr;
        D3D12_RANGE range = {0, upload_size};
        hr = upload_buffer->Map(0, &range, &mapped);
        if (FAILED(hr)) {
            upload_buffer->Release();
            texture->Release();
            return false;
        }
        auto *mapped_bytes = static_cast<uint8_t *>(mapped);
        const size_t row_bytes = static_cast<size_t>(width) * 4u;
        for (int y = 0; y < height; ++y) {
            memcpy(mapped_bytes + static_cast<size_t>(y) * upload_pitch,
                   data + static_cast<size_t>(y) * row_bytes,
                   row_bytes);
        }
        D3D12_RANGE written_range = {0, upload_size};
        upload_buffer->Unmap(0, &written_range);

        // 设置拷贝源（上传缓冲，带占位足迹描述）与目标（纹理子资源 0）。
        D3D12_TEXTURE_COPY_LOCATION src_location = {};
        src_location.pResource = upload_buffer;
        src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src_location.PlacedFootprint.Footprint.Width = width;
        src_location.PlacedFootprint.Footprint.Height = height;
        src_location.PlacedFootprint.Footprint.Depth = 1;
        src_location.PlacedFootprint.Footprint.RowPitch = upload_pitch;

        D3D12_TEXTURE_COPY_LOCATION dst_location = {};
        dst_location.pResource = texture;
        dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_location.SubresourceIndex = 0;

        // 拷贝完成后将纹理从 COPY_DEST 切换为像素着色器可采样状态。
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // 为本次一次性上传建立独立的 fence/事件/队列/分配器/命令列表（与渲染队列解耦）。
        ID3D12Fence *fence = nullptr;
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        IM_ASSERT(SUCCEEDED(hr));

        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        IM_ASSERT(event != nullptr);

        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        ID3D12CommandQueue *command_queue = nullptr;
        hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue));
        IM_ASSERT(SUCCEEDED(hr));

        ID3D12CommandAllocator *command_allocator = nullptr;
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator));
        IM_ASSERT(SUCCEEDED(hr));

        ID3D12GraphicsCommandList *command_list = nullptr;
        hr = device_->CreateCommandList(0,
                                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        command_allocator,
                                        nullptr,
                                        IID_PPV_ARGS(&command_list));
        IM_ASSERT(SUCCEEDED(hr));

        // 录制拷贝与屏障命令，提交后通过 fence 等待此次上传完成。
        command_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
        command_list->ResourceBarrier(1, &barrier);
        hr = command_list->Close();
        IM_ASSERT(SUCCEEDED(hr));

        command_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&command_list));
        hr = command_queue->Signal(fence, 1);
        IM_ASSERT(SUCCEEDED(hr));
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);

        // 释放本次上传用到的临时对象。
        command_list->Release();
        command_allocator->Release();
        command_queue->Release();
        CloseHandle(event);
        fence->Release();
        upload_buffer->Release();

        // 创建着色器资源视图(SRV)到调用方提供的 CPU 句柄，并输出纹理资源指针。
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = desc.MipLevels;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device_->CreateShaderResourceView(texture, &srv_desc, srv_cpu_handle);

        *out_texture_resource = texture;
        return true;
    }

    // 功能：为所有视频纹理（UDP、自定义、小地图）预先从 SRV 堆分配描述符。
    // 思路：在初始化时一次性分配好描述符并标记，避免运行时频繁分配。
    void AllocateVideoTextureDescriptors() {
        for (auto &texture: udp_video_textures_.textures) {
            srv_descriptor_heap_allocator_.Alloc(&texture.cpu_handle, &texture.gpu_handle);
            texture.descriptor_allocated = true;
        }
        for (auto &texture: custom_video_textures_.textures) {
            srv_descriptor_heap_allocator_.Alloc(&texture.cpu_handle, &texture.gpu_handle);
            texture.descriptor_allocated = true;
        }
        srv_descriptor_heap_allocator_.Alloc(&mini_map_texture_.cpu_handle, &mini_map_texture_.gpu_handle);
        mini_map_texture_.descriptor_allocated = true;
    }

    // 功能：向动态纹理集合的“下一张”写入新视频帧，并轮换 current/next 索引。
    // 思路：先释放旧纹理，上传新帧到 next 槽；上传成功后把 current 指向它、next 轮转，并将刚写入帧标记为非就绪。
    [[nodiscard]] bool UploadDynamicTexture(WindowsDynamicTextureSet &texture_set, const RoboGo::app::VideoFrame &frame) const {
        WindowsVideoTexture &next_texture = texture_set.textures[texture_set.next_index];
        if (next_texture.texture != nullptr) {
            next_texture.texture->Release();
            next_texture.texture = nullptr;
        }
        if (!LoadTextureFromMemory(frame.buffer.data(), frame.width, frame.height, next_texture.cpu_handle, &next_texture.texture)) {
            return false;
        }
        next_texture.width = frame.width;
        next_texture.height = frame.height;
        next_texture.ready = true;
        texture_set.current_index = texture_set.next_index;
        texture_set.next_index = (texture_set.next_index + 1) % 2;
        next_texture.ready = false;
        return true;
    }

    // 功能：向静态纹理（小地图）上传一次像素数据。
    // 思路：先释放旧纹理再上传新数据，并记录尺寸；静态纹理内容不频繁变化。
    [[nodiscard]] bool UploadStaticTexture(WindowsVideoTexture &texture,
                                           const uint8_t *data,
                                           const int width,
                                           const int height) const {
        if (texture.texture != nullptr) {
            texture.texture->Release();
            texture.texture = nullptr;
        }
        if (!LoadTextureFromMemory(data, width, height, texture.cpu_handle, &texture.texture)) {
            return false;
        }
        texture.width = width;
        texture.height = height;
        return true;
    }

    // 功能：释放动态纹理集合中的两张纹理。
    // 思路：遍历集合，按指定是否归还描述符的标记逐个释放。
    void ReleaseDynamicTextureSet(WindowsDynamicTextureSet &texture_set, const bool free_descriptor) {
        for (auto &texture: texture_set.textures) {
            ReleaseTexture(texture, free_descriptor);
        }
    }

    // 功能：释放单张视频纹理资源，必要时归还其 SRV 描述符。
    // 思路：释放 D3D 纹理、复位尺寸与就绪标志；free_descriptor 为真时把描述符退回分配器以便复用。
    void ReleaseTexture(WindowsVideoTexture &texture, const bool free_descriptor) {
        if (texture.texture != nullptr) {
            texture.texture->Release();
            texture.texture = nullptr;
        }
        if (free_descriptor && texture.descriptor_allocated) {
            srv_descriptor_heap_allocator_.Free(texture.cpu_handle, texture.gpu_handle);
            texture.descriptor_allocated = false;
        }
        texture.width = 0;
        texture.height = 0;
        texture.ready = false;
    }

    // 功能：由视频纹理构建供 ImGui 使用的 TextureView（GPU 句柄 + 像素尺寸）。
    // 思路：把 SRV 的 GPU 句柄转为 ImTextureID 填入纹理引用，并携带宽高。
    [[nodiscard]] static RoboGo::app::TextureView BuildTextureView(const WindowsVideoTexture &texture) {
        RoboGo::app::TextureView texture_view;
        if (texture.texture != nullptr) {
            texture_view.texture_ref = ImTextureRef(static_cast<ImTextureID>(texture.gpu_handle.ptr));
        }
        texture_view.size = ImVec2(static_cast<float>(texture.width), static_cast<float>(texture.height));
        return texture_view;
    }

    // 功能：窗口类注册的静态窗口过程（C 风格回调），负责把窗口绑定到后端实例并转发到成员 WndProc。
    // 思路：在 WM_NCCREATE 时把创建参数中的 this 存入窗口用户数据并记录 hwnd_；之后取出后端调用其 WndProc。
    static LRESULT WINAPI StaticWndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        if (message == WM_NCCREATE) {
            auto *create_struct = reinterpret_cast<CREATESTRUCTW *>(l_param);
            auto *backend = static_cast<WindowsPlatformBackend *>(create_struct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(backend));
            backend->hwnd_ = hwnd;
        }

        auto *backend = reinterpret_cast<WindowsPlatformBackend *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (backend != nullptr) {
            return backend->WndProc(hwnd, message, w_param, l_param);
        }
        return ::DefWindowProcW(hwnd, message, w_param, l_param);
    }

    // 功能：后端实例的窗口过程，处理与渲染/窗口生命周期相关的消息。
    // 思路：先把消息交给 ImGui 的 Win32 处理器；再处理窗口尺寸变化(重建交换链)、系统命令(屏蔽 Alt 菜单)、销毁(退出)等。
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        // 优先让 ImGui 处理输入/光标等消息（如已被处理则直接返回）。
        if (ImGui::GetCurrentContext() != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd, message, w_param, l_param)) {
            return TRUE;
        }

        switch (message) {
            case WM_SIZE:
                // 窗口大小变化且非最小化时，重建交换链缓冲与新渲染目标。
                if (device_ != nullptr && w_param != SIZE_MINIMIZED) {
                    CleanupRenderTarget();
                    DXGI_SWAP_CHAIN_DESC1 desc = {};
                    swap_chain_->GetDesc1(&desc);
                    const HRESULT result = swap_chain_->ResizeBuffers(0,
                                                                      static_cast<UINT>(LOWORD(l_param)),
                                                                      static_cast<UINT>(HIWORD(l_param)),
                                                                      desc.Format,
                                                                      desc.Flags);
                    IM_ASSERT(SUCCEEDED(result) && "Failed to resize swapchain.");
                    CreateRenderTarget();
                }
                return 0;
            case WM_SYSCOMMAND:
                // 屏蔽通过 Alt 键触发的系统菜单，避免干扰游戏操作。
                if ((w_param & 0xfff0) == SC_KEYMENU) {
                    return 0;
                }
                break;
            case WM_DESTROY:
                // 窗口被销毁时投递退出消息，主循环据此结束。
                ::PostQuitMessage(0);
                return 0;
            default:
                break;
        }

        return ::DefWindowProcW(hwnd, message, w_param, l_param);
    }

    // 私有成员：DX12 设备与每帧上下文、窗口、纹理集合及各类状态标志。
    std::array<WindowsFrameContext, kFramesInFlight> frame_context_ = {};
    UINT frame_index_ = 0;
    ID3D12Device *device_ = nullptr;
    ID3D12DescriptorHeap *rtv_descriptor_heap_ = nullptr;
    ID3D12DescriptorHeap *srv_descriptor_heap_ = nullptr;
    DescriptorHeapAllocator srv_descriptor_heap_allocator_;
    ID3D12CommandQueue *command_queue_ = nullptr;
    ID3D12GraphicsCommandList *command_list_ = nullptr;
    ID3D12Fence *fence_ = nullptr;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_last_signaled_value_ = 0;
    IDXGISwapChain3 *swap_chain_ = nullptr;
    bool swap_chain_tearing_support_ = false;
    bool swap_chain_occluded_ = false;
    HANDLE swap_chain_waitable_object_ = nullptr;
    std::array<ID3D12Resource *, kBackBuffers> main_render_target_resource_ = {};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBackBuffers> main_render_target_descriptor_ = {};
    WNDCLASSEXW window_class_ = {};
    HWND hwnd_ = nullptr;
    bool window_class_registered_ = false;
    float main_scale_ = 1.0f;
    WindowsDynamicTextureSet udp_video_textures_;
    WindowsDynamicTextureSet custom_video_textures_;
    WindowsVideoTexture mini_map_texture_;
    bool initialized_ = false;
    bool should_close_ = false;
    bool game_input_captured_ = false;
};

}// namespace

namespace RoboGo::platform {

// 功能：Windows 平台下的后端工厂实现。
// 思路：返回 WindowsPlatformBackend 实例（仅本文件内可见的具名类型经基类指针返回）。
std::unique_ptr<PlatformBackend> CreatePlatformBackend() {
    return std::make_unique<WindowsPlatformBackend>();
}

}// namespace RoboGo::platform

#endif
