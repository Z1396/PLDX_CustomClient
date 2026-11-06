// 键鼠状态管理实现：UI 线程只负责"采样"，独立线程按固定 71Hz 向裁判系统上报 KeyboardMouseControl，
// 同时维护比赛模式、鼠标灵敏度与地图标记模式等模块级状态。
//
// 为什么要拆线程（2027 协议 2.2.1 要求键鼠最高 75Hz）：
//   原先"采样 + 打包 + 发布"全塞在渲染循环里，实际上报频率被垂直同步绑死 ——
//   60Hz 屏就是 60Hz、144Hz 屏会跑到 76.9Hz（超过协议上限）、掉帧就直接掉控制频率。
//   现在渲染循环只把按键状态与鼠标增量写进原子变量，由独立线程按固定周期取走并发送，
//   上报频率与画面刷新率彻底解耦。
#include "keyboard_mouse_state_manager.h"  // 本模块头文件：对外函数声明与依赖(ImGui/MQTT/Protobuf 消息类型)

#ifdef _WIN32
#include <timeapi.h>  // timeBeginPeriod / timeEndPeriod：把系统定时器精度提到 1ms
#pragma comment(lib, "winmm.lib")  // 链接 winmm（时间精度 API 所在库，Windows SDK 自带）
#endif

// 上报周期：14ms ≈ 71.4Hz，留出余量确保任何情况下都不越过协议 75Hz 上限。
// 说明：Windows 默认定时器精度约 15.6ms，若不调 timeBeginPeriod，sleep_until 会被拉到 64Hz 左右，
//       虽然也不违规，但会明显低于预期；启动线程时已把精度提到 1ms。
constexpr std::chrono::milliseconds kPublishPeriod(14);

// 鼠标按键在 g_state.pending_buttons 中的位定义（避免三个独立原子变量，读的时候能一次取齐）。
constexpr uint32_t kButtonLeft = 1u << 0;
constexpr uint32_t kButtonRight = 1u << 1;
constexpr uint32_t kButtonMid = 1u << 2;

namespace {

// KeyboardMouseState: 把原先散落的匿名 static 收敛到一处，便于统一查看初始化与析构顺序。
// 仅本编译单元可见(匿名命名空间)；对外 free function 签名与行为不变。
struct KeyboardMouseState {
    // 跨线程共享状态(UI 线程写，上报线程读/取走)
    std::atomic<bool> game_mode_enabled{false};
    std::atomic<int32_t> pending_mouse_x{0};
    std::atomic<int32_t> pending_mouse_y{0};
    std::atomic<int32_t> pending_mouse_z{0};
    std::atomic<uint32_t> pending_keyboard_value{0};
    std::atomic<uint32_t> pending_buttons{0};

    // 仅 UI 线程访问的状态(含地形叠加开关，地图界面共用)
    std::atomic<float> mouse_sensitivity{1.0f};
    bool marker_mode_enabled = false;
    bool commander_role_enabled = false;
    bool commander_console_active = false;
    bool marker_mode_include_sentry = true;
    std::function<void(bool enabled)> game_input_capture_handler;

    // 上报线程自身状态
    std::thread publisher_thread;
    std::atomic<bool> publisher_running{false};
    std::thread mouse_sampler_thread;
    std::atomic<bool> mouse_sampler_running{false};
    RoboGo::io::MqttClient *publisher_client = nullptr;

    // terrain overlay toggle: shared by both map views
    bool terrain_overlay_enabled = true;
};

KeyboardMouseState g_state;  // 全局唯一状态实例(匿名命名空间内，仅本编译单元可见)

}  // namespace

// 功能：查询当前是否处于比赛模式。
bool is_in_game_mode() {  // 无副作用的只读查询
    return g_state.game_mode_enabled.load(std::memory_order_acquire);  // 原子读：UI 线程与上报线程都可能访问
}

