# sherpa-onnx C++ API 集成研究

## 1. 现状

当前架构：C++ 主进程 → TCP JSON line → Python ASR worker

```
main.cpp
  ↓ SendWorkerJson()  (TCP 127.0.0.1:18088)
asr_worker.py
  ↓ sherpa_onnx.OfflineRecognizer
  ↓ sherpa_onnx.VoiceActivityDetector
  ↓ sherpa_onnx.OfflinePunctuation
模型文件 (.onnx)
```

**痛点**：
- 每次启动需等待 Python 解释器加载（~2-7 秒）
- 需要维护 Python runtime 目录（~800MB+）
- TCP 通信有额外开销
- 两个进程，调试复杂

## 2. sherpa-onnx C++ API 可用性

### 2.1 已有库文件

项目 `runtime/Lib/site-packages/sherpa_onnx/lib/` 下已有：

| 文件 | 说明 |
|------|------|
| `sherpa-onnx-cxx-api.dll` | C++ API 动态库 |
| `sherpa-onnx-cxx-api.lib` | C++ API 导入库 |
| `sherpa-onnx-c-api.dll` | C API 动态库（cxx 依赖） |
| `onnxruntime.dll` | ONNX Runtime |

头文件位置：`runtime/Lib/site-packages/sherpa_onnx/include/sherpa-onnx/c-api/cxx-api.h`

### 2.2 C++ API 结构

```cpp
namespace sherpa_onnx::cxx {

// === 流式 ASR ===
struct OnlineRecognizerConfig { ... };
class OnlineRecognizer {
    static OnlineRecognizer Create(const OnlineRecognizerConfig&);
    OnlineStream CreateStream() const;
    void Decode(const OnlineStream*) const;
    OnlineRecognizerResult GetResult(const OnlineStream*) const;
    bool IsEndpoint(const OnlineStream*) const;
};

// === 非流式 ASR ===
struct OfflineRecognizerConfig { ... };
class OfflineRecognizer {
    static OfflineRecognizer Create(const OfflineRecognizerConfig&);
    OfflineStream CreateStream() const;
    void Decode(const OfflineStream*) const;
    OfflineRecognizerResult GetResult(const OfflineStream*) const;
};

// === VAD ===
struct VadModelConfig { ... };
class VoiceActivityDetector {
    static VoiceActivityDetector Create(const VadModelConfig&, float buffer_size);
    void AcceptWaveform(const float*, int32_t n) const;
    bool IsEmpty() const;
    SpeechSegment Front() const;
    void Pop() const;
    void Flush() const;
    void Reset() const;
};

// === 标点 ===
struct OfflinePunctuationConfig { ... };
class OfflinePunctuation {
    static OfflinePunctuation Create(const OfflinePunctuationConfig&);
    std::string AddPunctuation(const std::string&) const;
};

}  // namespace sherpa_onnx::cxx
```

### 2.3 支持的模型

**非流式 OfflineRecognizer**（当前项目使用的模式）：

| 方法 | 模型类型 | 对应 Python |
|------|---------|-------------|
| `config.model_config.fire_red_asr` | FireRedASR2 AED | `from_fire_red_asr()` |
| `config.model_config.fire_red_asr_ctc` | FireRedASR2 CTC | `from_fire_red_asr_ctc()` |
| `config.model_config.sense_voice` | SenseVoice | `from_sense_voice()` |
| `config.model_config.paraformer` | Paraformer | `from_paraformer()` |
| `config.model_config.whisper` | Whisper | `from_whisper()` |
| ... | 更多 | ... |

**流式 OnlineRecognizer**（未来扩展）：

| 方法 | 模型类型 |
|------|---------|
| `config.model_config.transducer` | Transducer (Zipformer 等) |
| `config.model_config.paraformer` | Paraformer 流式版 |
| `config.model_config.zipformer2_ctc` | Zipformer2 CTC |
| `config.model_config.nemo_ctc` | NeMo CTC |
| `config.model_config.t_one_ctc` | T-One CTC |

**注意**：FireRedASR2 CTC **没有**流式版本（`OnlineRecognizer` 无 `fire_red_asr_ctc` 配置项）。

## 3. 集成方案

### 3.1 CMakeLists.txt 修改

```cmake
cmake_minimum_required(VERSION 3.15)
project(VoxType)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# sherpa-onnx 路径
set(SHERPA_DIR "${CMAKE_SOURCE_DIR}/runtime/Lib/site-packages/sherpa_onnx")

add_executable(VoxType WIN32
    main.cpp
    resources.rc
)

add_compile_definitions(UNICODE _UNICODE)

target_include_directories(VoxType PRIVATE
    "${SHERPA_DIR}/include"
)

target_link_directories(VoxType PRIVATE
    "${SHERPA_DIR}/lib"
)

target_link_libraries(VoxType
    sherpa-onnx-cxx-api
    user32 gdi32 shell32 ole32 comctl32
    d2d1 dwrite shlwapi winmm ws2_32
)
```

