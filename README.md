# LanAudio

通过 Android USB 网络共享，将 Windows 系统音频传输到 Android，优先保持音频连贯，其次限制延时，再保证音质。支持场景只考虑 USB 网络。

版本 `1.2.5`，音频协议 `v2`，默认音频 UDP 端口 `30010`，发现 UDP 端口 `30011`。支持 Windows 10/11 和 Android 8.0+，服务端与客户端须使用同一音频协议版本及包大小。开发约束见 [AGENTS.md](AGENTS.md)，使用、实现和测试说明统一放在本文。

## 快速运行

### 1. 构建服务端

使用 Visual Studio Build Tools：

```powershell
cmake -S server -B server/build -A x64
cmake --build server/build --config Release
.\server\build\Release\lan-audio-server.exe
```

使用 MinGW/Ninja：

```powershell
cmake -S server -B server/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build server/build
.\server\build\lan-audio-server.exe
```

两种构建方式二选一，不要在同一个目录中混用生成器。运行时可用第一个参数覆盖端口，例如 `lan-audio-server.exe 30010`。重新构建前退出旧服务端，否则 Windows 不能覆盖正在运行的 EXE。

### 2. 构建 Android 客户端

使用 Android Studio 打开 `android` 目录，执行 Gradle Sync，然后构建 `app` 模块。需要 SDK 35、NDK、CMake 3.22.1 和 Android Studio 自带 JBR 或 JDK 17+。

也可在 `android` 目录运行：

```powershell
.\gradlew.bat :app:assembleDebug
```

Oboe 1.9.3 通过 Maven 和 Prefab 导入：`buildFeatures.prefab = true`，CMake 链接 `oboe::oboe`。不需要手动下载或编译 Oboe 源码。

APK 输出位置：

```text
android/app/build/outputs/apk/debug/app-debug.apk
```

### 3. 连接播放

1. 用数据线连接 Windows 和 Android，在手机系统设置中开启 USB 网络共享。客户端底部“USB 网络共享设置”可跳转设置页面。
2. 在 Windows 防火墙中允许服务端通过 USB 共享网络，放行 UDP `30010` 和 `30011`。该网卡可能被识别为公用网络。
3. 启动服务端。
4. 安装 APK 并打开，客户端自动扫描约 3 秒。
5. 在“USB 网络服务器”列表中点选服务器即可连接；刷新按钮可重新扫描。

USB 网卡通常被 Windows 识别为公用网络。在**管理员 PowerShell** 中，从项目目录执行以下命令，允许本地子网访问指定程序的音频和发现端口，无需修改网络类型：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\server\allow-firewall.ps1 -Server .\server\build\lan-audio-server.exe
```

`-Server` 必须指向实际运行的 EXE；Visual Studio 构建可指定 `server/build/Release/lan-audio-server.exe`，临时更新版本可指定 `server/build/update/lan-audio-server.exe`。程序移动后重新运行脚本；自定义端口可加 `-Port` 和 `-DiscoveryPort`。脚本创建或更新同一条规则，适用于公用、专用和域网络，只允许本地子网。若此前在系统弹窗中拒绝访问，已有明确阻止规则会优先于允许规则，需在高级防火墙设置中删除或禁用该程序的对应阻止规则。无需关闭整个防火墙。

列表显示 Windows 计算机名称、IPv4 地址及音频端口，协议或包大小不兼容时禁止点选连接。也可以手动输入 IPv4 地址和端口，点击“连接”。地址和端口会保存在 Android 本地，下次启动自动恢复。前台服务支持息屏播放，通知栏提供断开按钮。

自动发现使用独立的 UDP 广播查询和单播响应（发现协议 `v1`），不会启动音频或接管正在播放的客户端。仅在界面可见且未连接时扫描，连接或退出界面即停止。服务器即使使用自定义音频端口，也会通过 `30011` 返回该端口。第二个启动参数可覆盖发现端口，例如 `lan-audio-server.exe 30010 30011`；Android 固定扫描 `30011`。

客户端从可用 IPv4 网卡向各自子网广播，并向同子网的记忆地址查询，支持 `10.*`、`172.*`、`192.*` 等地址。为兼容手机厂商不同的 USB 网卡名称，仍枚举 IPv4 网卡，不按名称判断 USB；不使用默认出口扫描或全局广播，不增加 Wi-Fi 专用逻辑。

广播只覆盖同一子网。手机系统、VPN 或防火墙可能阻止发现，扫描不到时可手动连接。旧版服务端不支持自动发现，但音频协议及包大小一致时仍可手动连接。

不同手机的设置按钮可能打开网络共享、无线网络或系统设置首页。应用不能直接开启共享。开启后返回客户端会重新扫描；扫描不到时，手动填写 Windows USB 虚拟网卡的 IPv4 地址。

服务端音频与发现端口均监听 `0.0.0.0`，USB 网卡新增或移除无需重新绑定或重启服务端。通配监听避免 Windows 网卡名称及地址识别逻辑，技术上也接受其他 IPv4 网卡上的请求，不强制屏蔽其他网络。Windows 按路由表选择发送路径。服务端只向一个客户端发送音频，后连接的客户端接管目标；当前不支持 IPv6。

## 配置

### 音频包大小

默认每个 UDP 包包含 96 个立体声 float PCM 帧。每帧 8 字节，采样率表示每秒帧数；48kHz 下每包承载 2ms 音频、每秒 500 包，含项目包头共 808 字节。这不是端到端延时。

两端必须使用相同值：

```powershell
# Windows
cmake -S server -B server/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLANAUDIO_PACKET_FRAMES=192
cmake --build server/build