// 功能：进入比赛模式，置位开关并记录日志。
void enter_game_mode() {  // 由界面"进入比赛"按钮触发
    if (!g_state.game_mode_enabled.exchange(true, std::memory_order_acq_rel)) {  // 仅在状态真正变化时打日志，避免重复刷屏
        SPDLOG_INFO("进入比赛模式");  // spdlog 宏：格式化输出 info 级别日志到默认 logger
    }
}

// 功能：查询当前登录身份是否为指挥手。
bool is_commander_role() {
    return g_state.commander_role_enabled;  // 直接返回角色标志
}

// 功能：设置当前登录身份是否为指挥手；取消身份时顺手关掉指挥台。
void set_commander_role(bool enabled) {
    g_state.commander_role_enabled = enabled;  // 记录新的角色标志
    if (!enabled) {  // 不再是指挥手
        g_state.commander_console_active = false;  // 指挥台不能留在打开状态
    }
    SPDLOG_INFO("指挥手身份切换 enabled={}", enabled);  // 记录切换便于排查
}

// 功能：查询指挥台是否已打开。
bool is_commander_console_active() {
    return g_state.commander_console_active;  // 直接返回指挥台标志
}

// 功能：打开指挥台。
// 关键点：指挥台不控车，鼠标必须可见可点，所以强制退出比赛模式（比赛模式会捕获并隐藏鼠标）。
//
// 【重要】这里**不再校验登录身份**。原因：
//   1) 规则里根本没有"指挥手"这个编号（附录二只有 1~11，8 是飞镖），"指挥手"是队内角色；
//   2) 指挥台本质是一个界面，显示的是广播类数据（比分/全场血量/事件），任何已登录终端都能收到；
//   3) 之前的"只有 8 号能进"把界面和一个并不存在的身份绑死了 —— 如果哪天改用 7 号登录，
//      指挥台就再也打不开，而这跟数据权限毫无关系。
// 安全约束仍在：调用点只有开局面板的按钮（非比赛模式），以及"指挥手槽位登录后自动进入"。
void enter_commander_console() {
    g_state.commander_console_active = true;  // 置位指挥台标志
    g_state.game_mode_enabled.store(false, std::memory_order_release);  // 退出比赛模式：释放键鼠捕获、恢复鼠标箭头
    SPDLOG_INFO("进入指挥台（鼠标可见、不采集键鼠）");  // 记录操作
}

// 功能：关闭指挥台，回到开局面板（仍保持登录）。
void exit_commander_console() {
    g_state.commander_console_active = false;  // 清除指挥台标志
    SPDLOG_INFO("退出指挥台");  // 记录操作
}

// 功能：指挥手登录成功后自动进入指挥台（每次成功登录只自动进一次）。
// 思路：用"连接状态上升沿"触发——只在 未连接→已连接 的那一帧进指挥台。
//       这样退出指挥台后回到赛前面板不会被立刻又拽进去，用户想再进就自己点按钮。
void maybe_auto_enter_commander_console(RoboGo::io::MqttClient &mqtt_client) {
    static bool was_connected = false;  // 上一帧是否已登录
    const bool connected = mqtt_client.get_connection_state() ==
                           RoboGo::io::MqttClient::ConnectionState::Connected;  // 本帧是否已登录
    const bool just_connected = connected && !was_connected;  // 上升沿：刚刚登录成功
    was_connected = connected;  // 记录本帧状态供下一帧比较
    if (!just_connected || !g_state.commander_role_enabled || g_state.commander_console_active) {  // 非刚登录 / 非指挥手 / 已在指挥台
        return;  // 什么都不做
    }
    enter_commander_console();  // 直接铺开指挥台（内部会退出比赛模式、恢复鼠标）
}  // maybe_auto_enter_commander_console 结束

// 说明：原 leave_commander_console（离开指挥台并退出登录）已删除。
//   指挥台的出口改为"返回"：只关界面、保留连接，回上一层继续点"进入比赛"；
//   退出登录统一由开局面板「登录控制」里的按钮负责（那里本来就有）。

// 地形分层叠加开关：两个地图界面共用同一份状态（只在 UI 线程读写，无需原子）。

