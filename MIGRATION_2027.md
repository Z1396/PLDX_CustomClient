# RoboGo 自定义客户端 → RoboMaster 2027 通信协议 迁移计划

> 目标协议：《RoboMaster 2027 机甲大师高校系列赛通信协议 V1.0.0（20261029）》
> 当前项目：`proto/messages.proto` 为 2026 版 V2.0.0（20260626）
> 本文件只做【计划 + 提示词】，供评审；确认后再按 Phase 执行。

---

## 一、连接参数（无需改动，核对即可）

| 项 | 2027 协议 | 项目现状 | 结论 |
|---|---|---|---|
| MQTT 服务器 | 192.168.12.1 : 3333 | 默认 3333 | ✅ 一致 |
| 自定义终端 IP | 192.168.12.2 | 运行时绑定 | ✅ |
| 图传（视频） | UDP **3334**，HEVC/H.265，1920×1080，60fps | `application.cpp` 默认 `video_port="3334"`，解码器用 HEVC | ✅ 一致（需确认解码器确为 HEVC） |
| UDP 包格式 | 首 8 字节：帧号(2B)/片序号(2B)/帧总字节(4B)，大端；单包≤1400（载荷 1392） | `video_stream_receiver`/`video_shards_manager` 已按 8 字节头处理 | ✅ 基本一致（核对） |
| 传输方式 | MQTT 直接传 Protobuf 二进制，topic=指令名 | 同 | ✅ |

---

## 二、消息级变更（Proto）

### 2.1 删除的 4 个消息（2027 已不存在）
| 消息 | 方向 | 影响 |
|---|---|---|
| `HeroDeployModeEventCommand` | 上行 | 英雄部署模式指令，删除 |
| `AirSupportCommand` | 上行 | 空中支援指令，删除 |
| `AirSupportStatusSync` | 下行 | 空中支援状态，删除 |
| `DeployModeStatusSync` | 下行 | 英雄部署状态，删除 |

> 2027 因此把后续指令序号整体前移（如 AssemblyCommand 由 2.2.20→2.2.21，DartCommand 2.2.30→2.2.29 等）。

### 2.2 保留但字段/枚举有变化的消息
- **`GlobalUnitStatus`**（重点）：
  - `outpost_health/outpost_status` → `outpost_R_health/outpost_R_status`
  - `enemy_outpost_health/enemy_outpost_status` → `outpost_B_health/outpost_B_status`
  - 新增 `total_CP_ally=15`、`total_CP_enemy=16`（累计控制度）
  - `robot_health` 顺序由「己方 1,2,3,4,7 / 对方 1,2,3,4,7」改为「己方 1,3,4,7 / 对方 1,3,4,7」（**去掉 2 号**，每方由 5→4）
  - `base_shield`/`enemy_base_shield` **字段名不变但语义变为"阵地控制状态"**（1=己方完全控制,2=对方临时控制…），不再是"护盾值"
- **`Event`**（重点，事件编号重排）：
  - 删除旧 5/6（己方/对方英雄狙击伤害）、旧 7/8（对方呼叫/反制空中支援）
  - 新增 5/6（R/B 前哨阵地控制状态变更）、7/8（R/B 基地阵地改变控制方）
  - 飞镖命中由 9→9（不变）；新增 16（重装成功安装模块化 42mm 发射机构）
  - 14 由"装配强制退出"改为"重装战亡临时激活强制退出"
- **`RobotDynamicStatus`**：字段 4 由 `current_chassis_energy` 改为 `reserved`（保留位）→ `current_chassis_energy()` 访问器将不存在
- **`RobotModuleStatus`**：字段 11 由 `laser_detection_module` 改为 `modular_42mm`（是否拥有模块化 42mm 安装权限，仅重装）；字段 4/5 注释改为 17mm/42mm **测速**模块
- **`CustomControl`**：最大字节 **30 → 60**
- **`GameStatus`**：频率 1Hz → 5Hz（仅注释/认知，字段不变）
- **`RobotStaticStatus`**：性能体系枚举收敛为 shooter{1,2}、chassis{1,2}（去掉英雄近战/远程 3,4）