### 3.2 代码改动范围

**可删除的函数**（Python 通信相关）：
- `FindPythonExe()` (~line 993)
- `StartWorkerProcess()` (~line 1001)
- `PingWorker()` (~line 988)
- `SendWorkerJson()` (~line 942)
- `WorkerAliveLocked()` (~line 932)
- `CloseWorkerHandlesLocked()` (~line 921)
- `g_workerMutex`, `g_workerProcess`, `g_workerStarted` 全局变量

**需新增的代码**：
- `#include "sherpa-onnx/c-api/cxx-api.h"`
- ASR 引擎初始化/缓存逻辑
- WAV 读取（或用 `sherpa_onnx::cxx::ReadWave()`）
- 识别调用（在后台线程）

**需修改的代码**：
- 识别请求发送逻辑（改为直接调用 C++ API）
- 结果接收逻辑（不再解析 JSON，直接取 `result.text`）

### 3.3 核心调用示例

```cpp
#include "sherpa-onnx/c-api/cxx-api.h"

// 初始化（对应 Python 的 make_recognizer）
sherpa_onnx::cxx::OfflineRecognizerConfig config;
config.model_config.fire_red_asr_ctc.model = L"models/.../model.int8.onnx";
config.model_config.tokens = L"models/.../tokens.txt";
config.model_config.num_threads = 4;

auto recognizer = sherpa_onnx::cxx::OfflineRecognizer::Create(config);

// 识别（对应 Python 的 create_stream → accept_waveform → decode_stream）
auto wave = sherpa_onnx::cxx::ReadWave("last_recording.wav");
auto stream = recognizer.CreateStream();
stream.AcceptWaveform(wave.sample_rate, wave.samples.data(), wave.samples.size());
recognizer.Decode(&stream);
auto result = recognizer.GetResult(&stream);
std::string text = result.text;  // 最终文本
```

### 3.4 VAD 调用示例

```cpp
sherpa_onnx::cxx::VadModelConfig vad_config;
vad_config.silero_vad.model = L"models/silero_vad.int8.onnx";
vad_config.sample_rate = 16000;

auto vad = sherpa_onnx::cxx::VoiceActivityDetector::Create(vad_config, 600);

// 逐块喂入音频
vad.AcceptWaveform(samples.data(), samples.size());
vad.Flush();

while (!vad.IsEmpty()) {
    auto segment = vad.Front();
    // segment.start, segment.samples
    vad.Pop();
}
```

### 3.5 标点调用示例

```cpp
sherpa_onnx::cxx::OfflinePunctuationConfig punct_config;
punct_config.model.ct_transformer = L"models/.../model.int8.onnx";

auto punctuation = sherpa_onnx::cxx::OfflinePunctuation::Create(punct_config);
std::string result = punctuation.AddPunctuation("你好世界");
// → "你好，世界！"
```

## 4. 线程模型

当前 Python worker 是独立进程，C++ 通过 TCP 通信。改为 C++ 后：

```
UI 线程 (main.cpp)
  ↓ 用户松开快捷键
  ↓ PostMessage 到后台线程
后台线程
  ↓ 读取 WAV
  ↓ (可选) VAD 裁剪
  ↓ ASR 识别
  ↓ (可选) 标点后处理
  ↓ PostMessage(kAsrResultMessage) 到 UI 线程
UI 线程
  ↓ 显示结果 / 注入文本
```

sherpa-onnx 的 `OfflineRecognizer` 是线程安全的（内部有锁），可以在后台线程调用。

## 5. 文件部署

运行时需要以下文件与 exe 同目录（或在 PATH 中）：

```
VoxType.exe
sherpa-onnx-cxx-api.dll
sherpa-onnx-c-api.dll
onnxruntime.dll
models/
  sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/
    model.int8.onnx
    tokens.txt
  silero_vad.int8.onnx
  sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/
    model.int8.onnx
```

## 6. 风险与注意事项

| 风险 | 说明 |
|------|------|
| DLL 版本匹配 | `sherpa-onnx-cxx-api.dll` 和 `onnxruntime.dll` 必须版本匹配 |
| 模型路径编码 | C++ API 使用 `std::string`，中文路径需要 UTF-8 编码 |
| 内存占用 | 模型加载后常驻内存，与 Python 行为一致 |
| 初始化耗时 | 首次加载模型仍需几秒，但后续识别无 TCP 开销 |
| 流式支持 | FireRedASR2 CTC 无流式版，如需流式需换模型 |

## 7. 下一步

1. 修改 `CMakeLists.txt` 添加 sherpa-onnx 链接
2. 在 `main.cpp` 中添加 `#include "sherpa-onnx/c-api/cxx-api.h"` 并验证编译
3. 实现 `AsrEngine` 类封装识别逻辑
4. 替换 `SendWorkerJson` 调用为直接 C++ 调用
5. 测试完整流程