bool is_terrain_overlay_enabled() {
    return g_state.terrain_overlay_enabled;  // 直接返回当前状态
}

void toggle_terrain_overlay() {
    g_state.terrain_overlay_enabled = !g_state.terrain_overlay_enabled;  // 翻转
    SPDLOG_INFO("地图地形分层叠加已{}", g_state.terrain_overlay_enabled ? "开启" : "关闭");  // 留痕便于排查
}

// 功能：注册平台层的游戏输入捕获回调，进入/退出比赛模式时回调以独占输入。
// 参数 handler：回调函数(参数为是否启用捕获)。
void set_game_input_capture_handler(std::function<void(bool enabled)> handler) {  // 按值接收后移动进静态变量
    g_state.game_input_capture_handler = std::move(handler);  // 以 move 保存回调，避免拷贝
}  // set_game_input_capture_handler 结束

// 功能：获取当前鼠标灵敏度系数。
float get_mouse_sensitivity() {  // 只读访问器
    return g_state.mouse_sensitivity.load(std::memory_order_relaxed);  // 返回当前鼠标灵敏度系数
}

// 功能：更新鼠标灵敏度系数并记录日志。
// 参数 sensitivity：新的灵敏度值。
void set_mouse_sensitivity(float sensitivity) {  // 由配置界面调整灵敏度
    SPDLOG_INFO("更新鼠标灵敏度 old={} new={}", g_state.mouse_sensitivity.load(std::memory_order_relaxed), sensitivity);  // "{}" 为 spdlog 占位符，按参数顺序替换
    g_state.mouse_sensitivity.store(sensitivity, std::memory_order_relaxed);  // 更新灵敏度，采样线程下一轮生效
}

// 功能：查询当前是否处于地图点位标记模式。
bool is_marker_mode() {  // 只读访问器
    return g_state.marker_mode_enabled;  // 返回地图标记模式标志
}

// 功能：查询地图标记是否包含哨兵。
bool is_marker_mode_include_sentry() {  // 只读访问器
    return g_state.marker_mode_include_sentry;  // 返回标记是否包含哨兵标志
}  // is_marker_mode_include_sentry 结束

// 功能：设置地图标记是否包含哨兵并记录日志。
// 参数 include：true 表示群发给哨兵。
void set_marker_mode_include_sentry(bool include) {  // 由设置界面切换
    SPDLOG_INFO("更新地图标记范围 include_sentry={}", include);  // 记录本次开关变化，便于排查误发
    g_state.marker_mode_include_sentry = include;  // 更新标记范围标志
}  // set_marker_mode_include_sentry 结束

// 功能：进入地图点位标记模式并记录日志。
void enter_marker_mode() {  // 由快捷键触发
    SPDLOG_INFO("进入地图点位标记模式");  // 记录模式切换
    g_state.marker_mode_enabled = true;  // 置位标记模式，等待用户在小地图点击
}

// 功能：退出地图点位标记模式并记录日志。
void exit_marker_mode() {  // 用户取消标记时调用
    SPDLOG_INFO("退出地图点位标记模式");  // 记录模式切换
    g_state.marker_mode_enabled = false;  // 清除标记模式
}

