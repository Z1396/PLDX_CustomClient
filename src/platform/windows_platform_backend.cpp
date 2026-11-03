// windows_platform_backend.cpp —— 平台抽象层的 Windows 实现：基于 Win32 窗口 + DirectX12 设备/交换链/纹理，
// 实现纹理上传(D3D12)、窗口消息处理、游戏输入捕获切换等，对外行为与 Linux 后端保持一致。
#ifdef _WIN32

#include "platform/platform_backend.h"  // 包含平台后端抽象接口声明（PlatformBackend 基类、TextureView、TextureSlot 等）

#include "app/imgui_setup.h"  // 包含 ImGui 通用初始化与字体加载函数声明
#include "logging/logging.h"  // 包含 spdlog 日志宏，用于输出运行期诊断信息
#include "resources.h"  // 包含资源 ID 定义（例如窗口图标 IDI_APP_ICON）

// 在调试版本中开启 DX12 调试层，便于发现资源泄漏与 API 误用。
#ifdef _DEBUG
#define DX12_ENABLE_DEBUG_LAYER  // 调试构建下定义该宏，表示开启 D3D12 调试层
#endif  // 结束 _DEBUG 条件编译块

// 启用调试层时需要链接 dxguid.lib（用于 D3D 调试接口 GUID）。
#ifdef DX12_ENABLE_DEBUG_LAYER
#pragma comment(lib, "dxguid.lib")  // 告知链接器链接 dxguid.lib（D3D12 调试接口的 GUID 符号在其中）
#endif  // 结束 DX12_ENABLE_DEBUG_LAYER 条件编译块

// 声明 ImGui 自带的 Win32 窗口过程处理函数，供本后端在 WndProc 中转发 ImGui 相关消息。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 匿名命名空间 —— 仅本编译单元可见的实现细节：常量、内部结构体（帧上下文、描述符堆分配器、视频纹理等）
// 以及具体的 WindowsPlatformBackend 类定义；不对外暴露，避免污染全局符号。
namespace {

// kFramesInFlight —— 同时“在飞行中”的帧数量（CPU 可提前提交、GPU 异步完成的帧数）。
constexpr int kFramesInFlight = 2;
// kBackBuffers —— 交换链后台缓冲数量（双缓冲）。
constexpr int kBackBuffers = 2;
// kSrvHeapSize —— SRV 描述符堆可容纳的描述符总数（供 ImGui 纹理与视频纹理共用）。
constexpr int kSrvHeapSize = 64;

// WindowsFrameContext —— 每一“在飞行中”帧的上下文：命令分配器与该帧对应的 fence 值（用于判断 GPU 是否完成该帧）。
struct WindowsFrameContext {
    ID3D12CommandAllocator *command_allocator = nullptr;  // 命令分配器，为该帧的命令列表提供内存
    UINT64 fence_value = 0;  // 该帧围栏的目标值，用于等待 GPU 完成本帧
};  // WindowsFrameContext 结构体定义结束

// DescriptorHeapAllocator —— 对一段 D3D12 描述符堆做“子分配”的小型分配器：以空闲索引栈管理 CPU/GPU 描述符句柄。
struct DescriptorHeapAllocator {
    ID3D12DescriptorHeap *heap = nullptr;  // 被管理的描述符堆本体，nullptr 表示尚未绑定
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;  // 堆类型(RTV/SRV 等)，默认值 NUM_TYPES 表示尚未确定
    D3D12_CPU_DESCRIPTOR_HANDLE heap_start_cpu = {};  // 描述符堆起始 CPU 句柄，据此按增量偏移定位各描述符
    D3D12_GPU_DESCRIPTOR_HANDLE heap_start_gpu = {};  // 描述符堆起始 GPU 句柄，供命令列表绑定与着色器索引
    UINT heap_handle_increment = 0;  // 单个描述符的句柄步长(字节)，由设备查询得到
    ImVector<int> free_indices;  // 空闲描述符索引栈，分配时弹出、释放时压回

    // 功能：绑定一个已创建的描述符堆并完成初始化。
    // 思路：记录堆起止句柄、句柄步长，并把所有描述符下标压入空闲栈备用。
    void Create(ID3D12Device *device, ID3D12DescriptorHeap *descriptor_heap) {
        IM_ASSERT(heap == nullptr && free_indices.empty());  // 断言：分配器只能初始化一次，且空闲栈必须为空
        heap = descriptor_heap;  // 记录被管理的描述符堆指针
        const D3D12_DESCRIPTOR_HEAP_DESC desc = descriptor_heap->GetDesc();  // 读取该堆的创建描述，后面要用它的类型与容量
        heap_type = desc.Type;  // 记录堆类型（RTV / CBV_SRV_UAV 等），用于查询句柄步长
        heap_start_cpu = heap->GetCPUDescriptorHandleForHeapStart();  // 缓存描述符堆起始的 CPU 侧句柄（创建视图时使用 CPU 句柄）
        heap_start_gpu = heap->GetGPUDescriptorHandleForHeapStart();  // 缓存描述符堆起始的 GPU 侧句柄（着色器可见堆才有含义）
        heap_handle_increment = device->GetDescriptorHandleIncrementSize(heap_type);  // 查询该类型堆中单个描述符的字节步长，用于按索引计算地址
        free_indices.reserve(static_cast<int>(desc.NumDescriptors));  // 预分配空闲栈容量，避免逐个入栈时反复扩容
        for (int n = static_cast<int>(desc.NumDescriptors); n > 0; --n) {  // 从最大索引倒序入栈，使得 0 号描述符最先被分配
            free_indices.push_back(n - 1);  // 把索引 n-1 压入空闲栈
        }  // 结束倒序填充循环
    }  // 结束 Create 函数

    // 功能：清理分配器状态（仅清空指针与空闲列表，不释放堆本身）。
    void Destroy() {
        heap = nullptr;  // 清空堆指针，表示分配器不再管理任何堆
        free_indices.clear();  // 清空空闲索引栈，释放其内部内存
    }  // 结束 Destroy 函数

    // 功能：从空闲栈弹出一个描述符下标，计算出对应的 CPU/GPU 句柄返回。
    // 思路：下标乘以句柄步长再加上堆基址，得到连续排列的描述符地址。
    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_desc_handle) {
        IM_ASSERT(free_indices.Size > 0);  // 断言尚有空闲描述符，堆耗尽属于调用方违约
        const int index = free_indices.back();  // 取栈顶索引作为本次分配结果
        free_indices.pop_back();  // 弹出栈顶，标记该索引已被占用
        out_cpu_desc_handle->ptr = heap_start_cpu.ptr + (static_cast<SIZE_T>(index) * heap_handle_increment);  // CPU 句柄 = 堆起始地址 + 索引 × 步长（按字节推进）
        out_gpu_desc_handle->ptr = heap_start_gpu.ptr + (static_cast<UINT64>(index) * heap_handle_increment);  // GPU 句柄同样按索引推进，供命令列表绑定使用
    }  // 结束 Alloc 函数

    // 功能：释放一个描述符，将其下标归还空闲栈。
    // 思路：用句柄地址反推下标（CPU/GPU 下标应一致），压回空闲列表供后续复用。
    void Free(const D3D12_CPU_DESCRIPTOR_HANDLE cpu_desc_handle, const D3D12_GPU_DESCRIPTOR_HANDLE gpu_desc_handle) {
        const int cpu_index = static_cast<int>((cpu_desc_handle.ptr - heap_start_cpu.ptr) / heap_handle_increment);  // 用 CPU 句柄地址反推出描述符下标
        const int gpu_index = static_cast<int>((gpu_desc_handle.ptr - heap_start_gpu.ptr) / heap_handle_increment);  // 用 GPU 句柄地址反推下标，正常情况下两者应相等
        IM_ASSERT(cpu_index == gpu_index);  // 断言两端下标一致，防止传入不匹配的句柄
        free_indices.push_back(cpu_index);  // 把下标归还空闲栈，供后续 Alloc 复用
    }  // 结束 Free 函数
};  // DescriptorHeapAllocator 结构体定义结束

// WindowsVideoTexture —— 单张视频纹理：D3D12 资源、SRV 的 CPU/GPU 描述符句柄、尺寸与状态标志。
struct WindowsVideoTexture {
    ID3D12Resource *texture = nullptr;  // 纹理资源本体，nullptr 表示尚未上传创建
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {};  // 该纹理 SRV 的 CPU 句柄，用于创建/归还视图
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = {};  // 该纹理 SRV 的 GPU 句柄，交给 ImGui 采样绘制
    int width = 0;  // 纹理像素宽度，未上传时为 0
    int height = 0;  // 纹理像素高度，未上传时为 0
    bool uploaded_once = false;         // 是否已经至少上传过一帧：决定复用前是否需要 PSR -> COPY_DEST 屏障
    bool descriptor_allocated = false;  // 对应的 SRV 描述符是否已从该后端堆中分配。
};  // WindowsVideoTexture 结构体定义结束

// WindowsDynamicTextureSet —— 双缓冲视频纹理集合：轮换使用两张纹理避免上传与渲染相互阻塞。
struct WindowsDynamicTextureSet {
    std::array<WindowsVideoTexture, 2> textures;  // 两张轮换使用的纹理，使上传与显示互不阻塞
    int current_index = 0;  // 当前正在被显示/采样的一张。
    int next_index = 1;     // 下一次上传写入的一张（写完成后再切换 current）。
};  // WindowsDynamicTextureSet 结构体定义结束

// TextureUploadContext —— 视频帧上传用的常驻资源集合。
// 为什么要常驻：原实现每帧都新建命令队列/分配器/命令列表/fence/event + 8MB 上传缓冲，
// 1080p60 下每秒要创建销毁上百个 D3D12 对象、约 475MiB 的资源分配，既卡顿又容易触发驱动抖动。
// 这里把它们做成一次性创建、按需扩容、循环复用，每帧只做 Map/拷贝/提交/等待。
struct TextureUploadContext {
    ID3D12CommandQueue *queue = nullptr;      // 专用拷贝队列（与渲染队列解耦，避免打乱主渲染节奏）
    ID3D12CommandAllocator *allocator = nullptr;  // 命令分配器
    ID3D12GraphicsCommandList *list = nullptr;    // 命令列表
    ID3D12Fence *fence = nullptr;             // 上传完成围栏
    HANDLE event = nullptr;                   // 围栏完成事件
    ID3D12Resource *upload_buffer = nullptr;  // 常驻上传缓冲（按最大帧尺寸扩容）
    UINT64 upload_capacity = 0;               // 上传缓冲当前容量（字节）
    UINT64 fence_value = 0;                   // 围栏单调递增值
};  // TextureUploadContext 结构体定义结束

// WindowsPlatformBackend —— PlatformBackend 的 Windows 实现：Win32 窗口 + DirectX12，承载所有 DX12 资源管理。
class WindowsPlatformBackend final : public RoboGo::platform::PlatformBackend {
public:  // 以下为 PlatformBackend 接口的公开实现
    // 功能：返回本后端的平台标识名称。
    // 思路：直接返回编译期字面量 "RoboGo-windows"，用于日志/调试区分当前运行平台。
    // 返回值：C 字符串形式的应用名（静态存储，生命周期安全）。
    [[nodiscard]] const char *application_name() const override {
        return "RoboGo-windows";  // 返回 Windows 平台标识字符串，供日志与调试区分运行平台
    }  // 结束 application_name 函数