# Android
.\gradlew.bat :app:assembleDebug '-Planaudio.packetFrames=192'
```

192/240 帧可减少调用次数，但 UDP 数据长度为 1576/1960 字节，超过常见 1500 字节 MTU，可能发生 IP 分片。只有目标手机与 USB 网络实测证明有必要时才改变默认值。

## 设计

### 线程与模块

链路为 WASAPI 采集 → 源时间线分包 → UDP → 有限乱序等待 → 播放队列 → Oboe。采集和发送分线程，接收和播放分线程；跨线程队列严格保持单生产者、单消费者，满队列拒绝新数据，不覆盖正在读取的数据。

服务端控制线程使用 `select` 等待握手和发现请求，优先处理音频握手，每轮最多接收 16 个音频控制包和 8 个发现包，避免突发请求反复进入等待或单个端口独占线程。通过 `FIONREAD` 检查待接收数据，保持音频发送 socket 的阻塞方式和超时。首次握手使用条件变量通知采集线程，移除原先最多 50ms 的轮询等待；发现响应缓存固定字段，只更新查询编号。这些优化针对连接和扫描，不降低播放缓冲或保证端到端延时。

采集先释放 WASAPI 缓冲再整理音频；Oboe 回调独占播放读指针，不执行网络 I/O、内存分配或互斥锁。Android 前台服务持有 CPU WakeLock，以支持息屏播放；不持有 Wi-Fi 锁。

| 模块 | 职责 |
| --- | --- |
| `server/main.cpp` | 服务端启动、握手、退出与采集错误重试 |
| `server/wasapi_capture.cpp` | WASAPI 采集、声道转换、高分辨率发送节拍 |
| `server/packet_sender.h` | 有界发送队列、包头编码与发送线程 |
| `server/discovery_responder.h` | 局域网发现响应，复用控制线程 |
| `shared/discovery_protocol.h` | 独立的发现协议、服务器名称和音频配置 |
| `android/app/src/main/java/local/lanaudio/ServerDiscovery.kt` | 有限时长扫描及发现响应校验 |
| `shared/capture_timeline.h` | 源帧定位、连续分包、静音替代与迟到裁剪 |
| `shared/audio_protocol.h` | v2 包格式、校验与 PCM 解码 |
| `shared/audio_core.h` | 单写单读队列、乱序窗口和滤波器 |
| `shared/playback_buffer.h` | 平台无关的预热、直通、重采样和积压控制 |
| `shared/audio_metrics.h` | 两个客户端音频线程共享的原子统计 |
| `android/app/src/main/cpp/audio_player.h` | Oboe 输出设备生命周期 |
| `android/app/src/main/cpp/receiver.h` | UDP 接收、保活与时间线恢复 |
| `android/app/src/main/cpp/audio_client.cpp` | JNI 入口与客户端会话管理 |

### 连续时间线

服务端按采样率持续分包，对应时段有采集数据就发送真实音频，没有则发送静音，一个包可以包含两者。不能在每次读取为空时追加静音；WASAPI 成批交付音频，错误插入会扩大时间线。

WASAPI 设备帧位置保持连续样本顺序，QPC 时间戳定位首次采集、间断和空闲恢复。采集等待窗口为设备周期与缓冲时长的较大值加一个包时长，启动时打印 `capture_wait_us`，仍需实测验证。时间戳错误时采用估计定位；设备位置与 QPC 的偏差超过窗口时重对齐并计数。

发送节拍使用高分辨率等待定时器，不支持时回退普通定时器，不忙等。晚醒最多补发 20ms，过期时段跳过；迟到采集数据不能追加到已发送的时段。普通空闲与恢复保持会话，设备重建产生新会话。

协议 v2 携带采样率、会话、帧时间戳及由时间戳推导的 32 位包序号。丢弃或发送失败仍推进时间线，序号允许回绕。不使用有损编码、重传、FEC 或额外空闲控制消息。

### 播放与恢复

客户端乱序等待 4-20ms，超时缺包补静音。播放目标固定 100ms，启动与欠载后预热；小幅积压平缓消化，只有超过 150ms 才跳过旧音频回到目标附近。以上是连贯优先的参数，需要真机验证，不是端到端延时保证。

使用 96-tap sinc 异步重采样平缓补偿源与输出设备的时钟偏差，ARM 使用 NEON 并保留标量回退。相同标称采样率、补偿为零且相位对齐时直通，否则采用高质量重采样。平滑队列深度后控制速率，1ms 内误差不调速，最大调整为正负 0.2%；不通过重复或丢弃单帧调速。补偿可能带来轻微音高变化，不是保持音高的时间伸缩。淡入淡出用于启动、欠载和严重积压跳帧边界。

恢复请求携带当时的队列写入位置。回调只释放边界之前的数据，保留后续新音频，读指针不能倒退，多次请求合并到最新边界。

| 情况 | 动作 |
| --- | --- |
| 少量乱序或缺包 | 有限等待，超时补静音 |
| 短时断流导致播放队列不足 | 回调淡出并等待重新预热，保留接收状态 |
| 持续 1 秒无有效新音频、明显序号跳变、时间线异常或同格式新会话 | 重置时间线并重新预热 |
| 输出设备错误或源采样率变化 | 重开 Oboe 并预热 |

恢复清理与计数集中管理，首次连接不算自动恢复。

## 运行统计

客户端每秒更新一次统计。计数在本次连接期间累计。

| 字段 | 含义 |
| --- | --- |
| `packets` | 接受进入重排窗口的数据包数 |
| `lost_packets` | 等待结束后仍缺失并补静音的数据包数 |
| `late_packets` | 播放序号已推进后才到达的数据包数 |
| `duplicates` | 重排窗口内检测到的重复包数 |
| `max_gap_ms` | 相邻数据包的最大到达间隔 |
| `long_gaps` | 到达间隔不小于 20ms 的次数 |
| `queue_drops` | 播放队列满而拒绝写入的数据包数 |
| `active_skips` | 为控制积压主动跳过旧音频的次数 |
| `buffer_ms` | 当前应用播放缓冲，不是端到端延时 |
| `target_ms` | 当前预缓冲目标 |
| `correction_ppm` | 播放速率补偿，正值加快消化、负值放慢；绝对值不超过 2000 |
| `underruns` | 连续缺音区间的次数 |
| `missing_ms` | 因应用队列不足而缺失的音频时长 |
| `dropped_ms` | 溢出、预热或积压控制丢弃的音频时长 |
| `output_hz` | Android 实际输出采样率 |
| `hal_xruns` | Android 音频系统欠载计数 |
| `reconnects` | 自动恢复次数 |
| `stalls` | 持续 1 秒没有可接受新音频后主动恢复的次数 |
| `recv_poll_max_ms` / `recv_long_polls` | 阻塞接收调用返回之间的最大间隔 / 不小于 20ms 的次数，包含等待、线程调度及设备操作 |
| `recv_buf_bytes` | Android 查询得到的内核 UDP 接收缓冲估算值；Linux 将 `SO_RCVBUF` 返回的双倍记账值除以 2 |
| `recv_batches` | 完成的接收批次数；批次以一次阻塞接收开始，随后非阻塞接收最多补收至 16 包 |
| `recv_batch_packets` | 接收批次中有效长度的 UDP 数据报数量，包括协议无效数据报 |
| `recover_output` | Oboe 输出设备错误触发的恢复 |
| `recover_sequence` | 序号超过乱序窗口触发的恢复 |
| `recover_timestamp` | 输出包时间戳不连续触发的恢复 |
| `recover_session` | 服务端切换源会话触发的恢复 |
| `recover_rate` | 源采样率变化触发的恢复 |
| `output_open_failures` | Oboe 输出流打开失败次数，独立于恢复总数 |

`reconnects` 汇总上述五类恢复与 `stalls`，首次连接不计入。不同原因同时发生时按本次执行的恢复动作分类，计数快照可能有短暂更新差异。

服务端每 5 秒输出统计。采集相关计数在每次设备初始化时重置，发送计数在整个进程内累计：

| 字段 | 含义 |
| --- | --- |
| `silence_frames` | 发送时间线中没有对应采集数据而合成的静音帧 |
| `late_capture_frames` | 对应时段已经发送或跳过的迟到采集帧 |
| `capture_overflow_frames` | 超出源时间线容量而拒绝写入的帧 |
| `pacing_skipped_frames` | 调度长延误时跳过的过期帧 |
| `source_resyncs` | 设备位置间断或源时钟偏差超出窗口后的重对齐次数 |
| `timestamp_errors` | WASAPI 时间戳无效而使用估计定位的次数 |
| `send_queue_drops` | 发送队列满而拒绝的数据包 |
| `expired_packets` | 在发送队列中超过 20ms 的过期包 |
| `no_target_packets` | 没有有效客户端而丢弃的包 |
| `send_errors` | socket 发送失败次数 |
| `sent_packets` | socket 成功发送的数据包数，不代表手机已收到 |
| `send_max_gap_ms` / `send_long_gaps` | 同一客户端连续成功发送音频包的最大间隔 / 不小于 20ms 的次数 |

排查卡顿时同时记录服务端日志与手机统计。发送间隔也变长时检查 Windows 采集、调度与发送；发送正常而接收间隔变长时检查网络和 Android。`recv_poll_max_ms` 也很大时可能存在接收线程停顿或长等待；它不等于网络延时，初始化或重开设备也会影响该值。更换客户端或没有有效目标后，发送间隔的观察起点重置，累计计数保留。

## 测试

### 音频核心测试

```powershell
cmake -S tests -B tests/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