// 功能：在地图标记模式下，向裁判系统发送一次地图点击标记指令(MapClickCmd)。
// 思路：校验标记模式 -> 填充 Protobuf 字段 -> 序列化并发布；发送后保持标记模式，由操作手手动退出。
static void PublishMapClick(RoboGo::io::MqttClient &mqtt_client, float map_x, float map_y, bool include_sentry) {
    MapClickCmd map_click_cmd;  // 构造地图点击指令 Protobuf 消息
    map_click_cmd.set_is_send_all(include_sentry ? 2 : 1);  // 2 表示群发给哨兵，1 表示仅普通机器人
    std::string robot_id(7, 0x00);  // 固定 7 字节 robot_id，先以全 0 占位
    map_click_cmd.set_robot_id(robot_id);  // 设置目标机器人 ID(此处为占位)
    map_click_cmd.set_mode(1);  // 设置标记类型(1=攻击)
    map_click_cmd.set_enemy_id(0);  // 敌方 ID(0 表示不适用)
    map_click_cmd.set_ascii(0);  // ASCII 字段(0 表示不适用)
    map_click_cmd.set_type(1);  // 标记模式(1=地图)
    map_click_cmd.set_map_x(map_x);  // 设置地图 X 坐标
    map_click_cmd.set_map_y(map_y);  // 设置地图 Y 坐标

    std::string serialized;  // 准备接收序列化字节串
    if (map_click_cmd.SerializeToString(&serialized)) {  // Protobuf 序列化：把消息编码为紧凑二进制写入字符串
        SPDLOG_INFO("发送地图标记 map_x={} map_y={} include_sentry={}",  // 记录发送内容，便于复盘对账
                    map_x,  // 占位符 1：地图 X 坐标
                    map_y,  // 占位符 2：地图 Y 坐标
                    include_sentry);  // 占位符 3：是否包含哨兵
        mqtt_client.publish("MapClickCmd", serialized);  // 发布到 MapClickCmd 主题
    } else {  // 序列化失败(通常表示必填字段缺失或内存不足)
        SPDLOG_ERROR("地图标记序列化失败 map_x={} map_y={}", map_x, map_y);  // 记录失败坐标，便于复现
    }
}

// 功能：标记模式下发送一次地图标记（保留"必须先进入标记模式"的原有约束）。
void send_map_marker(RoboGo::io::MqttClient &mqtt_client, float map_x, float map_y) {  // mqtt_client 用于发布消息
    if (!g_state.marker_mode_enabled) {  // 模式校验：只有处于标记模式才允许发送
        SPDLOG_WARN("忽略地图标记请求，因为当前不在标记模式");  // 记录告警，便于发现异常调用
        return;  // 非标记模式直接忽略，避免误发
    }

    PublishMapClick(mqtt_client, map_x, map_y, g_state.marker_mode_include_sentry);  // 复用公共下发实现

    // 注意：这里**不再**自动退出标记模式。
    // 早先的写法是"发完立刻 exit"，操作手看到的是"点一下模式就没了、地图上什么都没留下"，
    // 会误以为点击没生效。现在保持模式直到操作手在面板里手动退出，方便连续打几个点
    // （进攻/防御/警戒），同时由小地图画出标记点做可视化确认。
}

// 功能：指挥台点地图时直接下发标记，不要求先切标记模式。
void send_map_marker_now(RoboGo::io::MqttClient &mqtt_client, float map_x, float map_y, bool include_sentry) {
    PublishMapClick(mqtt_client, map_x, map_y, include_sentry);  // 直接走公共下发实现
}

