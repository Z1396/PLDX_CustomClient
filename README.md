# 破浪笃行战队 · RoboMaster 2027 赛季自定义终端

2027 赛季自定义终端：C++23 / ImGui / D3D12（Linux 用 Vulkan）/ FFmpeg / async_mqtt + Protobuf，
接入裁判系统 MQTT 比赛数据与 UDP 图传，提供操作手 HUD、综合指挥台、战术地图与键鼠上报。

> 规则提示：2026 版制作规范 S87/S88 明确要求「自定义客户端等运算平台必须使用开源操作系统，
> 不得使用 Windows/macOS」；2027 版 S60 表述有调整，但赛场部署仍按裸 Linux 准备。
> Windows 只作为开发机，不参与赛场运行。

## 自定义终端使用方法

### 一、准备环境

Windows（开发机，clang-cl + Ninja）：

1. 安装 CMake、Ninja，以及 LLVM（clang-cl）或 MSVC 生成工具
2. 第三方依赖已随仓库放在 `3rdparty_downloads/`，离线即可配置，无需联网

Ubuntu 24.04（赛场运行 / Linux 构建）：

```sh
bash tools/setup_ubuntu.sh      # 安装编译器、Vulkan、GLFW 所需的 X11/Wayland 开发包
```

脚本会检查 GCC 版本（需 12+，因项目使用 C++23 `std::expected`）并用 `vulkaninfo` 确认有真实 GPU。
GLFW 由 `3rdparty_downloads/glfw` 编译，不依赖系统 `libglfw-dev`。

### 二、编译代码

```sh
# Windows
cmake --preset release && cmake --build --preset release

# Linux
cmake --preset linux-release && cmake --build --preset linux-release
```

Windows / Linux 一键编译运行脚本（自动选择对应 preset）：

```sh
python build.py                     # 构建并运行（Linux 用 python3）
python build.py --release --no-run  # 只构建
```

### 三、运行程序

```sh
build\release\RoboGo.exe        # Windows
./build/linux-release/RoboGo    # Linux
```

Linux 上单元测试与协议自检：

```sh
ctest --test-dir build/linux-release --output-on-failure
python3 tools/mock_rm_server.py --selftest
```

常用参数：

| 参数 | 说明 |
|---|---|
| `--mqtt_server tcp://192.168.12.1:3333` | 指定裁判系统地址（默认即此值） |
| `--dev` | 连本地假数据源 `127.0.0.1:3333` |
| `--video-test` | 合成 1080p60 测试画面，无图传也可自检 |
| `--capture off\|mqtt\|all` | 抓包档位，赛场建议 `off` 或 `mqtt` |
| `--video_port 3334` | 图传 UDP 端口 |

本地联调：`python tools/mock_rm_server.py --robot-id 3`，再启动 `RoboGo.exe --dev`。
快捷键：`F12` 性能浮层、`M` 地形分层、`I` 图传反转、`F1` 重装装配浮层、`Esc` 比赛模式切换。

## 声明
1.本次开源文件的最终解释权归桂林电子科技大学北海破浪笃行战队所有,作品仅用于技术交流，未经作者允许，不得作任何商业用途。

2.开源目的在于加强参赛队之间交流，有助于提高自身和其他参赛队的水平。

3.本次项目参考了RM26-东莞理工学院ACE战队自定义客户端的技术栈。解决了技术栈选型上的问题;
参考了复旦大学星云 EGA 战队的综合指挥界面设计;
使用了https://github.com/diaofenyuan/RMUC_2027map_unity的28 × 15 m 的 2800 × 1500 底图用于区分高度,
特此感谢。

## 通讯信息

QQ号：2906361832