### 2.3 完全不变的消息
KeyboardMouseControl、MapClickCmd/Info、RadarInfoToClient、CustomByteBlock、AssemblyCommand、TechCoreMotionStateSync、RobotPerformanceSelectionCommand/Sync、CommonCommand、RuneActivateCommand、RuneStatusSync、SentryStatusSync、DartCommand、DartSelectTargetStatusSync、SentryCtrlCommand、SentryCtrlResult、RobotInjuryStat、RobotRespawnStatus、RobotPosition、Buff、PenaltyInfo、RobotPathPlanInfo、GlobalLogisticsStatus、GlobalSpecialMechanism（枚举扩到 1-6，字段不变）。

---

## 三、代码影响面（已定位）

| 文件 | 触及点 |
|---|---|
| `proto/messages.proto` | 见第二节，需重写并重新生成 `gen/messages.pb.{h,cc}` |
| `src/io/mqtt_client.cpp` | 订阅列表删除 `DeployModeStatusSync`、`AirSupportStatusSync`（25→23，正好等于 2027 下行数） |
| `src/main_window.cpp` | `HandleMqttMessage` 删除两个 topic 分支；Event 事件号重映射；GlobalUnitStatus 字段名/索引更新；删除空中支援一系列调用；删除 `GetAirSupportStatusName/ShooterStatusName` |
| `src/main_window.h` | 删除 State 字段 `deploy_mode_status_sync`/`air_support_status_sync`/`air_support_custom_control` 与两个函数声明 |
| `src/panel/bottom_panel.cpp` | 删除空中支援状态文本块 |
| `src/panel/header_panel.cpp` | `base_shield` 不再叠加到血量；前哨站改 `outpost_R/B` |
| `src/panel/robot_status_panel.cpp` | 删除 `current_chassis_energy()` 显示；`laser_detection_module()` → `modular_42mm()` |
| `src/panel/start_panel.cpp` | 删除空中支援面板、英雄部署 UI、`SendAirSupportCommand`；保留飞镖/装配/兑换 |
| `src/custom_control_payload.{h,cpp}` | CustomControl 载荷里的"空中支援呼叫标志/冷却"2 字节处理（见决策点） |

### ⚠️ 决策点（需与机械/嵌入式队友确认）
`CustomControl`（0x0311）载荷目前含 2 字节空中支援数据（`kCustomControlAirSupportCallOffset`、`kCustomControlCounterCooldownOffset`）。2027 删除空中支援后：
- **默认方案 A（推荐，先行）**：保留载荷布局不变，将这 2 字节**填 0**，避免破坏偏移与机器人端解析；
- 方案 B：与机器人端协商重新定义这 2 字节用途。
> 在队友确认前，按 A 处理（填 0），保证其它字段偏移不变。

---

## 四、构建前置条件（重要）
- `gen/messages.pb.{h,cc}` 是**提交进仓库的预生成物**，项目 `protobuf_BUILD_PROTOC_BINARIES=OFF`，**不会自动重新生成**。
- 库为 protobuf **v35.0**，必须用**匹配的 protoc（35.x）**重新生成，否则 ABI/API 不匹配。
- 需先确认本机有 protoc 35.x（`protoc --version`）；若无，需先安装/获取，再执行 Phase 1 的生成步骤。
- CMake 同时把 `proto/messages.proto` 作为内嵌资源（`add_embedded_resource`），改 .proto 会一并生效。

---

## 五、分步实施计划