// 功能：把采样到的键鼠状态打包成一条 KeyboardMouseControl 并发布（只有上报线程调用）。
// 思路：一次性取走累计位移（exchange 归零），按键位掩码直接读；不在比赛模式时清空累计值，
//       避免下次进入比赛模式时把这一段位移一次性甩给机器人。
static void PublishKeyboardMouseOnce() {
    if (g_state.publisher_client == nullptr) {  // 线程已启动但客户端还没绑定（理论上不会发生）
        return;  // 直接返回
    }

    if (!g_state.game_mode_enabled.load(std::memory_order_acquire)) {  // 非比赛模式
        g_state.pending_mouse_x.store(0, std::memory_order_relaxed);  // 清空 X 位移，避免残留
        g_state.pending_mouse_y.store(0, std::memory_order_relaxed);  // 清空 Y 位移
        g_state.pending_mouse_z.store(0, std::memory_order_relaxed);  // 清空滚轮
        return;  // 不上报
    }

    // 取走本周期累计位移：exchange 保证"取走"与 UI 线程的 fetch_add 不丢不重。
    // 单包位移限幅：正常 14ms 内人手最多挪几个像素到几十像素，500 已经非常宽松。
    // 限幅的意义是兜住"极端卡顿/休眠唤醒/采样线程被挂起"这类情况：
    // 没有限幅时，攒下的几千位移会一次性发给机器人，云台会猛地甩到一边（现场很难解释）。
    constexpr int32_t kMaxMouseDeltaPerPacket = 500;  // 单包最大位移（像素）
    const int32_t raw_mouse_x = g_state.pending_mouse_x.exchange(0, std::memory_order_acq_rel);  // 取走 X 累计位移
    const int32_t raw_mouse_y = g_state.pending_mouse_y.exchange(0, std::memory_order_acq_rel);  // 取走 Y 累计位移
    const int32_t mouse_x = std::clamp(raw_mouse_x, -kMaxMouseDeltaPerPacket, kMaxMouseDeltaPerPacket);  // 限幅后的 X
    const int32_t mouse_y = std::clamp(raw_mouse_y, -kMaxMouseDeltaPerPacket, kMaxMouseDeltaPerPacket);  // 限幅后的 Y
    const int32_t mouse_z = g_state.pending_mouse_z.exchange(0, std::memory_order_acq_rel);  // 滚轮位移
    const uint32_t keyboard_value = g_state.pending_keyboard_value.load(std::memory_order_relaxed);  // 键盘位掩码
    const uint32_t buttons = g_state.pending_buttons.load(std::memory_order_relaxed);  // 鼠标按键掩码

    KeyboardMouseControl keyboard_mouse_control;  // 构造键鼠控制 Protobuf 消息
    keyboard_mouse_control.set_mouse_x(mouse_x);  // 设置 X 方向累计位移(已乘灵敏度)
    keyboard_mouse_control.set_mouse_y(-mouse_y);  // Y 轴取反以匹配裁判系统坐标系
    keyboard_mouse_control.set_mouse_z(mouse_z);  // 设置滚轮位移
    keyboard_mouse_control.set_left_button_down((buttons & kButtonLeft) != 0);  // 左键按下状态
    keyboard_mouse_control.set_right_button_down((buttons & kButtonRight) != 0);  // 右键按下状态
    keyboard_mouse_control.set_mid_button_down((buttons & kButtonMid) != 0);  // 中键按下状态
    keyboard_mouse_control.set_keyboard_value(keyboard_value);  // 打包后的 32 位键盘值

    std::string serialized;  // 准备接收序列化字节串
    if (keyboard_mouse_control.SerializeToString(&serialized)) {  // 序列化为字节串
        g_state.publisher_client->publish("KeyboardMouseControl", serialized);  // 发布到 KeyboardMouseControl 主题
    } else {  // 序列化失败(必填字段缺失/内存不足)
        SPDLOG_ERROR("键鼠控制序列化失败");  // 记录错误，但不影响后续周期
    }
}

// 功能：键鼠上报线程主循环：按固定周期取走采样值并发送，与渲染帧率完全解耦。
static void KeyboardMousePublishLoop() {
    using clock = std::chrono::steady_clock;  // 单调时钟：不受系统时间调整影响
    auto next_wake = clock::now();  // 下一次唤醒时刻（绝对时间，避免累计漂移）

    while (g_state.publisher_running.load(std::memory_order_acquire)) {  // 直到 stop 请求
        next_wake += kPublishPeriod;  // 推进到下一个周期
        std::this_thread::sleep_until(next_wake);  // 睡到目标时刻（已把系统定时器精度调到 1ms）
        const auto now = clock::now();  // 实际唤醒时刻
        if (now - next_wake > kPublishPeriod) {  // 落后超过一个周期（系统卡顿/休眠唤醒）
            next_wake = now;  // 重新对齐，避免"补发风暴"把链路打爆
        }  // 结束落后重对齐判断
        PublishKeyboardMouseOnce();  // 打包并发送一帧
    }  // 结束主循环
}

