# LanAudio

将 Windows 系统音频通过局域网发送到 Android。音频优先保证连续播放，再限制延时，最后保持音质。

当前版本：`1.2.6`

音频协议：`v2`

音频端口：`30010`

发现端口：`30011`
默认包大小：每包 `96` 帧，双声道 float PCM

详细开发规则见 [AGENTS.md](AGENTS.md)。

## 快速使用

### 1. 构建 Windows 服务端

需要 CMake、Ninja 和 MinGW 或 Visual Studio C++ 工具链。推荐使用 Ninja：

```powershell
cmake -S server -B server/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build server/build
```

生成文件：

```text
server/build/lan-audio-server.exe
```

这是唯一需要复制到其他 Windows 电脑的发布文件。服务端要求管理员权限，双击时 Windows 会显示一次 UAC 确认。

### 2. 构建 Android 客户端

使用 Android Studio 打开 `android` 目录，执行 Gradle Sync，然后构建 `app` 模块。需要 Android SDK 35、NDK、CMake 3.22.1 和 JDK 17 或更高版本。

也可以执行：

```powershell
cd android
.\gradlew.bat :app:assembleDebug
```

APK 位于：

```text
android/app/build/outputs/apk/debug/app-debug.apk
```

### 3. 连接播放

1. 手机开启 USB 网络共享。也可以使用 Wi-Fi 或以太网。
2. 启动 `server/build/lan-audio-server.exe`，并确认 UAC。
3. 安装并打开 Android APK。
4. 客户端扫描服务器，扫描结束后自动连接优先级最高的兼容服务器，优先使用 USB/RNDIS 网络。
5. 扫描不到服务器时可手动输入 Windows IPv4 地址和端口后连接。

服务端启动时会自动添加 UDP `30010` 和 `30011` 防火墙规则。若系统策略阻止添加，请在 Windows 防火墙中手动放行这两个端口。USB 网卡通常属于公用网络。

客户端连接后，服务端会静音电脑本地默认输出，只在手机播放。服务端退出时恢复原来的静音状态。

服务端托盘菜单提供：

- 暂停/继续播放
- 开机自启动
- 退出服务端

## 网络与端口

服务端监听所有 IPv4 网卡，因此 USB、Wi-Fi 和以太网可以同时使用。客户端按 USB/RNDIS、其他有线网络、Wi-Fi 的顺序展示发现结果。

可以用启动参数覆盖端口：

```powershell
lan-audio-server.exe 30010 30011
```

第一个参数是音频端口，第二个参数是发现端口。两个端口必须不同。

## 音频参数

默认采样率由 Windows 播放设备决定，服务端发送双声道 float PCM。默认每包 96 帧；48kHz 时每包约 2ms 音频，包含协议头约 808 字节。

服务端和 Android 必须使用相同的包大小。只有实测证明有必要时才改为 192 或 240 帧，因为较大的 UDP 包可能超过 MTU 并产生 IP 分片。

Android 客户端的“高级设置”中可以输入任意毫秒数，默认值为 `64ms`。设置会立即提交给播放队列，在音频回调的安全边界应用，不需要断开连接；目标改变时只重置速率补偿，不清空已经接收的音频。实际范围为 `1-149ms`，USB 稳定且追求低延时时可尝试较小值，出现欠载或缺音时应增大缓冲。

## 运行统计

Android 界面中的主要统计：

| 字段 | 含义 |
| --- | --- |
| `packets` | 接收的有效音频包数 |
| `lost_packets` | 等待结束后仍缺失的包数 |
| `late_packets` | 播放位置推进后才到达的包数 |
| `queue_drops` | 播放队列已满而丢弃的包数 |
| `active_skips` | 为限制积压主动跳过的次数 |
| `buffer_ms` | 应用播放缓冲，不是端到端延时 |
| `underruns` | 缺音区间次数 |
| `missing_ms` | 队列不足造成的缺音时长 |
| `dropped_ms` | 溢出或积压控制丢弃的时长 |
| `correction_ppm` | 播放速率补偿，范围不超过正负 2000ppm |

服务端每 5 秒输出采集、发送和队列统计。`sent_packets` 只表示 Windows 成功调用发送，不代表手机已经收到。

## 测试

运行音频核心测试：

```powershell
cmake -S tests -B tests/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

运行服务端发现、握手和连续发送测试：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/server_smoke.ps1
```

硬件验收至少覆盖亮屏、息屏、USB 网络高负载、Wi-Fi、拔线重连、输出设备切换和 Android 耳机插拔。端到端延时必须通过同时录制 Windows 原始声音和 Android 输出声音测量，不能用 `buffer_ms` 代替。

## 目录

| 目录 | 内容 |
| --- | --- |
| `server` | Windows WASAPI 服务端源码和发布配置 |
| `android` | Android 客户端源码 |
| `shared` | 两端共用的协议、队列和时间线代码 |
| `tests` | 音频核心测试和服务端冒烟测试 |
| `AGENTS.md` | 设计、性能和验证规则 |

构建生成物只放在各自的 `build` 目录中，不作为源码发布物提交。