测试覆盖并发环形队列、容量保护、乱序、序号回绕、重采样、协议校验、连续发送、迟到裁剪、恢复边界保留新音频，以及无补偿时逐帧直通。新增正负 500ppm 时钟偏差两分钟模拟、30ms 接收暂停与批量补到、小幅积压平缓消化和严重积压上限测试。平台无关测试不能替代设备上的音频输出与网络验证。

环形队列使用最多两段批量复制；NEON 重采样直接加载交错立体声和系数，跨环形边界的四帧组才使用临时数组。保留 96-tap 滤波器、标量回退和相同播放参数。测试包含跨环形边界的重采样一致性。

客户端回调缓存已发布的队列写入边界，只在样本不足时重新读取，仍可使用回调期间到达的新音频。淡入完成后直接输出样本，避免逐帧混合运算及混合舍入导致的小信号损失；欠载淡出和恢复淡入保留。重排队列通过待处理包计数判断是否非空，避免正常收包时遍历 128 个槽位。服务端发送线程复用一次时间读取完成目标有效性与包过期检查。

可运行 `tests/build/audio-core-benchmark.exe`（Visual Studio 构建位于对应配置子目录）测量队列和重采样局部耗时，不设时间阈值作为测试门槛。本机 Windows Release 微基准中，96 帧队列写入、读取和释放的中位耗时从约 25.4ns 降至 11.7ns；这不是整体 CPU 或延时改善值，也不代表 Android NEON 收益，真机结果需单独测量。

