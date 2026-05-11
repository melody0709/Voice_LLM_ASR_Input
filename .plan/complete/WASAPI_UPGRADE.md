# WASAPI Shared Mode 录音升级方案

## 目标

将 VoxType 录音链路从 MME waveIn 升级到 WASAPI Shared Mode + 自定义重采样，
获得更低延迟、设备选择能力和可控的采样率转换质量。

## 当前链路

```
vox_mic 48kHz → WASAPI render → VB-CABLE → Windows 混音器(自动降采样) → MME waveIn 16kHz → VoxType
```

**问题：**
- MME waveIn 延迟高（4×100ms 缓冲区 = 400ms）
- Windows 混音器降采样算法不可控，可能引入失真
- `WAVE_MAPPER` 只能用默认设备，无法选择麦克风
- 无法获取设备原生采样率

## 目标链路

```
vox_mic 48kHz → WASAPI render → VB-CABLE → WASAPI capture(系统混合格式,通常48kHz/f32) → 自定义重采样 → 16kHz s16le → VoxType
```

## 架构设计

### 新增文件

| 文件 | 职责 |
|---|---|
| `src/wasapi_capture.h/cpp` | WASAPI Shared Mode 捕获模块 |
| `src/resampler.h/cpp` | 高质量重采样（线性插值 / speexdsp） |

### 修改文件

| 文件 | 改动 |
|---|---|
| `src/globals.h` | Config 增加 `audioDeviceId`、`audioBackend` 字段 |
| `src/engine.cpp` | `StartAudioCapture` / `StopAudioCapture` 改为调用 WASAPI |
| `src/engine.h` | 增加设备枚举函数声明 |
| `src/settings.cpp` | 增加音频设备选择下拉框 |
| `build.bat` | 链接 `ole32.lib`、`mmdevapi.lib`（已有）、`ksuser.lib` |

### 类设计

```cpp
// wasapi_capture.h
class WasapiCapture {
public:
    bool Init(const std::wstring& deviceId = L"");  // 空=默认设备
    bool Start(std::wstring& error);
    void Stop();
    std::vector<BYTE> GetRecordedPcm();  // 返回 16kHz s16le mono

    struct DeviceInfo {
        std::wstring id;
        std::wstring name;
        bool isDefault;
    };
    static std::vector<DeviceInfo> EnumerateDevices();

    UINT32 GetNativeSampleRate() const;  // 设备原生采样率
    UINT32 GetNativeChannels() const;

private:
    void CaptureThread();
    void ResampleAndAppend(const BYTE* src, UINT32 frames);

    IMMDeviceEnumerator* m_enumerator{nullptr};
    IMMDevice* m_device{nullptr};
    IAudioClient* m_audioClient{nullptr};
    IAudioCaptureClient* m_captureClient{nullptr};
    WAVEFORMATEX* m_mixFormat{nullptr};
    UINT32 m_bufferFrames{0};

    // 重采样状态
    double m_resampleRatio{1.0};
    double m_resamplePhase{0.0};

    // 输出缓冲区
    std::vector<BYTE> m_pcmBuffer;
    CRITICAL_SECTION m_bufferLock;

    std::thread m_captureThread;
    std::atomic<bool> m_running{false};
    HANDLE m_event{nullptr};
};
```

## 实现步骤

### Phase 1：核心 WASAPI 捕获（最小可用）

**1.1 设备枚举**

```cpp
// 使用 eCapture + eConsole 枚举录音设备
IMMDeviceEnumerator → EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE)
遍历获取 id + friendly name
默认设备：GetDefaultAudioEndpoint(eCapture, eConsole)
```

参考 vox_mic `device_enum.h` 的模式，但改为 eCapture（录音端点）。

**1.2 Shared Mode 初始化**

```cpp
// 1. Activate → IAudioClient
// 2. GetMixFormat() → 获取系统混合格式（通常 48000Hz / 2ch / 32bit float）
// 3. Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, ...)
//    缓冲区：50ms（比 vox_mic 的 10ms 宽裕，因为录音不急）
// 4. GetService → IAudioCaptureClient
// 5. SetEventHandle + Start
```

**1.3 捕获循环**

```cpp
// 事件驱动，WaitForSingleObject 等待新数据
// GetBuffer → 获取 float32/stereo 原始帧
// 转 mono float（如果是立体声，取平均或只取左声道）
// 重采样到 16kHz
// 转 int16 写入 m_pcmBuffer
// ReleaseBuffer
```

**1.4 重采样（Phase 1 用线性插值）**

```cpp
// 与 vox_mic wasapi_output.cpp:206-221 相同的算法
// 48kHz → 16kHz，ratio = 3.0，每3个源样本插值出1个目标样本
// 线性插值质量足够 ASR 使用，CPU 开销几乎为零
```

**1.5 接入 engine.cpp**

```cpp
// StartAudioCapture 改为：
if (g_wasapiCapture.Init()) {
    g_wasapiCapture.Start(error);
} else {
    // fallback 到 waveIn
    waveInOpen(...);
}

// StopAudioCapture 改为：
if (g_wasapiCapture) {
    return g_wasapiCapture.GetRecordedPcm();
} else {
    // 原 waveIn 逻辑
}
```