- **Phase 0 前置核对**：确认 protoc 35.x 可用；确认视频解码器为 HEVC；确认 CustomByteBlock（0x0310）分帧与 2027 §1.4.1 一致。只读，不改代码。
- **Phase 1 proto 升级 + 重新生成**：按第二节改 `proto/messages.proto`；用 protoc 35.x 重新生成 `gen/messages.pb.{h,cc}`。
- **Phase 2 通信层**：`mqtt_client.cpp` 订阅表、`main_window.cpp` 的 `HandleMqttMessage`（删分支、Event 重映射、GlobalUnitStatus 字段/索引）。
- **Phase 3 UI/状态清理**：`main_window.h/.cpp`、`bottom_panel.cpp`、`header_panel.cpp`、`robot_status_panel.cpp`、`start_panel.cpp` 删除空中支援/英雄部署/激光模块/底盘能量，改模块化 42mm 与控制度。
- **Phase 4 CustomControl 载荷**：按决策点 A，将空中支援 2 字节填 0（保留偏移）。
- **Phase 5 编译 + 冒烟**：全量构建通过；启动无崩溃；如能连 2027 服务器则验证 topic 收发。

---

## 六、子代理提示词（按 Phase 复制粘贴）

> 通用安全约束（已并入每条提示词）：**这是真实代码改动，不是加注释**；只改与 2027 迁移相关的内容，不要顺手重构；不得改动无关文件；每步结束必须 `cmake --build build/release --target RoboGo -j 4` 验证能编译链接，汇报错误。改动要保留现有的逐行中文注释风格（新增代码同样加 `//` 注释）。

### Phase 0 提示词（只读核对，不改代码）
```
任务：为 RoboGo 迁移到 2027 协议做前置核对，只读不改代码。
1) 确认本机 protoc 版本：运行 `protoc --version`，报告版本号（需为 35.x 以匹配 protobuf v35.0）；若不是 35.x 或找不到，明确说明。
2) 读 src/video/video_decoder.cpp 与 src/video/video_decoder.h，确认图传解码器编码标准是 HEVC/H.265（AV_CODEC_ID_HEVC / hevc），报告结论。
3) 读 src/video/custom_byte_block_video_decoder*.{h,cpp} 与 src/video/video_shards_manager.*，摘要 CustomByteBlock（0x0310）与 UDP 分片（8 字节头：帧号2B/片序2B/帧总字节4B 大端）当前解析逻辑，判断是否与 2027 协议一致，列出疑点。
4) 读 src/io/video_stream_receiver.cpp 确认 UDP 接收端口默认 3334。
只报告结论与疑点，不做任何修改。
```