    // 功能：初始化整个 Windows 后端（DPI、窗口、DX12 设备、ImGui 上下文、视频纹理描述符）。
    // 思路：依次开启 DPI 感知、注册窗口类、创建窗口、创建设备，再初始化 ImGui 的 Win32/DX12 后端。
    bool Initialize() override {
        // 开启进程级 DPI 感知，避免高 DPI 屏幕下界面被拉伸模糊。
        ImGui_ImplWin32_EnableDpiAwareness();
        // 获取主显示器 DPI 缩放比例，用于后续按物理像素正确初始化窗口与 ImGui。
        main_scale_ = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

        // 构造并注册 Win32 窗口类：使用 CS_CLASSDC 共享设备上下文，窗口过程指向静态转发函数 StaticWndProc。
        window_class_ = {sizeof(window_class_),
                         CS_CLASSDC,         // 共享设备上下文，避免每个窗口都创建独立的上下文
                         StaticWndProc,      // 窗口过程指向静态转发函数 StaticWndProc  
                         0L,                 // 额外的窗口类数据字节数，0 表示无
                         0L,                 // 额外的窗口实例数据字节数，0 表示无
                         GetModuleHandleW(nullptr),// 应用实例句柄，用于加载图标等资源
                         LoadIcon(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)),// 加载应用图标
                         nullptr, // 菜单资源句柄，0 表示无
                         nullptr, // 背景画刷句柄，0 表示使用默认
                         nullptr, // 鼠标指针资源句柄，0 表示使用默认
                         L"RoboGo", // 窗口类名，用于创建窗口时指定类名
                         nullptr   // 窗口图标资源句柄，0 表示使用默认
                        };  // 窗口类描述结构体初始化结束
        // 注册窗口类
        // 注册窗口类失败时，会返回 0，需要检查返回值是否为 0。
        if (::RegisterClassExW(&window_class_) == 0) {
            SPDLOG_CRITICAL("注册Windows窗口类失败");  // 记录窗口类注册失败
            return false;  // 注册失败，终止初始化
        }  // 结束注册窗口类的失败分支
        window_class_registered_ = true;  // 标记窗口类已注册（退出时据此注销）

        // 以重叠窗口样式在 (100,100) 处创建 1280x800 的 RoboGo 窗口，并将 this 作为创建参数传入以便回取后端指针。
        hwnd_ = ::CreateWindowW(window_class_.lpszClassName,
                                L"RoboGo",  // 窗口标题字符串
                                WS_OVERLAPPEDWINDOW,  // 标准重叠窗口样式：带标题栏、边框与最小化/最大化按钮
                                100,  // 窗口左上角 X 坐标（屏幕像素）
                                100,  // 窗口左上角 Y 坐标（屏幕像素）
                                1280,  // 窗口客户区宽度（像素）
                                800,  // 窗口客户区高度（像素）
                                nullptr,  // 父窗口句柄，nullptr 表示创建顶层窗口
                                nullptr,  // 菜单句柄，nullptr 表示不使用菜单
                                window_class_.hInstance,  // 创建窗口所需的模块实例句柄，与窗口类保持一致
                                this);  // 创建参数 lpCreateParams，WndProc 在 WM_NCCREATE 时据此取回后端指针
        if (hwnd_ == nullptr) {  // 检查窗口句柄是否创建成功
            SPDLOG_CRITICAL("创建Windows窗口失败");  // 记录窗口创建失败
            CleanupWindow();  // 清理已注册的窗口类
            return false;  // 窗口创建失败，终止初始化
        }  // 结束窗口创建失败分支

        // 创建 DirectX12 设备、交换链等核心图形资源。
        if (!CreateDeviceD3D()) {
            SPDLOG_CRITICAL("Direct3D初始化失败");  // 记录设备初始化失败
            CleanupDeviceD3D();  // 释放已创建的 D3D 资源
            CleanupWindow();  // 清理回退窗口
            return false;  // 设备初始化失败，终止
        }  // 结束 D3D 设备初始化失败分支
        SPDLOG_INFO("Direct3D初始化成功");  // 记录 D3D 初始化成功

        ::ShowWindow(hwnd_, SW_SHOWDEFAULT); // 显示窗口
        ::UpdateWindow(hwnd_); // 更新窗口状态

        // 初始化本应用通用的 ImGui 样式/配置，再初始化 Win32 与 DX12 两个 ImGui 后端。
        RoboGo::app::InitializeCommonImGui(main_scale_);

        // 初始化 Win32 后端：绑定窗口句柄、设置 DPI 缩放比例。
        ImGui_ImplWin32_Init(hwnd_);