微基准还测量正常顺序收包和 96 帧直通写入/播放。本机直通写入/播放中位耗时从约 395ns 降至 123ns；这只是局部 Windows 基准，编译器、设备及实际回调大小会影响结果，不能换算为 Android 整机 CPU 降幅。

### 协议握手测试

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/server_smoke.ps1
```

脚本会在音频 UDP `15010`、发现 UDP `15011` 启动临时服务端，验证发现响应、扫描不启动音频或接管现有连接、协议 v2 握手、保活、连续音频包时间戳及大致发送速率，然后自动结束服务端。它不测量音质或精确调度抖动，也不替代手机上的广播发现测试。

监听验证还包含空包、超长包之后同时发送 32 个发现请求和 32 个保活请求，检查全部响应及后续连续音频。打印的突发请求耗时包含 PowerShell 和回环网络开销，不代表 USB 性能。

### 硬件测试

在目标手机上分别测试：

- 亮屏播放 30 分钟
- 息屏播放 30 分钟
- USB 网络高负载传输期间与结束后播放
- USB 拔线、重新连接并开启共享后恢复
- Windows 默认播放设备切换
- Android 耳机插拔
- USB 自动发现、刷新、手动连接及播放期间发送发现查询

端到端延时应使用同时录制 Windows 原始声音和 Android 输出声音的方式测量。蓝牙耳机通常会增加明显延时。

记录实际延时、卡顿次数、缺音时长和恢复时间，同时观察长间隔、溢出、跳帧与恢复计数。自动测试不替代硬件验收，也不证明没有卡顿。

## 限制

- 当前服务端面向单个客户端，后连接的客户端会接管发送目标。
- UDP 不保证送达；USB 驱动、设备调度或高负载仍可能产生长间隔和丢包。
- 持续发送以 WASAPI 成功初始化为前提；没有可用输出设备或设备重建期间可能中断。服务端等待窗口和定时器精度仍需目标 Windows 设备实测。
- Android 厂商省电策略、音频 HAL 和蓝牙设备会影响息屏稳定性与实际延时。
- 100ms 缓冲无法保证覆盖数百毫秒的网络空窗。时钟补偿只能平缓处理有界偏差与小幅积压，不能制造丢失的音频；严重异常仍可能缺音或跳帧。
- `buffer_ms`、`missing_ms` 和 `dropped_ms` 都是应用层指标，不包含 Windows 音频引擎、网络、Android HAL 和耳机延时。