### Phase 1 提示词（proto 升级 + 重新生成）
```
任务：把 proto/messages.proto 从 2026 V2.0.0 升级到 2027 V1.0.0，并用 protoc 35.x 重新生成 gen/messages.pb.{h,cc}。严格按下列清单改，不要改动无关消息：

[删除 4 个 message]：HeroDeployModeEventCommand、AirSupportCommand（上行）、AirSupportStatusSync、DeployModeStatusSync（下行）。

[改 GlobalUnitStatus]：outpost_health→outpost_R_health、outpost_status→outpost_R_status、enemy_outpost_health→outpost_B_health、enemy_outpost_status→outpost_B_status；新增 optional uint32 total_CP_ally=15、total_CP_enemy=16（累计控制度）；robot_health 注释顺序改为「己方1,3,4,7 / 对方1,3,4,7」；base_shield/enemy_base_shield 注释改为"基地阵地控制状态(1=己方完全控制,2=对方临时控制…)"而非护盾。

[改 RobotDynamicStatus]：字段4 current_chassis_energy 改名 reserved（保留位），注释"保留位"。

[改 RobotModuleStatus]：字段11 laser_detection_module 改名 modular_42mm（是否拥有模块化42mm发射机构安装权限，仅重装，0未拥有/1拥有）；字段4/5 注释改为 17mm/42mm 测速模块状态。

[改 Event]：event_id 枚举注释更新为 2027：1击杀(参数1被击杀者,2击杀者)、2前哨站被摧毁(红11/蓝111)、3大能量机关激活(灯臂数,平均环数)、4能量机关进入已激活(1小/2大)、5 R前哨阵地控制状态变更(1红临时2红完全3蓝临时4蓝完全)、6 B前哨阵地控制状态变更(同5)、7 R基地阵地改变控制方(1红完全2蓝临时)、8 B基地阵地改变控制方(1蓝完全2红临时)、9飞镖命中(参数1命中方1红2蓝,参数2目标1前哨2基地固定3随机固定4随机移动5末端移动)、10对方飞镖闸门开启、11基地遭攻击(5s冷却)、12对方前哨站停转、13对方基地护甲展开、14重装战亡临时激活强制退出缓冲期、15装配结果(0成功1拔出2超时3离开装配区过久4重装战亡5四级未满足协作时限6主动退出7完成装配但无能量单元8缓冲期到期强制结束)、16重装成功安装模块化42mm发射机构。

[改 CustomControl]：data 注释"最大30字节"改为"最大60字节"。

[改 GameStatus]：频率注释 1Hz 改 5Hz（字段不变）。
[改 RobotStaticStatus]：performance_system_shooter 枚举只留 1冷却优先/2爆发优先；chassis 只留 1血量优先/2功率优先。
[文件头]：版本注释改为「RoboMaster 2027 ... V1.0.0 (20261029)」；各消息前的 "// 2.2.x" 编号按 2027 重新编号（删除消息后整体前移）。

完成后：用 protoc 重新生成。命令示例（在仓库根，protoc 需 35.x，且 include 路径覆盖 proto/）：
  protoc --cpp_out=gen proto/messages.proto
若 protoc 版本不是 35.x 或缺失，停止并报告，不要强行生成。生成后 `git diff --stat gen/` 确认 pb 文件已更新。不要运行完整编译（交由后续 Phase）。
```

### Phase 2 提示词（通信层）
```
任务：迁移 src/io/mqtt_client.cpp 与 src/main_window.cpp 的协议处理到 2027。gen/messages.pb.* 已在 Phase 1 更新。
1) mqtt_client.cpp 订阅表：删除 {"DeployModeStatusSync",...} 与 {"AirSupportStatusSync",...} 两项（其余 23 个下行 topic 保持不变）。
2) main_window.cpp HandleMqttMessage：删除 topic=="DeployModeStatusSync" 与 topic=="AirSupportStatusSync" 两个 else-if 分支（含其中的消息解析与 State 写入）。
3) Event 分支：把 event_id 的判定/展示按 2027 新编号重映射（删除空中支援/狙击类旧编号逻辑；新增 5-8 阵地控制、16 模块化42mm；14 改为"重装战亡临时激活"）。若原代码按 event_id 数字硬编码判断，逐条对照更新。
4) GlobalUnitStatus 分支：字段访问从 outpost_health()/enemy_outpost_health() 等改为 outpost_R_health()/outpost_B_health()/outpost_R_status()/outpost_B_status()；robot_health / robot_bullets 的索引逻辑按「每方 4 台、顺序 1,3,4,7（无 2 号）」更新（原 5/6 台的 half 判断改为 4，并保留下标越界检查）；如界面要展示控制度，可接 total_CP_ally/enemy。
改完运行 `cmake --build build/release --target RoboGo -j 4`，修到编译链接通过，汇报剩余错误（若有）。
```