        // 组装 DX12 后端初始化信息：填入设备、命令队列、飞行帧数、RTV/SRV 格式以及 SRV 描述符分配回调。
        ImGui_ImplDX12_InitInfo init_info = {};  // 组装 DX12 后端初始化信息结构体
        init_info.Device = device_; // DX12 设备             
        init_info.CommandQueue = command_queue_; // 命令队列
        init_info.NumFramesInFlight = kFramesInFlight; // 飞行帧数
        init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // RTV 格式
        init_info.DSVFormat = DXGI_FORMAT_UNKNOWN; // DSV 格式
        init_info.SrvDescriptorHeap = srv_descriptor_heap_; // SRV 描述符堆
        init_info.UserData = this; // 用户数据指针，用于在回调中回取后端指针
        // 提供 SRV 描述符的分配/释放回调：让 ImGui 通过后端自己的堆分配器获取/归还描述符。
        init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *info,          // 初始化信息指针
                                            D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_handle, // 输出 CPU 描述符句柄指针
                                            D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_handle) {  // 分配回调 lambda 的参数表结束，进入函数体
            auto *backend = static_cast<WindowsPlatformBackend *>(info->UserData);  // 从 UserData 取回后端实例指针
            backend->srv_descriptor_heap_allocator_.Alloc(out_cpu_handle, out_gpu_handle);  // 把分配请求转发给后端的 SRV 堆子分配器
        };  // 结束 SRV 分配回调 lambda
        // 提供 SRV 描述符的释放回调：让 ImGui 通过后端自己的堆分配器归还描述符。
        init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *info,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,  // 初始化信息指针（此处未使用，仅为满足回调签名）
                                           const D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {  // 待释放的 CPU 描述符句柄
            auto *backend = static_cast<WindowsPlatformBackend *>(info->UserData);  // 从 UserData 取回后端实例指针
            backend->srv_descriptor_heap_allocator_.Free(cpu_handle, gpu_handle);  // 把描述符归还给后端的 SRV 堆子分配器
        };  // 结束 SRV 释放回调 lambda
        // 初始化 ImGui 的 DX12 后端：绑定设备、命令队列、飞行帧数、RTV/SRV 格式以及 SRV 描述符分配/释放回调。
        ImGui_ImplDX12_Init(&init_info);

        // 加载内嵌的 ImGui 字体，并预先从 SRV 堆分配各视频纹理所需的描述符。
        RoboGo::app::LoadEmbeddedImGuiFont();
        AllocateVideoTextureDescriptors();  // 为所有视频纹理预先从 SRV 堆分配描述符
        initialized_ = true;  // 标记后端初始化完成
        return true;  // 初始化成功
    }  // 结束 Initialize 函数

    // 功能：释放 Windows 后端所有资源（纹理、ImGui、DX12 设备、窗口）。
    // 思路：先解除输入捕获并等待 GPU 完成，再依次释放视频纹理、ImGui 上下文、DX12 设备与窗口。
    void Shutdown() override {
        if (!initialized_) {  // 未初始化则没有需要释放的资源，直接返回
            return;  // 提前返回，保证 Shutdown 可重复调用
        }  // 结束未初始化判断

        // 退出游戏输入捕获模式（恢复鼠标），并等待所有在飞的 GPU 命令完成。
        SetGameInputCapture(false);
        WaitForPendingOperations();  // 阻塞等待 GPU 执行完所有已提交命令，避免释放仍在使用的资源

        // 释放三类视频纹理集合，并归还它们的 SRV 描述符。
        ReleaseDynamicTextureSet(udp_video_textures_, true);
        ReleaseDynamicTextureSet(custom_video_textures_, true);  // 释放自定义视频的双缓冲纹理并归还描述符
        ReleaseTexture(mini_map_texture_, true);  // 释放小地图静态纹理并归还其描述符
        ReleaseTextureUploadContext();  // 释放常驻的上传队列/分配器/命令列表/fence/上传缓冲

        // 关闭 ImGui 的 DX12/Win32 后端并销毁 ImGui 上下文。
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();  // 关闭 ImGui 的 Win32 后端（摘除窗口消息钩子）
        ImGui::DestroyContext();  // 销毁 ImGui 全局上下文，释放其内部内存

        // 释放 DX12 设备相关资源与窗口，最后复位初始化标志。
        CleanupDeviceD3D();  // 释放全部 D3D 资源
        CleanupWindow();  // 销毁窗口并注销窗口类
        initialized_ = false;  // 复位初始化标志，防止重复释放
    }  // 结束 Shutdown 函数

    // 功能：非阻塞地取出并分发所有窗口消息。
    // 思路：用 PeekMessage 轮询消息队列，遇到 WM_QUIT 时标记应关闭；返回是否应继续运行主循环。
    [[nodiscard]] bool ProcessEvents() override {
        // MSG:是 Windows 消息结构体，包含消息类型、发送窗口句柄、消息参数等。
        MSG message;
        /*PeekMessage 是 Windows API 中用于检查消息队列的函数。它的核心特点是非阻塞：如果队列中没有消息，它会立即返回，而不会像 GetMessage 那样让线程休眠等待消息。
        关键参数决定了它的行为：
        BOOL PeekMessageW(
            LPMSG lpMsg,
            HWND  hWnd,
            UINT  wMsgFilterMin,
            UINT  wMsgFilterMax,
            UINT  wRemoveMsg
        );
        hWnd：指定要查看哪个窗口的消息。若为 NULL，则检查当前线程所有窗口及线程消息；若为 -1，仅检查通过 PostThreadMessage 投递的线程消息。
        wRemoveMsg：控制是否将消息从队列中移除。常用 PM_REMOVE（处理并移除）或 PM_NOREMOVE（仅查看，不移除，常用于检查队列是否为空）。*/
        while (::PeekMessage(&message, nullptr, 0U, 0U, PM_REMOVE)) {  // 循环取出消息队列中的全部消息（PM_REMOVE 表示取走即删除）
            // TranslateMessage 是 Windows 消息循环中的关键函数，它的作用是将虚拟键消息（如按键按下 WM_KEYDOWN也就是键盘上的任意键）转换为字符消息（如 WM_CHAR）
            ::TranslateMessage(&message);
            // DispatchMessage 是 Windows 消息循环的最后一步，负责将消息真正“派发”给对应的窗口过程函数去处理。
            ::DispatchMessage(&message);
            // 若遇到 WM_QUIT（窗口关闭消息） 则标记应关闭，返回 false（结束主循环）。
            if (message.message == WM_QUIT) {
                should_close_ = true;  // 收到退出消息，标记应关闭
            }  // 结束 WM_QUIT 判断
        }  // 结束消息轮询循环
        return !should_close_;  // 已标记关闭则返回 false 结束主循环
    }  // 结束 ProcessEvents 函数

    // 功能：开始新的一帧，处理交换链被遮挡/窗口最小化的暂停情形。
    // 思路：若交换链仍被遮挡或窗口最小化则短暂休眠并返回 false（跳过本帧）；否则依次启动 DX12/Win32/ImGui 帧。
    [[nodiscard]] bool BeginFrame() override {
        // 用 Present(测试) 探测交换链是否仍处于被遮挡状态，或窗口是否最小化；是则休眠 10ms 并跳过本帧。
        if ((swap_chain_occluded_ && swap_chain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) || ::IsIconic(hwnd_)) {
            ::Sleep(10);  // 暂停 10ms 避免空转
            return false;  // 本帧跳过渲染
        }  // 结束“遮挡或最小化”时的暂停分支
        swap_chain_occluded_ = false;  // 取消遮挡标记，恢复正常出帧

        ImGui_ImplDX12_NewFrame();  // 启动 ImGui 的 DX12 后端帧（准备顶点/索引缓冲与描述符）
        ImGui_ImplWin32_NewFrame();  // 启动 ImGui 的 Win32 后端帧（采集鼠标键盘输入）
        ImGui::NewFrame();  // 开始新的 ImGui 逻辑帧（重置绘制列表）
        return true;  // 本帧可以正常渲染，返回 true
    }  // 结束 BeginFrame 函数

    // 功能：将本帧 ImGui 绘制数据渲染到后台缓冲并提交呈现。
    // 思路：等待可用帧上下文，把后台缓冲从 PRESENT 态转为 RENDER_TARGET 态后清屏、绘制，再转回 PRESENT 态并呈现。
    // 参数：draw_data —— 当前帧 ImGui 绘制数据，包含全部图元命令与顶点缓冲。
    void RenderFrame(ImDrawData *draw_data) override {
        // 获取下一帧上下文（必要时等待 GPU 完成上一帧或等待交换链可呈现）。
        WindowsFrameContext *frame_context = WaitForNextFrameContext();  // 获取本帧上下文（必要时等待 GPU）
        const UINT back_buffer_index = swap_chain_->GetCurrentBackBufferIndex();  // 取当前后台缓冲下标
        frame_context->command_allocator->Reset();  // 重置命令分配器，准备录制本帧命令

        // 构建资源屏障：将当前后台缓冲从“呈现态”切换为“渲染目标态”，以允许向其绘制。
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;  // 屏障类型为资源状态转换（Transition）
        barrier.Transition.pResource = main_render_target_resource_[back_buffer_index];  // 屏障作用的后台缓冲资源
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;  // 作用于该资源的全部子资源
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;  // 转换前状态：上一次呈现结束后的 PRESENT 态
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;  // 转换后状态：允许作为渲染目标写入

        command_list_->Reset(frame_context->command_allocator, nullptr);  // 重置命令列表并绑定分配器
        command_list_->ResourceBarrier(1, &barrier);  // 提交屏障：后台缓冲转入 RENDER_TARGET

        // 以不透明黑色清屏，绑定渲染目标与 SRV 描述符堆，然后由 ImGui 的 DX12 后端执行实际绘制。
        constexpr float clear_color_with_alpha[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        command_list_->ClearRenderTargetView(main_render_target_descriptor_[back_buffer_index], clear_color_with_alpha, 0, nullptr);  // 用清屏色把整个后台缓冲刷成黑色（末尾 nullptr 表示清整块，不做矩形裁剪）
        command_list_->OMSetRenderTargets(1, &main_render_target_descriptor_[back_buffer_index], FALSE, nullptr);  // 绑定后台缓冲的 RTV 作为渲染目标（FALSE 表示它不是深度缓冲）
        command_list_->SetDescriptorHeaps(1, &srv_descriptor_heap_);  // 把着色器可见的 SRV 堆绑定到命令列表，供 ImGui 采样纹理
        ImGui_ImplDX12_RenderDrawData(draw_data, command_list_);  // 由 ImGui 的 DX12 后端把绘制数据录制成实际的绘制命令

        // 绘制完成后将后台缓冲从“渲染目标态”切回“呈现态”，关闭命令列表。
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;  // 转换后状态：回到 PRESENT，以便随后提交呈现
        command_list_->ResourceBarrier(1, &barrier);  // 提交屏障：后台缓冲转回 PRESENT
        command_list_->Close();  // 关闭命令列表，准备提交

        // 提交命令列表，向 fence 发出信号记录本帧 fence 值，用于后续帧同步。
        command_queue_->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&command_list_));  // 提交命令列表供 GPU 执行
        command_queue_->Signal(fence_, ++fence_last_signaled_value_);  // 向 fence 发信号，记录本帧完成值
        frame_context->fence_value = fence_last_signaled_value_;  // 记录本帧上下文对应的 fence 值

        // 呈现后台缓冲；若返回遮挡状态则记录，下一帧 BeginFrame 会据此暂停。帧索引自增以轮换飞行帧。
        const HRESULT present_result = swap_chain_->Present(1, 0);  // 呈现后台缓冲（1=垂直同步间隔）
        swap_chain_occluded_ = (present_result == DXGI_STATUS_OCCLUDED);  // 若被遮挡则记录，下帧暂停
        ++frame_index_;  // 帧序号自增，轮换飞行帧上下文
    }  // 结束 RenderFrame 函数

    // 功能：切换游戏输入捕获模式（将鼠标光标限制在窗口客户区内并隐藏光标）。
    // 思路：enabled 为真时把鼠标裁剪到窗口屏幕坐标矩形并隐藏光标；为假时恢复。重复设置直接返回。
    // 参数：enabled —— true 锁定并隐藏鼠标（游戏操控），false 恢复普通光标。
    void SetGameInputCapture(const bool enabled) override {
        /*hwnd_ 主窗口句柄
        game_input_captured_ 游戏输入捕获状态*/
        if (hwnd_ == nullptr || game_input_captured_ == enabled) {  // 窗口未创建或状态未发生变化时无需处理
            return;  // 窗口无效或状态已一致，无需处理
        }  // 结束提前返回分支

        if (enabled) {  // 进入“开启捕获”分支
            // 取窗口客户区，并将其左上/右下角换算为屏幕坐标，得到鼠标裁剪矩形。
            RECT rect;              // 窗口客户区矩形
            GetClientRect(hwnd_, &rect); // 获取窗口客户区矩形
            POINT top_left = {rect.left, rect.top}; // 左上角屏幕坐标
            POINT bottom_right = {rect.right, rect.bottom}; // 右下角屏幕坐标
            ClientToScreen(hwnd_, &top_left); // 转换为屏幕坐标
            ClientToScreen(hwnd_, &bottom_right); // 转换为屏幕坐标
            rect.left = top_left.x; // 更新矩形左边界
            rect.top = top_left.y; // 更新矩形上边界
            rect.right = bottom_right.x; // 更新矩形右边界
            rect.bottom = bottom_right.y; // 更新矩形下边界
            ClipCursor(&rect);  // 把鼠标锁定在该矩形内并隐藏光标，避免操作手误移出窗口。
            ShowCursor(FALSE);  // 隐藏系统鼠标光标
            SPDLOG_DEBUG("已锁定Windows鼠标光标区域 left={} top={} right={} bottom={}",  // 调试日志：记录锁定后的裁剪矩形，便于排查坐标换算问题
                         rect.left,  // 日志参数：矩形左边界
                         rect.top,  // 日志参数：矩形上边界
                         rect.right,  // 日志参数：矩形右边界
                         rect.bottom);  // 日志参数：矩形下边界
        } else {  // 进入“关闭捕获”分支，恢复普通鼠标行为
            // 解除鼠标裁剪并显示光标。
            ClipCursor(nullptr);
            ShowCursor(TRUE);  // 恢复显示系统鼠标光标
            SPDLOG_DEBUG("已解除Windows鼠标光标锁定");  // 调试日志：提示已解除鼠标锁定
        }  // 结束“关闭捕获”分支
        // 更新游戏输入捕获状态
        game_input_captured_ = enabled;
    }  // 结束 SetGameInputCapture 函数

    // 功能：从视频帧上传纹理（按纹理槽分流到动态或静态纹理）。
    // 思路：UDP/自定义视频走双缓冲动态纹理，小地图走静态纹理。
    // 参数：slot —— 目标纹理槽；frame —— 视频帧像素数据（RGBA 缓冲 + 宽高）。
    // 返回值：true 上传成功，false 槽位不支持或上传失败。
    [[nodiscard]] bool UploadTexture(const RoboGo::app::TextureSlot slot, const RoboGo::app::VideoFrame &frame) override {
        switch (slot) {  // 按纹理槽类型分流到不同的上传实现
            case RoboGo::app::TextureSlot::UdpVideo:  // UDP 图传视频槽
                return UploadDynamicTexture(udp_video_textures_, frame);  // UDP 视频走双缓冲动态纹理上传
            case RoboGo::app::TextureSlot::CustomVideo:  // 自定义客户端视频槽
                return UploadDynamicTexture(custom_video_textures_, frame);  // 自定义视频同样走双缓冲动态纹理上传
            case RoboGo::app::TextureSlot::MiniMap:  // 小地图槽
                return UploadStaticTexture(mini_map_texture_, frame.buffer.data(), frame.width, frame.height);  // 小地图内容变化不频繁，走静态纹理上传
        }  // 结束 switch 分支
        return false;  // 未知槽位或不支持，返回失败
    }  // 结束 UploadTexture（视频帧重载）

    // 功能：从解码后的图像上传纹理（仅小地图槽支持该重载）。
    // 思路：仅 MiniMap 槽接受 DecodedImage，其余槽直接拒绝。
    // 参数：slot —— 目标纹理槽（仅 MiniMap）；image —— 已解码图像像素数据。
    // 返回值：true 上传成功，false 槽位不支持或上传失败。
    [[nodiscard]] bool UploadTexture(const RoboGo::app::TextureSlot slot, const RoboGo::resource::DecodedImage &image) override {
        if (slot != RoboGo::app::TextureSlot::MiniMap) {  // 只有小地图槽接受已解码的静态图像
            return false;  // 其它槽位不支持，直接返回失败
        }  // 结束槽位判断
        return UploadStaticTexture(mini_map_texture_, image.pixels.data(), image.width, image.height);  // 把解码出的像素上传为小地图静态纹理
    }  // 结束 UploadTexture（解码图像重载）

    // 功能：获取指定纹理槽当前应显示的纹理视图。
    // 思路：动态纹理取 current_index 指向的一张，静态纹理直接取 mini_map_texture_。
    // 参数：slot —— 目标纹理槽。
    // 返回值：TextureView（GPU 句柄 + 尺寸）；若无有效纹理返回空视图。
    [[nodiscard]] RoboGo::app::TextureView GetTextureView(const RoboGo::app::TextureSlot slot) const override {
        switch (slot) {  // 按槽位返回对应的纹理视图
            case RoboGo::app::TextureSlot::UdpVideo:  // UDP 视频槽
                return BuildTextureView(udp_video_textures_.textures[udp_video_textures_.current_index]);  // 取当前正在显示的那一张 UDP 纹理
            case RoboGo::app::TextureSlot::CustomVideo:  // 自定义视频槽
                return BuildTextureView(custom_video_textures_.textures[custom_video_textures_.current_index]);  // 取当前正在显示的那一张自定义视频纹理
            case RoboGo::app::TextureSlot::MiniMap:  // 小地图槽
                return BuildTextureView(mini_map_texture_);  // 小地图是静态纹理，直接取它
        }  // 结束 switch 分支
        return {};  // 未命中任何槽位，返回空视图
    }  // 结束 GetTextureView 函数

private:  // 以下为内部实现细节，不对外暴露
    // 功能：创建并初始化 DirectX12 核心对象（设备、交换链、描述符堆、命令队列/分配器/列表、fence）。
    // 思路：先填交换链描述并按需开启调试层，再创建设备，随后依次创建 RTV/SRV 堆、命令队列与飞行帧资源。
    bool CreateDeviceD3D() {
        // 配置交换链：双缓冲、RGBA8 格式、可等待延迟对象、翻转丢弃交换效果。
        DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};              // 交换链描述
        swap_chain_desc.BufferCount = kBackBuffers;              // 双缓冲
        swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;      // RGBA8 格式
        swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT; // 可等待延迟对象
        swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // 渲染目标输出
        swap_chain_desc.SampleDesc.Count = 1;                          // 1x 抗样
        swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;     // 翻转丢弃交换效果
        swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;                // 拉伸缩放

#ifdef DX12_ENABLE_DEBUG_LAYER  // 调试层专用：获取并开启 D3D12 调试接口
        // 调试层：获取调试接口并开启，用于运行时校验 D3D12 调用。
        ID3D12Debug *dx12_debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dx12_debug)))) {  // 尝试取得调试接口，系统未安装调试层时这里会失败，直接跳过
            dx12_debug->EnableDebugLayer();  // 开启调试层，之后所有 D3D12 调用都会被运行时校验
        }  // 结束调试接口获取判断
#endif  // 结束调试层获取条件编译块

        // 创建 D3D12 设备（使用默认适配器，特性级别 11.0）。
        if (D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)) != S_OK) {
            return false;  // 设备创建失败，初始化失败
        }  // 结束 D3D12 设备创建失败分支