#ifdef _WIN32
// 功能：按约 1ms 的周期采样光标位置，把位移累加进待发累计量。
// 为什么要独立采样：鼠标位移是"累计量"，采样频率直接决定输入分辨率。
//   原来在渲染循环里读 io.MouseDelta，等于把鼠标采样率锁死在刷新率上：
//   - 60Hz 屏每 16.7ms 才采一次，慢速微调时位移被量化成一帧一个台阶，手感发涩；
//   - 某一帧卡了 100ms，这 100ms 的位移会攒成一包一次发出去，云台会突然甩一下；
//   - 换个 144Hz 显示器手感就变了，同一份配置在不同机器上不一致。
//   改成独立线程按 1ms 采样后，位移粒度提高约 16 倍，且与渲染帧率彻底无关。
void MouseSampleLoop() {
    POINT last_position{0, 0};  // 上一次采样到的光标位置
    bool has_last_position = false;  // 是否已有基准点
    while (g_state.mouse_sampler_running.load(std::memory_order_acquire)) {  // 直到 stop 请求
        // 非比赛模式：不累计位移，并丢弃基准点（避免进入比赛时把菜单里的移动一次性算进去）
        if (!g_state.game_mode_enabled.load(std::memory_order_acquire)) {
            has_last_position = false;  // 丢弃基准
            std::this_thread::sleep_for(std::chrono::milliseconds(4));  // 空闲时降低唤醒频率
            continue;  // 下一轮
        }

        POINT current_position{0, 0};  // 本次采样结果
        if (GetCursorPos(&current_position) == 0) {  // 取不到（锁屏/切换桌面/会话切换）
            has_last_position = false;  // 丢弃基准，避免恢复后算出一个巨大的跳变
            std::this_thread::sleep_for(std::chrono::milliseconds(4));  // 稍后再试
            continue;  // 下一轮
        }

        if (!has_last_position) {  // 刚进入比赛模式：这一轮只记录基准
            last_position = current_position;  // 记录基准
            has_last_position = true;  // 标记已有基准
            std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 1ms 后再采
            continue;  // 不产生位移
        }

        const int32_t delta_x = current_position.x - last_position.x;  // X 位移（物理像素）
        const int32_t delta_y = current_position.y - last_position.y;  // Y 位移（物理像素）
        last_position = current_position;  // 更新基准
        if (delta_x != 0 || delta_y != 0) {  // 有移动才累加
            const float sensitivity = g_state.mouse_sensitivity.load(std::memory_order_relaxed);  // 取当前灵敏度
            g_state.pending_mouse_x.fetch_add(static_cast<int32_t>(delta_x * sensitivity), std::memory_order_relaxed);  // 累加 X
            g_state.pending_mouse_y.fetch_add(static_cast<int32_t>(delta_y * sensitivity), std::memory_order_relaxed);  // 累加 Y
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 1ms 周期（timeBeginPeriod(1) 已把精度提上来）
    }
}
#endif

// 功能：启动键鼠上报线程（进程内只启动一次）。
// 参数 mqtt_client：发布用的客户端，调用方必须保证其生命周期长于本线程（application 里先 stop 再停 MQTT）。
void start_keyboard_mouse_publisher(RoboGo::io::MqttClient &mqtt_client) {
    if (g_state.publisher_running.exchange(true, std::memory_order_acq_rel)) {  // 已启动则不重复创建
        return;  // 直接返回
    }
    g_state.publisher_client = &mqtt_client;  // 绑定客户端（线程退出前不会被清空）
#ifdef _WIN32
    timeBeginPeriod(1);  // 把系统定时器精度提到 1ms，否则 sleep_until(14ms) 会被拉到 ~15.6ms
#endif
    g_state.publisher_thread = std::thread(KeyboardMousePublishLoop);  // 启动上报线程
#ifdef _WIN32
    g_state.mouse_sampler_running.store(true, std::memory_order_release);  // 置位采样线程标志
    g_state.mouse_sampler_thread = std::thread(MouseSampleLoop);  // 启动 1ms 光标采样线程
#endif
    SPDLOG_INFO("键鼠上报线程已启动 period_ms={} expected_hz={}",  // 记录实际配置，便于现场核对频率
                kPublishPeriod.count(),  // 周期毫秒
                1000 / kPublishPeriod.count());  // 预期频率（71Hz，低于协议 75Hz 上限）
}  // start_keyboard_mouse_publisher 结束

// 功能：停止键鼠上报线程并回收（幂等）。
void stop_keyboard_mouse_publisher() {
    if (!g_state.publisher_running.exchange(false, std::memory_order_acq_rel)) {  // 未启动或已停止
        return;  // 直接返回
    }
#ifdef _WIN32
    g_state.mouse_sampler_running.store(false, std::memory_order_release);  // 先停采样线程
    if (g_state.mouse_sampler_thread.joinable()) {  // 可连接才需要 join
        g_state.mouse_sampler_thread.join();  // 最多等 1ms
    }  // 结束采样线程回收
#endif
    if (g_state.publisher_thread.joinable()) {  // 线程可连接才需要 join
        g_state.publisher_thread.join();  // 最多等一个上报周期（14ms）
    }
#ifdef _WIN32
    timeEndPeriod(1);  // 还原系统定时器精度，避免影响其它进程
#endif
    g_state.publisher_client = nullptr;  // 解绑客户端，防止悬空指针被误用
    SPDLOG_INFO("键鼠上报线程已退出");  // 记录线程生命周期
}  // stop_keyboard_mouse_publisher 结束

// 功能：每帧由 UI 线程调用，只做"采样"，不再直接发布。
// 思路：Esc 切换比赛模式 -> 在模式边沿通知平台捕获输入 -> 把按键/鼠标增量写进原子变量 ->
//       由上报线程按固定周期取走并发送。所有 ImGui 访问都留在 UI 线程，不跨线程读 ImGui 状态。
void update_keyboard_mouse_state(RoboGo::io::MqttClient &mqtt_client) {
    (void) mqtt_client;  // 发布已移到上报线程，本函数不再直接使用客户端（保留形参以稳定调用点）
    ImGuiIO &io = ImGui::GetIO();  // 获取 ImGui IO，用于读取鼠标/键盘输入

    // 指挥台是常驻界面：Esc 在指挥台内既不切比赛模式、也不退出（现场误触一下不该掉线），
    // 离开统一走顶栏的"退出登录"按钮。普通界面里 Esc 仍然照旧切换比赛模式。
    const bool esc_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);  // 本帧是否按下 Esc（只取一次，避免后续分支重复触发）
    if (g_state.commander_console_active) {  // 指挥台激活期间
        g_state.game_mode_enabled.store(false, std::memory_order_release);  // 强制保持非比赛模式：鼠标可见、键鼠不上报
    } else if (esc_pressed) {  // 普通界面按 Esc 切换比赛模式
        const bool enabled = !g_state.game_mode_enabled.load(std::memory_order_acquire);  // 取反当前状态
        g_state.game_mode_enabled.store(enabled, std::memory_order_release);  // 写回切换后的状态
        SPDLOG_INFO("比赛模式切换 enabled={}", enabled);  // 记录切换后的状态
    }

    static bool was_in_game_mode = false;  // 记录上一帧的比赛模式状态，用于检测进入/退出边沿
    const bool in_game_mode = g_state.game_mode_enabled.load(std::memory_order_acquire);  // 本帧的比赛模式状态
    // 在比赛模式进入或退出的边沿，通知平台层捕获或释放游戏输入(独占键鼠)
    if (in_game_mode) {  // 当前处于比赛模式
        if (!was_in_game_mode) {  // 上一帧不在 → 检测到"进入"边沿
            if (g_state.game_input_capture_handler) {  // 回调已注册才调用，避免空 std::function 调用抛异常
                g_state.game_input_capture_handler(true);  // 进入比赛模式：请求平台捕获输入
            }
        }
    } else if (was_in_game_mode) {  // 当前不在、上一帧在 → 检测到"退出"边沿
        if (g_state.game_input_capture_handler) {  // 同样先判空
            g_state.game_input_capture_handler(false);  // 退出比赛模式：释放输入捕获
        }
    }
    was_in_game_mode = in_game_mode;  // 更新边沿检测用的上一帧状态

    if (!in_game_mode) {  // 非比赛模式：清零累计位移并跳过采样
        g_state.pending_mouse_x.store(0, std::memory_order_relaxed);  // 清空 X 位移，避免切回时带上历史残留
        g_state.pending_mouse_y.store(0, std::memory_order_relaxed);  // 清空 Y 位移
        g_state.pending_mouse_z.store(0, std::memory_order_relaxed);  // 清空滚轮位移
        return;  // 非比赛模式直接返回，不做后续采样
    }

    // 鼠标位移已由 MouseSampleLoop 按 1ms 独立采样，这里**不再**读 io.MouseDelta（否则会重复累计，
    // 而且又把采样率锁回渲染帧率）。滚轮没有高频采样源，仍按帧取用即可（离散事件，不受帧率影响）。
    g_state.pending_mouse_z.fetch_add(static_cast<int32_t>(io.MouseWheel), std::memory_order_relaxed);  // 滚轮增量不乘灵敏度

    // 将各按键状态按位打包进 32 位键盘值(低 16 位对应 W/S/A/D/Shift/Ctrl/Q~B 等键)。
    // 关键：必须用 IsKeyDown 取"当前是否按住"，不能用 IsKeyPressed —— 后者只在按下那一帧为真，
    //       按住 W 时绝大部分帧会发 0，机器人只会抽动一下、无法连续前进。
    std::bitset<32> keyboard_value;  // 32 位位集，下标即裁判协议中的按键位号
    keyboard_value[0] = ImGui::IsKeyDown(ImGuiKey_W);  // 记录 W 键(前进)
    keyboard_value[1] = ImGui::IsKeyDown(ImGuiKey_S);  // 记录 S 键(后退)
    keyboard_value[2] = ImGui::IsKeyDown(ImGuiKey_A);  // 记录 A 键(左移)
    keyboard_value[3] = ImGui::IsKeyDown(ImGuiKey_D);  // 记录 D 键(右移)
    keyboard_value[4] = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);  // 记录 Shift 键(左右均可)
    keyboard_value[5] = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);  // 记录 Ctrl 键(左右均可)
    keyboard_value[6] = ImGui::IsKeyDown(ImGuiKey_Q);  // 记录 Q 键
    keyboard_value[7] = ImGui::IsKeyDown(ImGuiKey_E);  // 记录 E 键
    keyboard_value[8] = ImGui::IsKeyDown(ImGuiKey_R);  // 记录 R 键
    keyboard_value[9] = ImGui::IsKeyDown(ImGuiKey_F);  // 记录 F 键
    keyboard_value[10] = ImGui::IsKeyDown(ImGuiKey_G);  // 记录 G 键
    keyboard_value[11] = ImGui::IsKeyDown(ImGuiKey_Z);  // 记录 Z 键
    keyboard_value[12] = ImGui::IsKeyDown(ImGuiKey_X);  // 记录 X 键
    keyboard_value[13] = ImGui::IsKeyDown(ImGuiKey_C);  // 记录 C 键
    keyboard_value[14] = ImGui::IsKeyDown(ImGuiKey_V);  // 记录 V 键
    keyboard_value[15] = ImGui::IsKeyDown(ImGuiKey_B);  // 记录 B 键

    g_state.pending_keyboard_value.store(static_cast<uint32_t>(keyboard_value.to_ulong()), std::memory_order_relaxed);  // 写入键盘位掩码

    uint32_t buttons = 0;  // 鼠标按键掩码
    if (io.MouseDown[0]) {  // 左键按下
        buttons |= kButtonLeft;  // 置位左键
    }
    if (io.MouseDown[1]) {  // 右键按下
        buttons |= kButtonRight;  // 置位右键
    }
    if (io.MouseDown[2]) {  // 中键按下
        buttons |= kButtonMid;  // 置位中键
    }
    g_state.pending_buttons.store(buttons, std::memory_order_relaxed);  // 写入鼠标按键掩码
}