### Phase 2：设备选择 UI

**2.1 Config 增加字段**

```cpp
// globals.h Config
std::wstring audioBackend = L"wasapi";   // "wasapi" | "wavein"
std::wstring audioDeviceId;              // WASAPI 设备 ID，空=默认
```

**2.2 Settings UI**

在 Settings 对话框增加 "Audio" 分组：
- 下拉框：音频后端（WASAPI / waveIn Legacy）
- 下拉框：录音设备（从 `WasapiCapture::EnumerateDevices()` 填充）
- 显示当前设备原生采样率（只读信息）

**2.3 配置持久化**

`engine.cpp` 的 `LoadConfig` / `SaveConfig` 增加 `audioBackend` 和 `audioDeviceId`。

### Phase 3：质量优化（可选）

**3.1 高质量重采样**

如果线性插值不够，引入 speexdsp：
- `third_party/speexdsp/include/speex/speex_resampler.h`
- `third_party/speexdsp/lib/libspeexdsp.lib`
- 质量设为 `SPEEX_RESAMPLER_QUALITY_VOIP`(3)，平衡质量和性能

**3.2 音量计算优化**

当前 `CalculateAudioLevel` 在 `WaveInProc` 里对 int16 计算 RMS。
WASAPI 捕获到的是 float32，需要新增 `CalculateAudioLevelFloat` 版本。

## 需要新增的库依赖

| 库 | 用途 | 状态 |
|---|---|---|
| `ole32.lib` | COM 初始化 | build.bat 已有 |
| `mmdevapi.lib` | MMDevice 枚举 | 需确认 |
| `ksuser.lib` | WASAPI 入口 | 需新增 |
| `speexdsp`（可选） | 高质量重采样 | Phase 3 |

实际上 WASAPI 的函数在 `mmdevapi.lib` 和 `ole32.lib` 中，当前 build.bat 已链接 `ole32.lib`。
只需确认 `mmdevapi.lib` 是否需要显式链接（Windows SDK 通常通过 `#pragma comment` 处理）。

## 风险和降级策略

| 风险 | 降级方案 |
|---|---|
| WASAPI 初始化失败（权限/驱动） | 自动 fallback 到 waveIn，HUD 提示 |
| Shared Mode 不支持某些旧设备 | 检测 `IsFormatSupported` 失败后 fallback |
| 捕获线程阻塞导致音频丢失 | 设置合理超时，超时后跳过该块 |
| COM 线程模型冲突 | 使用 `COINIT_MULTITHREADED`，与现有代码一致 |

## 验收标准

- [x] 默认麦克风通过 WASAPI 捕获，音频质量不劣于 waveIn
- [ ] 设备切换后自动跟随，或在 Settings 中可手动选择（Phase 2）
- [x] WASAPI 失败时自动 fallback 到 waveIn，无感知
- [x] 15s 录音体积仍为 ~475KB（16kHz/16bit/mono PCM）
- [x] 识别结果不劣于当前 waveIn 链路
- [x] 录音启动延迟 ≤ waveIn（目标 <100ms）

## 实现状态

### Phase 1：已完成

**新增文件：**
- `src/wasapi_capture.h` — WasapiCapture 类 + WasapiDeviceInfo 结构体
- `src/wasapi_capture.cpp` — WASAPI Shared Mode 捕获 + 线性插值重采样

**修改文件：**
- `src/globals.h` — Config 增加 `audioBackend`/`audioDeviceId`，extern `g_wasapiCapture`
- `src/engine.cpp` — `StartAudioCapture` WASAPI 优先 + waveIn fallback；`StopAudioCapture` 双路径；LoadConfig/SaveConfig 新增字段
- `src/main.cpp` — 定义 `g_wasapiCapture` 全局实例；录音时保存后端信息；DebugPrintHeader 显示音频后端
- `build.bat` — 增加 `wasapi_capture.cpp` 和 `mmdevapi.lib`
- `CMakeLists.txt` — 同步

**生命周期：** `Init()` → `Start()` → `Stop()` → `Release()`（类似 waveIn 的 Open → Start → Stop → Close）

**踩坑记录：**
1. `Stop()` 只停线程不清资源，必须有 `Release()` 才能重新 `Init()`，否则第二次录音卡死
2. 重采样相位 `m_resamplePhase -= numFrames` 对非整数比采样率会越界，改为 `m_resamplePhase -= written / m_resampleRatio`
3. `EnumerateDevices` 的 COM 清理条件不能用被覆盖的 `hr`，需单独保存 `hrInit`
4. `Init()` 成功但 `Start()` 失败时必须 `Release()`，否则 WASAPI 和 waveIn 资源同时持有

### Phase 2：待实现

Settings UI 音频设备选择下拉框

## 参考代码

- vox_mic `wasapi_output.cpp` — WASAPI 初始化和事件驱动循环
- vox_mic `device_enum.cpp` — 设备枚举
- Microsoft WASAPI capture 示例 — `IAudioCaptureClient::GetBuffer` 用法

## 依赖关系

- 此任务是 streaming ASR（OPTIMIZATION_PLAN #8）的前置
- 完成后可直接在捕获循环中按 chunk 送入 streaming recognizer
- 与性能日志（#3）独立，可并行开发