#ifdef DX12_ENABLE_DEBUG_LAYER  // 调试层专用：配置信息队列的断点与过滤规则
        if (dx12_debug != nullptr) {  // 只有成功开启调试层时才需要配置信息队列
            // 让信息队列在错误/损坏/警告级别上触发断点，并屏蔽已知的 fence 零等待噪声。
            ID3D12InfoQueue *info_queue = nullptr;
            device_->QueryInterface(IID_PPV_ARGS(&info_queue));  // 从设备取出信息队列接口，用于设置断点与过滤规则
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);  // 错误级消息触发断点，便于第一时间定位问题
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);  // 资源损坏级消息触发断点（最严重的级别）
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);  // 警告级消息也触发断点，尽早发现潜在误用

            constexpr int D3D12_MESSAGE_ID_FENCE_ZERO_WAIT_ = 1424;  // D3D12_MESSAGE_ID_FENCE_ZERO_WAIT 的数值（当前 SDK 未导出该枚举，故用魔数）
            D3D12_MESSAGE_ID disabled_messages[] = {static_cast<D3D12_MESSAGE_ID>(D3D12_MESSAGE_ID_FENCE_ZERO_WAIT_)};  // 构造需要屏蔽的消息 ID 数组，fence 零等待是已知的无害噪声
            D3D12_INFO_QUEUE_FILTER filter = {};  // 构造信息队列过滤器，用于填写拒绝列表
            filter.DenyList.NumIDs = 1;  // 拒绝列表包含 1 条消息 ID
            filter.DenyList.pIDList = disabled_messages;  // 指向要屏蔽的消息 ID 数组
            info_queue->AddStorageFilterEntries(&filter);  // 把拒绝规则加入信息队列的存储过滤

            info_queue->Release();  // 释放信息队列接口引用
            dx12_debug->Release();  // 释放调试接口引用（设备内部仍持有自己的引用，不影响调试层生效）
        }  // 结束调试层配置块
#endif  // 结束调试层配置条件编译块

        // 创建 RTV（渲染目标视图）描述符堆，数量等于后台缓冲数。
        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
        rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;  // 堆类型为渲染目标视图（RTV）
        rtv_heap_desc.NumDescriptors = kBackBuffers;  // 描述符数量等于后台缓冲数量，每个缓冲一个 RTV
        if (device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_descriptor_heap_)) != S_OK) {  // 创建 RTV 描述符堆，失败则初始化失败
            return false;  // RTV 堆创建失败
        }  // 结束 RTV 堆创建失败分支

        // 连续分配 kBackBuffers 个 RTV 句柄，记录到数组供每帧使用。
        const SIZE_T rtv_descriptor_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();  // 取 RTV 堆的首个 CPU 句柄作为起点
        for (UINT i = 0; i < kBackBuffers; ++i) {  // 为每个后台缓冲依次分配一个 RTV 句柄
            main_render_target_descriptor_[i] = rtv_handle;  // 记录第 i 个后台缓冲对应的 RTV 句柄
            rtv_handle.ptr += rtv_descriptor_size;  // 句柄按步长前进，指向下一个描述符位置
        }  // 结束 RTV 句柄分配循环

        // 创建 SRV（着色器资源视图）描述符堆，供 ImGui 与视频纹理共用，并绑定到本后端的小型分配器。
        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
        srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;  // 堆类型为 CBV/SRV/UAV 通用描述符堆
        srv_heap_desc.NumDescriptors = kSrvHeapSize;  // 描述符容量，ImGui 与所有视频纹理共用一个池
        srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;  // 标记为着色器可见堆，否则无法被 SetDescriptorHeaps 绑定
        if (device_->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_descriptor_heap_)) != S_OK) {  // 创建 SRV 堆，失败则初始化失败
            return false;  // SRV 堆创建失败
        }  // 结束 SRV 堆创建失败分支
        srv_descriptor_heap_allocator_.Create(device_, srv_descriptor_heap_);  // 把该堆交给子分配器管理，后续按需分配描述符

        // 创建直接命令队列。
        D3D12_COMMAND_QUEUE_DESC command_queue_desc = {};
        command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;  // 命令队列类型为直接队列（可执行图形/计算/拷贝全部命令）
        if (device_->CreateCommandQueue(&command_queue_desc, IID_PPV_ARGS(&command_queue_)) != S_OK) {  // 创建命令队列，失败则初始化失败
            return false;  // 命令队列创建失败
        }  // 结束命令队列创建失败分支

        // 为每一“飞行中”帧创建独立的命令分配器。
        for (UINT i = 0; i < kFramesInFlight; ++i) {
            if (device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame_context_[i].command_allocator)) != S_OK) {  // 为第 i 个“在飞行中”帧创建独立的命令分配器
                return false;  // 某帧命令分配器创建失败
            }  // 结束单个分配器创建失败分支
        }  // 结束飞行帧分配器创建循环

        // 创建并立即关闭一个命令列表（后续每帧 Reset 复用）。
        if (device_->CreateCommandList(0,
                                       D3D12_COMMAND_LIST_TYPE_DIRECT,  // 节点掩码 0 表示单 GPU 节点
                                       frame_context_[0].command_allocator,  // 命令列表类型与队列一致（直接类型）
                                       nullptr,  // 初始绑定的命令分配器（每帧会 Reset 重新绑定）
                                        IID_PPV_ARGS(&command_list_)) != S_OK  // 初始管线状态，nullptr 表示无（由 ImGui 自行设置）
            || command_list_->Close() != S_OK) {  // 输出图形命令列表接口指针
            return false;  // 命令列表创建或关闭失败
        }  // 结束命令列表创建或关闭失败分支

        // 创建 fence 与配套的等待事件，用于 CPU/GPU 帧同步。
        if (device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)) != S_OK) {
            return false;  // fence 创建失败
        }  // 结束 fence 创建失败分支

        fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);  // 创建手动重置的等待事件（初始未触发）
        if (fence_event_ == nullptr) {  // 等待事件创建失败则无法做 GPU 同步，直接返回失败
            return false;  // 事件创建失败
        }  // 结束事件创建失败分支

        // 创建 DXGI 工厂，检查是否支持“允许撕裂”呈现，若支持则给交换链加上对应标志。
        IDXGIFactory5 *dxgi_factory = nullptr;
        IDXGISwapChain1 *swap_chain_1 = nullptr;  // 临时保存基础版本交换链，稍后升级为 IDXGISwapChain3
        if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)) != S_OK) {  // 创建 DXGI 工厂，用于查询特性并创建交换链
            return false;  // DXGI 工厂创建失败
        }  // 结束工厂创建失败分支

        BOOL allow_tearing = FALSE;  // 输出变量：是否允许撕裂呈现（不走垂直同步的低延迟模式）
        dxgi_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));  // 查询运行时是否支持 DXGI_FEATURE_PRESENT_ALLOW_TEARING
        swap_chain_tearing_support_ = (allow_tearing == TRUE);  // 缓存撕裂支持结果，供 Present 时决定使用哪种标志
        if (swap_chain_tearing_support_) {  // 支持撕裂时为交换链描述追加对应标志
            swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;  // 打开“允许撕裂”标志
        }  // 结束撕裂标志追加

        // 为 HWND 创建交换链，并查询为 IDXGISwapChain3 以获取可等待对象与索引。
        if (dxgi_factory->CreateSwapChainForHwnd(command_queue_, hwnd_, &swap_chain_desc, nullptr, nullptr, &swap_chain_1) != S_OK) {
            dxgi_factory->Release();  // 释放工厂
            return false;  // 交换链创建失败
        }  // 结束交换链创建失败分支
        if (swap_chain_1->QueryInterface(IID_PPV_ARGS(&swap_chain_)) != S_OK) {  // 升级到 IDXGISwapChain3（需要它的 GetCurrentBackBufferIndex 等接口）
            swap_chain_1->Release();  // 释放版本1交换链
            dxgi_factory->Release();  // 释放工厂
            return false;  // 升级到 IDXGISwapChain3 失败
        }  // 结束接口升级失败分支
        if (swap_chain_tearing_support_) {  // 支持撕裂时，禁用 Alt+Enter 以避免驱动接管全屏
            // 禁用 Alt+Enter 全屏切换，避免打断自定义全屏逻辑。
            dxgi_factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        }  // 结束禁用 Alt+Enter 的分支
        swap_chain_1->Release();  // 释放临时的基础交换链引用（swap_chain_ 已另持一份）
        dxgi_factory->Release();  // 释放 DXGI 工厂（交换链创建完毕后不再需要）

        // 限制最大帧延迟为后台缓冲数，获取可等待对象，最后创建渲染目标视图。
        swap_chain_->SetMaximumFrameLatency(kBackBuffers);
        swap_chain_waitable_object_ = swap_chain_->GetFrameLatencyWaitableObject();  // 取帧延迟可等待对象句柄，用于等待后台缓冲可用
        CreateRenderTarget();  // 创建后台缓冲渲染目标视图
        return true;  // D3D 核心对象全部创建成功
    }  // 结束 CreateDeviceD3D 函数

    // 功能：释放所有 DX12 相关资源（交换链、命令队列/列表、描述符堆、fence、设备）。
    // 思路：先清理渲染目标，再按创建逆序逐个 Release 并置空指针；调试层下额外报告存活对象以便排查泄漏。
    void CleanupDeviceD3D() {
        CleanupRenderTarget();  // 先释放后台缓冲的 RTV 与资源引用
        if (swap_chain_ != nullptr) {  // 交换链存在时先退出全屏再释放
            swap_chain_->SetFullscreenState(false, nullptr);  // 退出独占全屏，避免释放时状态异常
            swap_chain_->Release();  // 释放交换链引用
            swap_chain_ = nullptr;  // 置空指针，防止悬垂引用
        }  // 结束交换链释放
        if (swap_chain_waitable_object_ != nullptr) {  // 关闭交换链可等待对象句柄
            CloseHandle(swap_chain_waitable_object_);  // 关闭内核句柄
            swap_chain_waitable_object_ = nullptr;  // 置空句柄
        }  // 结束可等待对象关闭
        for (auto &context: frame_context_) {  // 遍历每个飞行帧上下文，释放其命令分配器
            if (context.command_allocator != nullptr) {  // 分配器存在才需要释放
                context.command_allocator->Release();  // 释放命令分配器
                context.command_allocator = nullptr;  // 置空，避免重复释放
            }  // 结束单个分配器释放
        }  // 结束分配器遍历循环
        if (command_queue_ != nullptr) {  // 释放命令队列
            command_queue_->Release();  // 减少引用计数
            command_queue_ = nullptr;  // 置空指针
        }  // 结束命令队列释放
        if (command_list_ != nullptr) {  // 释放图形命令列表
            command_list_->Release();  // 减少引用计数
            command_list_ = nullptr;  // 置空指针
        }  // 结束命令列表释放
        if (rtv_descriptor_heap_ != nullptr) {  // 释放 RTV 描述符堆
            rtv_descriptor_heap_->Release();  // 减少引用计数
            rtv_descriptor_heap_ = nullptr;  // 置空指针
        }  // 结束 RTV 堆释放
        if (srv_descriptor_heap_ != nullptr) {  // 释放 SRV 描述符堆
            srv_descriptor_heap_->Release();  // 减少引用计数
            srv_descriptor_heap_ = nullptr;  // 置空指针
        }  // 结束 SRV 堆释放
        srv_descriptor_heap_allocator_.Destroy();  // 清空 SRV 子分配器内部状态
        if (fence_ != nullptr) {  // 释放围栏对象
            fence_->Release();  // 减少引用计数
            fence_ = nullptr;  // 置空指针
        }  // 结束围栏释放
        if (fence_event_ != nullptr) {  // 关闭围栏等待事件句柄
            CloseHandle(fence_event_);  // 关闭内核句柄
            fence_event_ = nullptr;  // 置空句柄
        }  // 结束事件句柄关闭
        if (device_ != nullptr) {  // 最后释放 D3D12 设备
            device_->Release();  // 减少引用计数（必须晚于所有由它创建的资源）
            device_ = nullptr;  // 置空指针
        }  // 结束设备释放

#ifdef DX12_ENABLE_DEBUG_LAYER  // 调试层专用：汇总报告仍存活的 DXGI 对象
        // 调试层：汇总报告仍存活的 DXGI 对象，帮助发现未释放的资源。
        IDXGIDebug1 *debug = nullptr;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug)))) {  // 取 DXGI 调试接口（0 表示使用默认调试通道）
            debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);  // 输出存活对象摘要，存在泄漏时可在此看到
            debug->Release();  // 释放调试接口引用
        }  // 结束调试接口使用
