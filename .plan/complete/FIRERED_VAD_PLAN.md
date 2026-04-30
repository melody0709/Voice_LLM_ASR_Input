# FireRedVAD 接入计划 (已完成 v0.1.5)

## 背景

当前 VAD 使用 Silero VAD（sherpa-onnx 内置）。FireRedVAD 由小红书团队开源，准确率更高：

| 指标 | FireRedVAD | Silero VAD |
|------|-----------|-----------|
| AUC-ROC | **99.60** | 97.97 |
| F1 | **97.57** | 95.95 |
| 误报率 | **2.69%** | 9.41% |
| 漏检率 | 3.62% | 3.95% |

模型仅 2.2MB（float32），~588K 参数。提供 ONNX 格式，可直接用 onnxruntime 加载。

## 技术要点

### ONNX 模型输入不是原始 PCM

FireRedVAD 的 pipeline：

```
原始 PCM (16kHz int16)
  → 80 维 fbank 特征 (25ms 窗, 10ms 帧移)
  → CMVN 归一化 (减均值 × 逆标准差)
  → ONNX 模型 → sigmoid → 语音概率
```

模型输入 shape: `[batch, num_frames, 80]`（fbank 特征）

### 流式推理

DFSMN 模型有 R 个缓存张量（lookback convolution 历史帧），每帧更新。需要管理缓存状态。

### ONNX 模型文件

GitHub 仓库 `pretrained_models/onnx_models/` 目录下：

| 文件 | 用途 |
|------|------|
| `fireredvad_stream_vad.onnx` | 流式 VAD（本项目使用） |
| `fireredvad_stream_vad_with_cache.onnx` | 流式 VAD（带缓存） |
| `fireredvad_vad.onnx` | 非流式 VAD |
| `fireredvad_aed.onnx` | 音频事件检测 |

配套文件：`cmvn.ark`（CMVN 归一化参数）

## 架构设计

新建独立模块 `firered_vad.h`，main.cpp 改动最小化：

```
main.cpp (AsrEngine)
  ├── Silero VAD  ← sherpa-onnx VoiceActivityDetector（现有，不动）
  └── FireRed VAD ← firered_vad.h（新增，用 onnxruntime 直接加载）
```

## 新文件：`firered_vad.h`

header-only 模块，包含三个组件：

### 1. FbankExtractor — 80 维 fbank 特征提取

手写实现，零外部依赖：

- FFT：Cooley-Tukey 基 2 FFT
- Mel 滤波器组：80 个三角滤波器，mel 频率范围 0 ~ 8000Hz
- 参数：25ms 窗口（400 samples），10ms 帧移（160 samples），80 mel bins
- 输入：原始 PCM float samples
- 输出：`[num_frames, 80]` 特征矩阵

### 2. CmvnNormalizer — CMVN 归一化

- 解析 Kaldi `cmvn.ark` 格式，或直接硬编码均值/方差数组
- 操作：`(x - mean) * inv_std`
- 80 维向量

### 3. FireRedVad — 主类

用 onnxruntime C API 加载 `fireredvad_stream_vad.onnx`：

```cpp
struct FireRedVadConfig {
    std::string model_path;   // fireredvad_stream_vad.onnx 路径
    std::string cmvn_path;    // cmvn.ark 路径
    float threshold = 0.5f;   // 语音概率阈值
};

class FireRedVad {
public:
    static std::unique_ptr<FireRedVad> Create(const FireRedVadConfig& cfg);
    void Reset();
    // 输入原始 PCM float，返回是否有语音
    bool Process(const float* samples, int32_t n);
private:
    FbankExtractor fbank_;
    CmvnNormalizer cmvn_;
    OrtSession* session_;
    // DFSMN 流式缓存（R 个张量）
    std::vector<OrtValue*> caches_;
    float threshold_;
};
```

## main.cpp 改动

### 1. Config 新增字段

```cpp
std::wstring vadModel = L"silero";  // "silero" | "firered"
```

### 2. AsrEngine 新增成员

```cpp
std::unique_ptr<FireRedVad> fireRedVad;
std::string fireRedVadKey;
bool EnsureFireRedVad(int threads);
```

### 3. Recognize() VAD 分支

```cpp
if (config.enableVad && workSamples.size() > 0) {
    if (config.vadModel == L"firered") {
        if (EnsureFireRedVad(threads)) {
            fireRedVad->Reset();
            bool hasSpeech = fireRedVad->Process(workSamples.data(), workSamples.size());
            if (!hasSpeech) return L"";
        }
    } else {
        // 现有 Silero VAD 逻辑，不动
    }
}
```

### 4. Settings UI

在 Punctuation 上方新增一行：

```
VAD model:      [Silero VAD      ▾]
```

### 5. config.json

```json
{
  "vad_model": "silero"
}
```

## build.bat 改动

添加 onnxruntime 头文件 include 路径：

```bat
set "ONNX_INCLUDE=%~dp0third_party\onnxruntime\include"
cl ... /I"%ONNX_INCLUDE%" ...
```

onnxruntime DLL 已随 sherpa-onnx 附带，无需额外复制。

## 需要的外部文件

| 文件 | 来源 | 放置位置 |
|------|------|---------|
| `fireredvad_stream_vad.onnx` | GitHub `FireRedTeam/FireRedVAD` | `models/` |
| `cmvn.ark` | 同上 | `models/` |
| `onnxruntime_c_api.h` | onnxruntime 官方发布 | `third_party/onnxruntime/include/` |

## 文件清单

| 文件 | 动作 | 说明 |
|------|------|------|
| `firered_vad.h` | **新建** | FireRedVAD 模块（fbank + CMVN + ONNX 推理 + 流式缓存） |
| `main.cpp` | 小改 | Config 加 `vadModel`，AsrEngine 加分支，Settings 加下拉框 |
| `build.bat` | 小改 | 加 onnxruntime include 路径 |
| `CMakeLists.txt` | 小改 | 同上 |

## 验证

1. `.\build.bat` 编译通过
2. Settings 中出现 VAD 模型下拉框，可切换 Silero / FireRed
3. 选择 FireRed VAD 后录音测试，HUD 显示正常、识别结果正确
4. 切换回 Silero VAD 确认仍正常工作
5. `config.json` 中 `vad_model` 字段正确保存和加载

## 参考资料

- FireRedVAD 仓库: https://github.com/FireRedTeam/FireRedVAD
- HuggingFace 模型: https://huggingface.co/FireRedTeam/FireRedVAD
- NCNN 流式实现: https://github.com/lhwcv/FireRedVAD-NCNN-streaming
- 论文: https://arxiv.org/pdf/2603.10420