### Phase 3 提示词（UI/状态清理）
```
任务：清理 2027 已删除特性对应的 UI 与状态。gen/messages.pb.* 已更新，Phase 2 已完成。
- main_window.h：删除 State 中 deploy_mode_status_sync、air_support_status_sync、air_support_custom_control 三个成员；删除 GetAirSupportStatusName/GetAirSupportShooterStatusName 声明。
- main_window.cpp：删除上述两函数定义；删除对 NoteOpponentAirSupportCalled / ResetAirSupportCounterForMatchStart / NoteAirSupportCounterRemaining 的调用，以及 CustomControl 载荷中空中支援字节的打印逻辑；删除 HandleMqttMessage 里遗留的空中支援/部署模式相关代码。
- bottom_panel.cpp：删除空中支援状态文本构造与显示整段。
- header_panel.cpp：基地显示不再把 base_shield/enemy_base_shield 叠加进血量（它现在是阵地控制状态，不是护盾值）；前哨站读取改用 outpost_R_health/outpost_B_health；如需可展示控制度。
- robot_status_panel.cpp：删除 current_chassis_energy() 相关显示（2027 该字段为 reserved）；把 laser_detection_module() 替换为 modular_42mm()（模块化42mm安装权限，仅重装有意义）或移除该行。
- start_panel.cpp：删除空中支援面板(ShowAirSupportPanel/SendAirSupportCommand 等)、英雄部署模式 UI、HeroDeployModeEventCommand/AirSupportCommand 的发布；保留飞镖(DartCommand)、装配(AssemblyCommand)、兑换/复活(CommonCommand)、性能选择等并确认用的消息名/字段与 2027 一致。
- custom_control_payload.{h,cpp}：先不要删结构；仅确保后续 Phase 4 能把空中支援两字节填 0。
每改完一个文件编译一次；全部完成后 `cmake --build build/release --target RoboGo -j 4` 通过。保留并更新逐行中文注释；不要顺手重构无关代码。
```

### Phase 4 提示词（CustomControl 载荷）
```
任务：2027 删除空中支援后，按"保留布局、填 0"方案处理 CustomControl 载荷中的空中支援 2 字节（决策点 A，待机器人端最终确认）。
- 在 src/custom_control_payload.cpp 打包函数里，把写入 kCustomControlAirSupportCallOffset 与 kCustomControlCounterCooldownOffset 的字节固定为 0（可保留偏移常量与函数签名以稳定布局，但不再依赖 AirSupportCustomControlState 的真实计时；如更简洁可移除相关状态函数并在注释中说明"2027 起该 2 字节保留填 0"）。
- 同步更新 main_window.cpp / bottom_panel.cpp 中不再需要的空中支援计数显示（若 Phase 3 未清干净）。
- 在 custom_control_payload.h 顶部加注释说明：这 2 字节为保留位，待与机器人端协商后再定义用途。
完成后 `cmake --build build/release --target RoboGo -j 4` 通过。不要改动 CustomControl 其它字节偏移。
```

### Phase 5 提示词（编译 + 冒烟）
```
任务：全量验证迁移结果。
1) `cmake --build build/release --target RoboGo -j 4`，确保 0 error、成功链接 RoboGo.exe；若报错定位到具体文件/行并修复（大概率是残留的已删消息/字段访问）。
2) 全仓搜索残留引用并报告（应为空或已注释说明）：AirSupport、DeployMode、HeroDeploy、laser_detection_module、current_chassis_energy、outpost_health(而非 outpost_R/B)、AirSupportStatusSync、DeployModeStatusSync。
3) 如环境允许，短时运行 RoboGo.exe（数秒）确认不崩溃、日志正常。
汇报：构建结果、残留搜索结果、运行结果。
```

---

## 七、验收清单
- [ ] `gen/messages.pb.{h,cc}` 已用 protoc 35.x 重新生成，与 2027 proto 一致
- [ ] proto 中 4 个消息已删除；GlobalUnitStatus/Event/RobotDynamicStatus/RobotModuleStatus/CustomControl 变更落地
- [ ] 订阅 topic = 2027 全部 23 个下行指令；上行指令仅保留 2027 允许集
- [ ] 空中支援/英雄部署/激光检测/底盘能量相关 UI 与状态清理干净，无编译错误
- [ ] `cmake --build ... -j 4` 0 error 链接成功；启动不崩溃
- [ ] （可选）实连 2027 服务器验证 topic 收发