#endif  // 结束调试层泄漏报告块
    }  // 结束 CleanupDeviceD3D 函数

    // 功能：销毁窗口并注销窗口类。
    // 思路：先销毁 HWND，再在已注册时注销窗口类并复位标志。
    void CleanupWindow() {
        if (hwnd_ != nullptr) {  // 窗口句柄存在才需要销毁
            ::DestroyWindow(hwnd_);  // 销毁窗口（会触发 WM_DESTROY 消息）
            hwnd_ = nullptr;  // 置空句柄
        }  // 结束窗口销毁
        if (window_class_registered_) {  // 窗口类已注册才需要注销
            ::UnregisterClassW(window_class_.lpszClassName, window_class_.hInstance);  // 注销窗口类，释放系统侧的类注册
            window_class_registered_ = false;  // 复位注册标志，保证下次可重新注册
        }  // 结束窗口类注销
    }  // 结束 CleanupWindow 函数

    // 功能：为交换链的每个后台缓冲创建渲染目标视图(RTV)。
    // 思路：依次取交换链缓冲，用预分配的 RTV 句柄创建 RTV 并保存资源指针。
    void CreateRenderTarget() {
        for (UINT i = 0; i < kBackBuffers; ++i) {  // 遍历每个后台缓冲，各创建一个 RTV
            ID3D12Resource *back_buffer = nullptr;  // 用于接收交换链缓冲资源指针
            swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffer));  // 取第 i 个后台缓冲（会 AddRef，一份引用由本后端持有）
            device_->CreateRenderTargetView(back_buffer, nullptr, main_render_target_descriptor_[i]);  // 以默认描述创建 RTV，写入预分配好的句柄位置
            main_render_target_resource_[i] = back_buffer;  // 保存资源指针，供每帧提交状态转换屏障时使用
        }  // 结束 RTV 创建循环
    }  // 结束 CreateRenderTarget 函数

    // 功能：释放后台缓冲渲染目标资源。
    // 思路：先等待 GPU 完成，再逐个 Release 后台缓冲资源。
    void CleanupRenderTarget() {
        WaitForPendingOperations();  // 先等 GPU 画完，避免释放仍在使用的后台缓冲
        for (auto &resource: main_render_target_resource_) {  // 遍历所有后台缓冲资源
            if (resource != nullptr) {  // 资源存在才释放
                resource->Release();  // 减少引用计数
                resource = nullptr;  // 置空，避免悬垂
            }  // 结束单个资源释放
        }  // 结束遍历
    }  // 结束 CleanupRenderTarget 函数

    // 功能：阻塞等待所有已提交但尚未完成的 GPU 命令执行完毕。
    // 思路：向 fence 发信号、设置完成事件，再用 WaitForSingleObject 等待该事件。
    // 返回值：无（当设备/围栏/事件任一为空时直接返回，避免非法等待）。
    void WaitForPendingOperations() {
        // 防御性检查：命令队列/围栏/事件任一为空时无法可靠等待，直接返回，避免对空接口调用导致崩溃。
        if (command_queue_ == nullptr || fence_ == nullptr || fence_event_ == nullptr) {
            return;  // 结束防御性判空返回
        }  // 结束 WaitForPendingOperations 函数
        // 向命令队列提交一个“信号”指令：把围栏值递增到 fence_last_signaled_value_，标记“当前已提交的命令到此为止”。
        command_queue_->Signal(fence_, ++fence_last_signaled_value_);
        // 注册完成事件：当 GPU 执行进度达到该值时，自动将 fence_event_ 置为触发态，供下方等待。
        fence_->SetEventOnCompletion(fence_last_signaled_value_, fence_event_);
        // 阻塞等待该事件被触发（INFINITE=无限期），即等到 GPU 真正执行完所有已提交命令后才返回，确保资源释放安全。
        ::WaitForSingleObject(fence_event_, INFINITE);
    }  // 结束 WaitForNextFrameContext 函数

    // 功能：获取当前帧对应的帧上下文，并在必要时等待其上一轮 GPU 工作结束或交换链可呈现。
    // 思路：用帧索引对飞行帧数取模选取上下文；若 GPU 未完成该帧则同时等待 fence 与交换链可等待对象。
    // 返回值：指向选中帧上下文的指针（其命令分配器将被本帧复用）；该指针生命周期与后端一致，调用方无需释放。
    WindowsFrameContext *WaitForNextFrameContext() {
        // 用当前帧序号 frame_index_ 对“在飞行中”的帧数 kFramesInFlight 取模，选出本帧要复用的帧上下文；
        // 双缓冲(kFramesInFlight=2)故在 0/1 之间轮换，保证 CPU 提交的第 N 帧与 GPU 正在画的第 N-1 帧互不踩踏。
        WindowsFrameContext *frame_context = &frame_context_[frame_index_ % kFramesInFlight];
        // 判断 GPU 是否已完成该上下文上一轮的工作：fence 的“已完成值” < 本帧记录的“目标 fence 值”，
        // 表示 GPU 还没画完上一轮用这个命令分配器录制的命令，必须先等它完成才能复用分配器。
        if (fence_->GetCompletedValue() < frame_context->fence_value) {
            // 向 fence 注册一个“完成事件”：当 GPU 执行进度到达本帧目标值时，自动将 fence_event_ 置为触发态。
            fence_->SetEventOnCompletion(frame_context->fence_value, fence_event_);
            // 构造一个等待对象数组，包含两个可等待句柄：① 交换链可呈现信号 swap_chain_waitable_object_，
            // ② 上面注册的 GPU 完成事件 fence_event_；二者都满足才允许继续下一帧。
            HANDLE waitable_objects[] = {swap_chain_waitable_object_, fence_event_};
            // WaitForMultipleObjects：第 1 个参数 2=数组长度；第 3 个参数 TRUE=必须等数组中“全部”对象就绪；
            // 第 4 个参数 INFINITE=无限阻塞直到都满足（保证不会在 GPU/交换链没准备好时就开始录制本帧）。
            ::WaitForMultipleObjects(2, waitable_objects, TRUE, INFINITE);
        } else {  // GPU 已完成上一轮工作，只需等待交换链给出可用缓冲
            // GPU 早已完成该上下文上一轮工作，无需等 fence；只需等交换链给出一块可呈现的后台缓冲即可开始本帧。
            ::WaitForSingleObject(swap_chain_waitable_object_, INFINITE);
        }  // 结束 else 分支
        // 返回选中的帧上下文指针（其 command_allocator 将被本帧 Reset 后重新录制命令）；
        // 该指针指向本后端内部的 frame_context_ 数组成员，生命周期与后端一致，调用方无需释放。
        return frame_context;
    }  // 返回选中的帧上下文指针

    // 功能：从内存中的 RGBA 像素数据创建 D3D12 纹理并完成上传（含临时上传缓冲与一次性拷贝命令）。
    // 思路：在 DEFAULT 堆建目标纹理（初始 COPY_DEST），在 UPLOAD 堆建暂存缓冲并写入像素，再用独立命令列表拷贝并屏障转态，最后建 SRV。
    // 参数：data —— 源像素指针（RGBA8，行宽=width*4）；width/height —— 纹理尺寸；
    //       srv_cpu_handle —— 用于创建 SRV 的 CPU 描述符句柄（由调用方从分配器取得）；
    //       out_texture_resource —— 输出参数，成功时写入新建的 D3D12 纹理资源指针。
    // 返回值：true 创建并上传成功，false 任意一步（设备/缓冲/映射）失败。
    [[nodiscard]] bool LoadTextureFromMemory(const uint8_t *data,
                                             const int width,  // 源像素数据指针（RGBA8 紧凑排列）
                                             const int height,  // 纹理宽度（像素）
                                             const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle,  // 纹理高度（像素）
                                             ID3D12Resource **out_texture_resource) const {  // 创建 SRV 的目标 CPU 描述符句柄
        // 目标纹理放在默认（显存）堆，初态为 COPY_DEST，等待接收上传数据。
        D3D12_HEAP_PROPERTIES props = {};
        props.Type = D3D12_HEAP_TYPE_DEFAULT;// 默认堆，显存

        D3D12_RESOURCE_DESC desc = {};// 目标纹理描述
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;// 2D 纹理
        desc.Width = width;  // 纹理宽度
        desc.Height = height;  // 纹理高度
        desc.DepthOrArraySize = 1;  // 2D 纹理的深度/数组层数固定为 1
        desc.MipLevels = 1;  // 只生成 1 级 Mip（不需要降采样链）
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;// RGBA8 格式
        desc.SampleDesc.Count = 1;  // 关闭多重采样，一个像素一个样本
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;// 未知布局

        ID3D12Resource *texture = nullptr;  // 用于接收创建出的目标纹理资源
        HRESULT hr = device_->CreateCommittedResource(&props,// 创建堆
                                                      D3D12_HEAP_FLAG_NONE,// 无标志
                                                      &desc,// 目标纹理描述
                                                      D3D12_RESOURCE_STATE_COPY_DEST,// 初始态 COPY_DEST
                                                      nullptr,// 无初始数据
                                                      IID_PPV_ARGS(&texture));// 创建纹理资源
        if (FAILED(hr)) {  // 纹理资源创建失败则直接返回
            return false;  // 上报失败，交由上层处理
        }  // 结束创建失败分支

        // 计算按 D3D12 对齐要求的每行字节跨度(行距)与总上传大小。
        const UINT upload_pitch =
                (static_cast<UINT>(width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)  // 行字节数按 256 字节对齐向上取整：先加上对齐值减一
                & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);  // 再用按位与清掉低位，得到对齐后的行距
        const UINT upload_size = static_cast<UINT>(height) * upload_pitch;  // 上传缓冲总大小 = 高度 × 对齐后的行距

        // 描述上传缓冲（行主序的缓冲，放在可映射的 UPLOAD 堆，初态 GENERIC_READ）。
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;// 缓冲区
        desc.Width = upload_size;// 上传缓冲区大小
        desc.Height = 1;  // 缓冲资源的“高度”固定为 1（一维缓冲）
        desc.DepthOrArraySize = 1;  // 深度/数组层数为 1
        desc.MipLevels = 1;  // Mip 层数为 1
        desc.Format = DXGI_FORMAT_UNKNOWN;  // 缓冲没有像素格式，必须填 UNKNOWN
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;  // 缓冲使用行主序布局

        props.Type = D3D12_HEAP_TYPE_UPLOAD;  // 切换到上传堆（CPU 可写、GPU 只读）

        ID3D12Resource *upload_buffer = nullptr;  // 用于接收暂存上传缓冲资源
        hr = device_->CreateCommittedResource(&props,  // 为上传缓冲创建提交式资源
                                              D3D12_HEAP_FLAG_NONE,  // 不使用特殊的堆标志
                                              &desc,  // 缓冲的资源描述
                                              D3D12_RESOURCE_STATE_GENERIC_READ,  // 上传堆资源必须处于 GENERIC_READ 初始状态
                                              nullptr,  // 不提供初始数据（随后手工拷贝像素进去）
                                              IID_PPV_ARGS(&upload_buffer));  // 输出上传缓冲接口指针
        if (FAILED(hr)) {  // 上传缓冲创建失败则回收目标纹理
            texture->Release();  // 释放刚创建的目标纹理
            return false;  // 上报失败
        }  // 结束上传缓冲失败分支

        // 映射上传缓冲，按行拷贝像素（每行按对齐后的 upload_pitch 间隔，源数据按实际行宽）。
        void *mapped = nullptr;
        D3D12_RANGE range = {0, upload_size};  // 映射范围：整个缓冲（CPU 将要访问全部内容）
        hr = upload_buffer->Map(0, &range, &mapped);  // 把上传堆映射到 CPU 虚拟地址，拿到可写指针
        if (FAILED(hr)) {  // 映射失败则回收已创建的资源
            upload_buffer->Release();  // 释放上传缓冲
            texture->Release();  // 释放目标纹理
            return false;  // 上报失败
        }  // 结束映射失败分支
        auto *mapped_bytes = static_cast<uint8_t *>(mapped);  // 以字节指针形式使用映射到的地址
        const size_t row_bytes = static_cast<size_t>(width) * 4u;  // 源数据每行实际字节数 = 宽 × 4（RGBA 每像素 4 字节）
        for (int y = 0; y < height; ++y) {  // 逐行把像素拷入上传缓冲
            memcpy(mapped_bytes + static_cast<size_t>(y) * upload_pitch,  // 目标地址按对齐行距前进（D3D12 要求每行起始 256 字节对齐）
                   data + static_cast<size_t>(y) * row_bytes,  // 源地址按实际行宽前进
                   row_bytes);  // 每行拷贝的字节数
        }  // 结束逐行拷贝循环
        D3D12_RANGE written_range = {0, upload_size};  // 告知驱动 CPU 已写入的区间，便于缓存写回管理
        upload_buffer->Unmap(0, &written_range);  // 解除映射，确保写入内容对 GPU 可见

        // 设置拷贝源（上传缓冲，带占位足迹描述）与目标（纹理子资源 0）。
        D3D12_TEXTURE_COPY_LOCATION src_location = {};
        src_location.pResource = upload_buffer;  // 拷贝的源资源为上传缓冲
        src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;  // 缓冲侧使用“放置足迹”来描述数据布局
        src_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // 足迹格式必须与目标纹理一致
        src_location.PlacedFootprint.Footprint.Width = width;  // 足迹宽度
        src_location.PlacedFootprint.Footprint.Height = height;  // 足迹高度
        src_location.PlacedFootprint.Footprint.Depth = 1;  // 足迹深度为 1（对应 2D 纹理）
        src_location.PlacedFootprint.Footprint.RowPitch = upload_pitch;  // 行距必须与写入时使用的对齐行距一致

        D3D12_TEXTURE_COPY_LOCATION dst_location = {};  // 拷贝目标位置描述
        dst_location.pResource = texture;  // 目标资源为目标纹理
        dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;  // 纹理侧使用“子资源索引”来描述
        dst_location.SubresourceIndex = 0;  // 拷贝到第 0 级 Mip 的子资源 0

        // 拷贝完成后将纹理从 COPY_DEST 切换为像素着色器可采样状态。
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;  // 资源状态转换屏障
        barrier.Transition.pResource = texture;  // 作用于目标纹理
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;  // 覆盖该纹理的全部子资源
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;  // 转换前：拷贝目标态
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;  // 转换后：像素着色器只读采样态

        // 为本次一次性上传建立独立的 fence/事件/队列/分配器/命令列表（与渲染队列解耦）。
        ID3D12Fence *fence = nullptr;
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));  // 创建围栏，初始值为 0
        IM_ASSERT(SUCCEEDED(hr));  // 断言围栏创建成功（一次性上传路径不做优雅降级）

        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);  // 创建自动重置的等待事件，初始未触发
        IM_ASSERT(event != nullptr);  // 断言事件句柄有效

        D3D12_COMMAND_QUEUE_DESC queue_desc = {};  // 命令队列描述结构（清零）
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;  // 使用直接队列类型

        ID3D12CommandQueue *command_queue = nullptr;  // 用于接收临时命令队列
        hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue));  // 创建临时队列，与渲染队列解耦以免打乱主渲染节奏
        IM_ASSERT(SUCCEEDED(hr));  // 断言队列创建成功

        ID3D12CommandAllocator *command_allocator = nullptr;  // 用于接收临时命令分配器
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator));  // 创建直接类型的命令分配器
        IM_ASSERT(SUCCEEDED(hr));  // 断言分配器创建成功

        ID3D12GraphicsCommandList *command_list = nullptr;  // 用于接收一次性命令列表
        hr = device_->CreateCommandList(0,  // 节点掩码 0
                                        D3D12_COMMAND_LIST_TYPE_DIRECT,  // 直接类型的命令列表
                                        command_allocator,  // 绑定上面创建的临时分配器
                                        nullptr,  // 不使用初始管线状态
                                        IID_PPV_ARGS(&command_list));  // 输出命令列表接口
        IM_ASSERT(SUCCEEDED(hr));  // 断言命令列表创建成功

        // 录制拷贝与屏障命令，提交后通过 fence 等待此次上传完成。
        command_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
        command_list->ResourceBarrier(1, &barrier);  // 提交屏障：把纹理转为着色器可采样状态
        hr = command_list->Close();  // 关闭命令列表，录制结束
        IM_ASSERT(SUCCEEDED(hr));  // 断言关闭成功

        command_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&command_list));  // 把命令列表提交给临时队列执行
        hr = command_queue->Signal(fence, 1);  // 在队列上提交 fence 信号值 1，表示此前命令执行完即到达
        IM_ASSERT(SUCCEEDED(hr));  // 断言信号提交成功
        fence->SetEventOnCompletion(1, event);  // 当 GPU 执行到值 1 时触发事件
        WaitForSingleObject(event, INFINITE);  // 阻塞直到上传完成，此时纹理才真正可用

        // 释放本次上传用到的临时对象。
        command_list->Release();
        command_allocator->Release();  // 释放临时命令列表
        command_queue->Release();  // 释放临时命令分配器
        CloseHandle(event);  // 释放临时命令队列
        fence->Release();  // 关闭等待事件句柄
        upload_buffer->Release();  // 释放临时围栏

        // 创建着色器资源视图(SRV)到调用方提供的 CPU 句柄，并输出纹理资源指针。
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // SRV 读取格式与纹理格式一致
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;  // 视图维度为 2D 纹理
        srv_desc.Texture2D.MipLevels = desc.MipLevels;  // 可采样的 Mip 层数
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;  // RGBA 四通道默认映射（不做通道重排）
        device_->CreateShaderResourceView(texture, &srv_desc, srv_cpu_handle);  // 在指定 CPU 句柄处创建 SRV，供 ImGui 采样

        *out_texture_resource = texture;  // 把纹理资源指针交给调用方持有
        return true;  // 创建并上传成功
    }  // 结束 LoadTextureFromMemory 函数

    // 功能：为所有视频纹理（UDP、自定义、小地图）预先从 SRV 堆分配描述符。
    // 思路：在初始化时一次性分配好描述符并标记，避免运行时频繁分配。
    void AllocateVideoTextureDescriptors() {
        for (auto &texture: udp_video_textures_.textures) {  // 为 UDP 双缓冲的两张纹理各分配一个描述符
            srv_descriptor_heap_allocator_.Alloc(&texture.cpu_handle, &texture.gpu_handle);  // 分配 CPU/GPU 句柄并写回纹理结构
            texture.descriptor_allocated = true;  // 标记已分配，释放时需要归还
        }  // 结束 UDP 描述符分配循环
        for (auto &texture: custom_video_textures_.textures) {  // 为自定义视频双缓冲的两张纹理各分配描述符
            srv_descriptor_heap_allocator_.Alloc(&texture.cpu_handle, &texture.gpu_handle);  // 分配并写回句柄
            texture.descriptor_allocated = true;  // 标记已分配
        }  // 结束自定义视频描述符分配循环
        srv_descriptor_heap_allocator_.Alloc(&mini_map_texture_.cpu_handle, &mini_map_texture_.gpu_handle);  // 为小地图纹理分配描述符
        mini_map_texture_.descriptor_allocated = true;  // 标记已分配
    }  // 结束 AllocateVideoTextureDescriptors 函数

    // 功能：确保上传上下文（队列/分配器/命令列表/fence/事件/上传缓冲）已创建，且上传缓冲容量足够。
    // 参数：required_size 本帧需要写入的字节数（行距已对齐）。
    // 返回值：true 表示可以开始上传。
    [[nodiscard]] bool EnsureTextureUploadContext(const UINT required_size) const {
        TextureUploadContext &context = texture_upload_;  // 常驻上下文（mutable，const 方法里也能更新）
        if (context.queue == nullptr) {  // 首次使用：把一次性对象都建好
            D3D12_COMMAND_QUEUE_DESC queue_desc = {};  // 队列描述
            queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;  // 直接队列（拷贝 + 屏障）
            if (FAILED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&context.queue)))) {  // 创建专用拷贝队列
                SPDLOG_ERROR("创建纹理上传队列失败");  // 记录失败
                return false;  // 交给上层丢弃该帧
            }  // 结束队列创建
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&context.allocator)))) {  // 创建分配器
                SPDLOG_ERROR("创建纹理上传分配器失败");  // 记录失败
                return false;  // 放弃
            }  // 结束分配器创建
            if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context.allocator, nullptr,
                                                  IID_PPV_ARGS(&context.list)))) {  // 创建命令列表
                SPDLOG_ERROR("创建纹理上传命令列表失败");  // 记录失败
                return false;  // 放弃
            }  // 结束命令列表创建
            context.list->Close();  // 新建的命令列表处于打开状态，必须先关闭才能 Reset
            if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context.fence)))) {  // 创建围栏
                SPDLOG_ERROR("创建纹理上传围栏失败");  // 记录失败
                return false;  // 放弃
            }  // 结束围栏创建
            context.event = CreateEvent(nullptr, FALSE, FALSE, nullptr);  // 自动重置事件
            if (context.event == nullptr) {  // 事件创建失败
                SPDLOG_ERROR("创建纹理上传事件失败");  // 记录失败
                return false;  // 放弃
            }  // 结束事件创建
        }  // 结束首次初始化

        if (context.upload_capacity >= required_size) {  // 容量够用：直接复用
            return true;  // 无需扩容
        }  // 结束容量判断

        if (context.upload_buffer != nullptr) {  // 容量不够：先释放旧缓冲
            context.upload_buffer->Release();  // 释放旧上传缓冲
            context.upload_buffer = nullptr;  // 置空
            context.upload_capacity = 0;  // 清零容量
        }  // 结束旧缓冲释放

        D3D12_HEAP_PROPERTIES properties = {};  // 堆属性
        properties.Type = D3D12_HEAP_TYPE_UPLOAD;  // 上传堆：CPU 可写、GPU 只读
        D3D12_RESOURCE_DESC description = {};  // 资源描述
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;  // 一维缓冲
        description.Width = required_size;  // 总字节数
        description.Height = 1;  // 缓冲高度固定 1
        description.DepthOrArraySize = 1;  // 深度/数组为 1
        description.MipLevels = 1;  // 单 Mip
        description.Format = DXGI_FORMAT_UNKNOWN;  // 缓冲没有像素格式
        description.SampleDesc.Count = 1;  // 缓冲必须为 1 采样
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;  // 行主序
        if (FAILED(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&context.upload_buffer)))) {  // 创建上传缓冲
            SPDLOG_ERROR("创建纹理上传缓冲失败 bytes={}", required_size);  // 记录失败与请求大小
            return false;  // 放弃
        }  // 结束上传缓冲创建
        context.upload_capacity = required_size;  // 记录新容量
        return true;  // 扩容成功
    }  // EnsureTextureUploadContext 结束

    // 功能：按帧尺寸创建（或重建）目标纹理，并创建对应的 SRV。
    // 说明：纹理只在尺寸变化时重建；尺寸不变时复用同一张，避免每帧 8MB 的显存分配。
    [[nodiscard]] bool CreateDynamicTexture(WindowsVideoTexture &texture, const int width, const int height) const {
        if (texture.texture != nullptr) {  // 已有旧纹理（尺寸变了）
            texture.texture->Release();  // 释放旧资源
            texture.texture = nullptr;  // 置空
        }  // 结束旧纹理释放

        D3D12_HEAP_PROPERTIES properties = {};  // 堆属性
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;  // 默认堆（显存）
        D3D12_RESOURCE_DESC description = {};  // 资源描述
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;  // 2D 纹理
        description.Width = static_cast<UINT64>(width);  // 宽度
        description.Height = static_cast<UINT>(height);  // 高度
        description.DepthOrArraySize = 1;  // 单层
        description.MipLevels = 1;  // 单 Mip
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // RGBA8
        description.SampleDesc.Count = 1;  // 单采样
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;  // 未知布局
        ID3D12Resource *created = nullptr;  // 新建的纹理
        if (FAILED(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&created)))) {  // 初始态为拷贝目标
            SPDLOG_ERROR("创建视频纹理失败 width={} height={}", width, height);  // 记录尺寸便于排查显存
            return false;  // 放弃
        }  // 结束纹理创建

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_description = {};  // SRV 描述
        srv_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // 与纹理格式一致
        srv_description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;  // 2D 纹理视图
        srv_description.Texture2D.MipLevels = 1;  // 单 Mip
        srv_description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;  // 默认通道映射
        device_->CreateShaderResourceView(created, &srv_description, texture.cpu_handle);  // 在已分配的句柄上创建 SRV

        texture.texture = created;  // 交给纹理结构持有
        texture.width = width;  // 记录宽度
        texture.height = height;  // 记录高度
        texture.uploaded_once = false;  // 新纹理当前处于 COPY_DEST 状态
        return true;  // 创建成功
    }  // CreateDynamicTexture 结束

    // 功能：把一帧 RGBA 像素上传到指定纹理（复用纹理与上传缓冲，只在尺寸变化时重建）。
    // 思路：尺寸校验 -> 必要时重建纹理 -> 确保上传上下文 -> 写映射内存 -> 录制「屏障 + 拷贝 + 屏障」-> 提交并等待完成。
    [[nodiscard]] bool UploadFrameToTexture(WindowsVideoTexture &texture, const RoboGo::app::VideoFrame &frame) const {
        if (frame.width <= 0 || frame.height <= 0) {  // 尺寸非法
            return false;  // 丢弃
        }  // 结束尺寸校验
        const std::size_t row_bytes = static_cast<std::size_t>(frame.width) * 4u;  // 源数据每行字节数
        if (frame.buffer.size() < row_bytes * static_cast<std::size_t>(frame.height)) {  // 缓冲不够一整帧
            SPDLOG_ERROR("视频帧缓冲长度不足 expect={} actual={}", row_bytes * static_cast<std::size_t>(frame.height),
                         frame.buffer.size());  // 记录期望与实际
            return false;  // 丢弃，避免越界读
        }  // 结束长度校验

        if (texture.texture == nullptr || texture.width != frame.width || texture.height != frame.height) {  // 首次或尺寸变化
            if (!CreateDynamicTexture(texture, frame.width, frame.height)) {  // 重建纹理（内部会释放旧的）
                return false;  // 创建失败
            }  // 结束纹理重建
        }  // 结束重建判断

        const UINT pitch = (static_cast<UINT>(frame.width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)
                           & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);  // 行距按 256 字节对齐
        const UINT upload_size = static_cast<UINT>(frame.height) * pitch;  // 本次需要写入的字节数
        if (!EnsureTextureUploadContext(upload_size)) {  // 确保上下文与上传缓冲就绪
            return false;  // 失败则丢弃本帧
        }  // 结束上下文准备

        TextureUploadContext &context = texture_upload_;  // 取常驻上下文
        D3D12_RANGE read_range = {0, 0};  // 声明 CPU 不会读这段内存（走只写快路径）
        void *mapped = nullptr;  // 映射后的 CPU 地址
        if (FAILED(context.upload_buffer->Map(0, &read_range, &mapped))) {  // 映射上传缓冲
            SPDLOG_ERROR("映射纹理上传缓冲失败");  // 记录失败
            return false;  // 丢弃
        }  // 结束映射失败分支
        auto *mapped_bytes = static_cast<uint8_t *>(mapped);  // 以字节指针使用
        for (int y = 0; y < frame.height; ++y) {  // 逐行拷贝（目标按对齐行距前进，源按实际行宽前进）
            memcpy(mapped_bytes + static_cast<std::size_t>(y) * pitch,
                   frame.buffer.data() + static_cast<std::size_t>(y) * row_bytes,
                   row_bytes);  // 拷贝一行像素
        }  // 结束逐行拷贝
        context.upload_buffer->Unmap(0, nullptr);  // 解除映射：nullptr 表示 CPU 可能写了整段

        if (FAILED(context.allocator->Reset())) {  // 复位分配器
            SPDLOG_ERROR("复位纹理上传分配器失败");  // 记录失败
            return false;  // 丢弃
        }  // 结束分配器复位
        if (FAILED(context.list->Reset(context.allocator, nullptr))) {  // 复位命令列表
            SPDLOG_ERROR("复位纹理上传命令列表失败");  // 记录失败
            return false;  // 丢弃
        }  // 结束命令列表复位

        if (texture.uploaded_once) {  // 复用中的纹理当前处于"着色器可采样"状态
            D3D12_RESOURCE_BARRIER to_copy_dest = {};  // 先转回拷贝目标
            to_copy_dest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;  // 状态转换屏障
            to_copy_dest.Transition.pResource = texture.texture;  // 目标纹理
            to_copy_dest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;  // 全部子资源
            to_copy_dest.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;  // 转换前：可采样
            to_copy_dest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;  // 转换后：拷贝目标
            context.list->ResourceBarrier(1, &to_copy_dest);  // 提交屏障
        }  // 结束复用屏障

        D3D12_TEXTURE_COPY_LOCATION source_location = {};  // 拷贝源（上传缓冲）
        source_location.pResource = context.upload_buffer;  // 源资源
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;  // 缓冲侧用放置足迹描述
        source_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // 足迹格式与纹理一致
        source_location.PlacedFootprint.Footprint.Width = static_cast<UINT>(frame.width);  // 足迹宽度
        source_location.PlacedFootprint.Footprint.Height = static_cast<UINT>(frame.height);  // 足迹高度
        source_location.PlacedFootprint.Footprint.Depth = 1;  // 深度 1
        source_location.PlacedFootprint.Footprint.RowPitch = pitch;  // 行距必须与写入时一致

        D3D12_TEXTURE_COPY_LOCATION destination_location = {};  // 拷贝目标（纹理子资源）
        destination_location.pResource = texture.texture;  // 目标纹理
        destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;  // 纹理侧用子资源索引
        destination_location.SubresourceIndex = 0;  // 第 0 级 Mip

        context.list->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location, nullptr);  // 录制拷贝

        D3D12_RESOURCE_BARRIER to_shader_resource = {};  // 拷贝完成后再转回可采样
        to_shader_resource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;  // 状态转换屏障
        to_shader_resource.Transition.pResource = texture.texture;  // 目标纹理
        to_shader_resource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;  // 全部子资源
        to_shader_resource.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;  // 转换前：拷贝目标
        to_shader_resource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;  // 转换后：可采样
        context.list->ResourceBarrier(1, &to_shader_resource);  // 提交屏障

        if (FAILED(context.list->Close())) {  // 关闭命令列表
            SPDLOG_ERROR("关闭纹理上传命令列表失败");  // 记录失败
            return false;  // 丢弃
        }  // 结束关闭失败分支

        ID3D12CommandList *command_lists[] = {context.list};  // 提交时需要的接口数组
        context.queue->ExecuteCommandLists(1, command_lists);  // 提交到专用拷贝队列
        const UINT64 signal_value = ++context.fence_value;  // 本轮围栏值（单调递增）
        if (FAILED(context.queue->Signal(context.fence, signal_value))) {  // 提交围栏信号
            SPDLOG_ERROR("纹理上传围栏信号失败");  // 记录失败
            return false;  // 丢弃
        }  // 结束信号提交
        if (FAILED(context.fence->SetEventOnCompletion(signal_value, context.event))) {  // 绑定完成事件
            SPDLOG_ERROR("纹理上传围栏事件绑定失败");  // 记录失败
            return false;  // 丢弃
        }  // 结束事件绑定
        WaitForSingleObject(context.event, INFINITE);  // 等这一帧真正拷完（下一帧要复用同一块上传缓冲）
        texture.uploaded_once = true;  // 标记纹理已进入"可采样"状态
        return true;  // 上传成功
    }  // UploadFrameToTexture 结束

    // 功能：向动态纹理集合的“下一张”写入新视频帧，并轮换 current/next 索引。
    // 思路：复用目标槽的纹理与共用的上传缓冲（只在尺寸变化时重建），上传成功后把 current 指向它、next 轮转。
    // 参数：texture_set —— 目标双缓冲纹理集合（引用，会被修改）；frame —— 新视频帧数据。
    // 返回值：true 上传与轮换成功，false 纹理创建/上传失败。
    [[nodiscard]] bool UploadDynamicTexture(WindowsDynamicTextureSet &texture_set, const RoboGo::app::VideoFrame &frame) const {
        WindowsVideoTexture &next_texture = texture_set.textures[texture_set.next_index];  // 取出下一张待写入的纹理槽
        if (!UploadFrameToTexture(next_texture, frame)) {  // 复用纹理上传本帧
            return false;  // 上传失败，保持显示上一帧
        }  // 结束上传
        texture_set.current_index = texture_set.next_index;  // 把显示索引切到刚写好的这一张
        texture_set.next_index = (texture_set.next_index + 1) % 2;  // 下一张写入槽在 0/1 之间轮转
        return true;  // 动态纹理上传与轮换成功
    }  // 结束 UploadDynamicTexture 函数

    // 功能：向静态纹理（小地图）上传一次像素数据。
    // 思路：先释放旧纹理再上传新数据，并记录尺寸；静态纹理内容不频繁变化。
    // 参数：texture —— 目标纹理（引用，会被更新）；data —— 源像素；width/height —— 尺寸。
    // 返回值：true 上传成功，false 失败。
    [[nodiscard]] bool UploadStaticTexture(WindowsVideoTexture &texture,
                                           const uint8_t *data,  // 源像素数据
                                           const int width,  // 纹理宽度
                                           const int height) const {  // 纹理高度
        if (texture.texture != nullptr) {  // 旧纹理存在则先释放
            texture.texture->Release();  // 释放旧的 D3D12 资源
            texture.texture = nullptr;  // 置空指针
        }  // 结束旧纹理释放
        if (!LoadTextureFromMemory(data, width, height, texture.cpu_handle, &texture.texture)) {  // 创建并上传新的静态纹理
            return false;  // 纹理创建/上传失败
        }  // 结束上传失败分支
        texture.width = width;  // 记录纹理宽度
        texture.height = height;  // 记录纹理高度
        return true;  // 静态纹理上传成功
    }  // 结束 UploadStaticTexture 函数

    // 功能：释放动态纹理集合中的两张纹理。
    // 思路：遍历集合，按指定是否归还描述符的标记逐个释放。
    // 功能：释放常驻的纹理上传上下文（退出时调用一次）。
    // 说明：必须在所有上传都停止之后调用，否则会释放仍在被 GPU 读取的上传缓冲。
    void ReleaseTextureUploadContext() {
        TextureUploadContext &context = texture_upload_;  // 常驻上下文
        if (context.list != nullptr) {  // 命令列表
            context.list->Release();  // 释放
            context.list = nullptr;  // 置空
        }  // 结束命令列表释放
        if (context.allocator != nullptr) {  // 命令分配器
            context.allocator->Release();  // 释放
            context.allocator = nullptr;  // 置空
        }  // 结束分配器释放
        if (context.queue != nullptr) {  // 拷贝队列
            context.queue->Release();  // 释放
            context.queue = nullptr;  // 置空
        }  // 结束队列释放
        if (context.fence != nullptr) {  // 围栏
            context.fence->Release();  // 释放
            context.fence = nullptr;  // 置空
        }  // 结束围栏释放
        if (context.event != nullptr) {  // 完成事件
            CloseHandle(context.event);  // 关闭句柄
            context.event = nullptr;  // 置空
        }  // 结束事件关闭
        if (context.upload_buffer != nullptr) {  // 上传缓冲
            context.upload_buffer->Release();  // 释放
            context.upload_buffer = nullptr;  // 置空
        }  // 结束上传缓冲释放
        context.upload_capacity = 0;  // 清零容量
        context.fence_value = 0;  // 清零围栏值
    }  // ReleaseTextureUploadContext 结束

    void ReleaseDynamicTextureSet(WindowsDynamicTextureSet &texture_set, const bool free_descriptor) {
        for (auto &texture: texture_set.textures) {  // 遍历双缓冲中的两张纹理
            ReleaseTexture(texture, free_descriptor);  // 逐张释放，是否归还描述符由参数决定
        }  // 结束遍历
    }  // 结束 ReleaseDynamicTextureSet 函数

    // 功能：释放单张视频纹理资源，必要时归还其 SRV 描述符。
    // 思路：释放 D3D 纹理、复位尺寸与就绪标志；free_descriptor 为真时把描述符退回分配器以便复用。
    void ReleaseTexture(WindowsVideoTexture &texture, const bool free_descriptor) {
        if (texture.texture != nullptr) {  // 纹理资源存在则释放
            texture.texture->Release();  // 减少 D3D12 引用计数
            texture.texture = nullptr;  // 置空指针
        }  // 结束纹理资源释放
        if (free_descriptor && texture.descriptor_allocated) {  // 需要归还且确实分配过描述符时才归还
            srv_descriptor_heap_allocator_.Free(texture.cpu_handle, texture.gpu_handle);  // 把描述符下标退回空闲栈
            texture.descriptor_allocated = false;  // 标记已归还，避免重复归还
        }  // 结束描述符归还
        texture.width = 0;  // 宽度归零，表示当前没有内容
        texture.height = 0;  // 高度归零
        texture.uploaded_once = false;  // 新纹理尚未上传过任何一帧
    }  // 结束 ReleaseTexture 函数

    // 功能：由视频纹理构建供 ImGui 使用的 TextureView（GPU 句柄 + 像素尺寸）。
    // 思路：把 SRV 的 GPU 句柄转为 ImTextureID 填入纹理引用，并携带宽高。
    // 参数：texture —— 源视频纹理（含 SRV GPU 句柄与宽高）。
    // 返回值：TextureView；若纹理为空则返回仅含尺寸 0 的视图。
    [[nodiscard]] static RoboGo::app::TextureView BuildTextureView(const WindowsVideoTexture &texture) {
        RoboGo::app::TextureView texture_view;  // 待填充的纹理视图（默认空）
        if (texture.texture != nullptr) {  // 只有资源有效时才提供可用的纹理引用
            texture_view.texture_ref = ImTextureRef(static_cast<ImTextureID>(texture.gpu_handle.ptr));  // 把 SRV 的 GPU 句柄转成 ImGui 纹理标识，ImGui 据此索引描述符采样
        }  // 结束有效性判断
        texture_view.size = ImVec2(static_cast<float>(texture.width), static_cast<float>(texture.height));  // 携带像素尺寸
        return texture_view;  // 返回供 ImGui 使用的纹理视图
    }  // 结束 BuildTextureView 函数

    // 功能：窗口类注册的静态窗口过程（C 风格回调），负责把窗口绑定到后端实例并转发到成员 WndProc。
    // 思路：在 WM_NCCREATE 时把创建参数中的 this 存入窗口用户数据并记录 hwnd_；之后取出后端调用其 WndProc。
    static LRESULT WINAPI StaticWndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        if (message == WM_NCCREATE) {  // 窗口创建消息：此时可以拿到 CREATESTRUCT 里的创建参数
            auto *create_struct = reinterpret_cast<CREATESTRUCTW *>(l_param);  // 把 lParam 解释为窗口创建结构体
            auto *backend = static_cast<WindowsPlatformBackend *>(create_struct->lpCreateParams);  // 取出 CreateWindowW 时传入的后端指针
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(backend));  // 把后端指针存进窗口用户数据，后续消息可据此回取
            backend->hwnd_ = hwnd;  // 把真实窗口句柄回填给后端
        }  // 结束 WM_NCCREATE 处理

        auto *backend = reinterpret_cast<WindowsPlatformBackend *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));  // 取回后端实例指针
        if (backend != nullptr) {  // 能取到后端实例则交给它的成员函数处理
            return backend->WndProc(hwnd, message, w_param, l_param);  // 转发给实例窗口过程
        }  // 结束转发分支
        return ::DefWindowProcW(hwnd, message, w_param, l_param);  // 无后端时走默认处理
    }  // 结束 StaticWndProc 函数

    // 功能：后端实例的窗口过程，处理与渲染/窗口生命周期相关的消息。
    // 思路：先把消息交给 ImGui 的 Win32 处理器；再处理窗口尺寸变化(重建交换链)、系统命令(屏蔽 Alt 菜单)、销毁(退出)等。
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        // 优先让 ImGui 处理输入/光标等消息（如已被处理则直接返回）。
        if (ImGui::GetCurrentContext() != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd, message, w_param, l_param)) {
            return TRUE;  // 结束“ImGui 已处理”分支
        }  // 结束 ImGui 前置处理

        switch (message) {  // 按消息类型分别处理窗口生命周期相关事件
            case WM_SIZE:  // 窗口尺寸变化消息
                // 窗口大小变化且非最小化时，重建交换链缓冲与新渲染目标。
                if (device_ != nullptr && w_param != SIZE_MINIMIZED) {
                    CleanupRenderTarget();  // 重建前先释放旧的渲染目标视图
                    DXGI_SWAP_CHAIN_DESC1 desc = {};  // 读取现有交换链描述，以便保留格式与标志
                    swap_chain_->GetDesc1(&desc);  // 查询当前交换链描述
                    const HRESULT result = swap_chain_->ResizeBuffers(0,  // ResizeBuffers：0 表示保留原有缓冲数量
                                                                      static_cast<UINT>(LOWORD(l_param)),  // 新宽度取自 lParam 的低 16 位
                                                                      static_cast<UINT>(HIWORD(l_param)),  // 新高度取自 lParam 的高 16 位
                                                                      desc.Format,  // 沿用原像素格式
                                                                      desc.Flags);  // 沿用原标志（含撕裂支持等）
                    IM_ASSERT(SUCCEEDED(result) && "Failed to resize swapchain.");  // 断言交换链重建成功
                    CreateRenderTarget();  // 重建渲染目标
                }  // 结束 WM_SIZE 处理
                return 0;  // 已处理 WM_SIZE
            case WM_SYSCOMMAND:  // 系统命令消息（例如 Alt 键唤出系统菜单）
                // 屏蔽通过 Alt 键触发的系统菜单，避免干扰游戏操作。
                if ((w_param & 0xfff0) == SC_KEYMENU) {
                    return 0;  // 屏蔽 Alt 触发的系统菜单
                }  // 结束“屏蔽系统菜单”分支
                break;  // 其它系统命令交回默认处理
            case WM_DESTROY:  // 窗口销毁消息
                // 窗口被销毁时投递退出消息，主循环据此结束。
                ::PostQuitMessage(0);  // 请求退出消息循环
                return 0;  // 已处理 WM_DESTROY
        default:  // 其它未处理的消息
            break;  // 跳出 switch，交给 DefWindowProc
        }  // 结束 switch

        return ::DefWindowProcW(hwnd, message, w_param, l_param);  // 未处理的消息交由系统默认处理
    }  // 结束 WndProc 函数

    // 私有成员：DX12 设备与每帧上下文、窗口、纹理集合及各类状态标志。
    std::array<WindowsFrameContext, kFramesInFlight> frame_context_ = {};  // 每个飞行帧独立的上下文(命令分配器+围栏值)
    UINT frame_index_ = 0;   // 单调递增的帧序号，取模后选出当前帧上下文
    ID3D12Device *device_ = nullptr;  // D3D12 设备，所有图形资源与命令对象的创建入口
    ID3D12DescriptorHeap *rtv_descriptor_heap_ = nullptr;  // 渲染目标视图(RTV)描述符堆，容纳各后台缓冲的 RTV
    ID3D12DescriptorHeap *srv_descriptor_heap_ = nullptr;  // 着色器可见的 SRV 堆，供 ImGui 与视频纹理共用
    DescriptorHeapAllocator srv_descriptor_heap_allocator_;  // SRV 堆的小型子分配器，负责描述符的申请与归还
    ID3D12CommandQueue *command_queue_ = nullptr;  // 直接命令队列，提交渲染与拷贝命令供 GPU 执行
    ID3D12GraphicsCommandList *command_list_ = nullptr;  // 图形命令列表，每帧 Reset 后重新录制绘制命令
    ID3D12Fence *fence_ = nullptr;  // CPU/GPU 同步围栏，记录已完成命令的进度值
    HANDLE fence_event_ = nullptr;  // 围栏完成事件句柄，用于阻塞等待 GPU 到达指定值
    UINT64 fence_last_signaled_value_ = 0;  // 最近一次提交给围栏的值，逐帧递增用于同步
    IDXGISwapChain3 *swap_chain_ = nullptr;  // DXGI 交换链(版本3)，管理后台缓冲与画面呈现
    bool swap_chain_tearing_support_ = false;  // 是否支持撕裂呈现，决定能否启用低延迟 Present
    bool swap_chain_occluded_ = false;  // 上一帧呈现是否被遮挡，为真时 BeginFrame 暂停出帧
    HANDLE swap_chain_waitable_object_ = nullptr;  // 交换链帧延迟可等待对象，用于等待可用的后台缓冲
    std::array<ID3D12Resource *, kBackBuffers> main_render_target_resource_ = {};  // 各后台缓冲资源，作为渲染目标与呈现源
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBackBuffers> main_render_target_descriptor_ = {};  // 各后台缓冲的 RTV 句柄，用于绑定渲染目标
    WNDCLASSEXW window_class_ = {};  // Win32 窗口类描述，注册后用于创建主窗口
    HWND hwnd_ = nullptr;  // 主窗口句柄，nullptr 表示窗口尚未创建
    bool window_class_registered_ = false;  // 窗口类是否已注册，决定退出时是否需要注销
    float main_scale_ = 1.0f;  // 主显示器 DPI 缩放比例，用于按物理像素初始化界面
    WindowsDynamicTextureSet udp_video_textures_;  // UDP 视频流的双缓冲纹理集合
    WindowsDynamicTextureSet custom_video_textures_;  // 自定义视频流的双缓冲纹理集合
    WindowsVideoTexture mini_map_texture_;  // 小地图静态纹理，内容变化不频繁
    // 常驻的纹理上传上下文：命令队列/分配器/命令列表/fence/事件/上传缓冲只创建一次，之后逐帧复用。
    // mutable 是因为上传发生在 const 成员函数里（UploadDynamicTexture 声明为 const）。
    mutable TextureUploadContext texture_upload_;
    bool initialized_ = false;  // 后端是否已完成初始化，防止重复初始化或重复释放
    bool should_close_ = false;  // 是否已收到退出请求，决定主循环是否结束
    bool game_input_captured_ = false;  // 是否处于游戏输入捕获态(锁定并隐藏鼠标光标)
};  // WindowsPlatformBackend 类定义结束

}// namespace

namespace RoboGo::platform {  // 进入平台命名空间，声明工厂函数

// 功能：Windows 平台下的后端工厂实现。
// 思路：返回 WindowsPlatformBackend 实例（仅本文件内可见的具名类型经基类指针返回）。
std::unique_ptr<PlatformBackend> CreatePlatformBackend() {
    return std::make_unique<WindowsPlatformBackend>();  // 构造 Windows 后端并以基类指针返回
}  // 结束 CreatePlatformBackend 函数

}// namespace RoboGo::platform

#endif  // 结束 _WIN32 条件编译块
