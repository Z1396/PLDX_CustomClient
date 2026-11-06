# 破浪笃行战队 · RoboMaster 2027 赛季自定义终端

2027 赛季自定义终端：C++20 / ImGui / D3D12（Linux 备用 Vulkan）/ FFmpeg / async_mqtt + Protobuf，
接入裁判系统 MQTT 比赛数据与 UDP 图传，提供操作手 HUD、综合指挥台、战术地图与键鼠上报。

## 自定义终端使用方法

### 一、准备环境

主力构建为 Windows（clang-cl + Ninja）：

1. 安装 CMake、Ninja，以及 LLVM（clang-cl）或 MSVC 生成工具
2. 第三方依赖已随仓库放在 `3rdparty_downloads/`，离线即可配置，无需联网
3. Linux 备用后端另需 Vulkan SDK 与 GLFW（见 `CMakeLists.txt` / `CMakePresets.json`）

### 二、编译代码

```sh
cmake --preset release
cmake --build --preset release
```

Windows / Linux 一键编译运行脚本：

```sh
python build.py                     # 构建并运行
python build.py --release --no-run  # 只构建
```

### 三、运行程序

```sh
build\release\RoboGo.exe    # Windows
./build/release/RoboGo      # Linux
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
