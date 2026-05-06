# FireRedPunc 集成方案

## 目标

将 FireRedPunc 集成到当前项目中，作为 CT-Transformer 标点模型的替代选项。用户可在 Settings 中切换，不影响现有功能。

## 为什么只集成 FireRedPunc

| 组件 | 是否集成 | 原因 |
|------|----------|------|
| FireRedVAD | 否 | Silero VAD 已是 ONNX 最优，提升 1.8% F1 不值得引入 PyTorch |
| FireRedLID | 否 | 对中英混杂场景无提升，FireRedASR2-AED 本身已支持双语 |
| **FireRedPunc** | **是** | 标点质量 62.77% → 78.90% F1，提升 16%，推理只多 ~6ms |

## 标点模型对比

| | CT-Transformer (ONNX) | FireRedPunc (PyTorch) |
|---|---|---|
| **中文 F1** | 62.77% | 78.90% |
| **英文 F1** | 49.91% | 74.83% |
| **模型大小** | 72 MB | ~1.29 GB (model + BERT tokenizer) |
| **推理框架** | ONNX Runtime | PyTorch + Transformers |
| **推理速度** | ~4ms | ~10ms |
| **加载速度** | ~50ms | ~2-3s（首次，后续缓存） |
| **常驻内存** | ~100 MB | ~500 MB |

## 架构设计

### 流程

```
录音 -> Silero VAD -> FireRedASR2-AED -> [标点] -> 文本注入
                                              ^
                                              |
                                    CT-Transformer (默认)
                                    或 FireRedPunc (可选)
```

### Worker 协议扩展

`recognize` 请求新增字段：

```json
{
  "cmd": "recognize",
  "model_id": "firered_aed",
  "model_dir": "...",
  "threads": "4",
  "enable_vad": true,
  "postprocess": "itn",
  "punc_engine": "firered",
  "firered_punc_dir": "models/FireRedPunc",
  "wav": "..."
}
```

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `punc_engine` | string | `"ct_transformer"` | `"ct_transformer"` 或 `"firered"` |
| `firered_punc_dir` | string | `""` | FireRedPunc 模型目录 |

### Worker 响应扩展

```json
{
  "ok": true,
  "text": "你好世界。",
  "model_id": "firered_aed",
  "load_ms": 0,
  "decode_ms": 850,
  "punc_engine": "firered",
  "punct_ms": 10,
  "punct_loaded": true,
  "postprocess": "punctuation"
}
```

## 配置变更

### config.json

```json
{
  "model_id": "firered_aed",
  "model_dir": "...",
  "threads": "auto",
  "enable_vad": true,
  "enable_partial": true,
  "postprocess": "itn",
  "punc_engine": "ct_transformer",
  "firered_punc_dir": "",
  "hotkey": "CapsLock"
}
```

### C++ Config 结构

```cpp
struct Config {
    // 现有字段
    std::wstring modelId = L"firered_aed";
    std::wstring modelDir;
    std::wstring threads = L"auto";
    bool enableVad = true;
    bool enablePartial = true;
    std::wstring postprocess = L"itn";
    std::wstring hotkey = L"CapsLock";

    // 新增
    std::wstring puncEngine = L"ct_transformer"; // "ct_transformer" | "firered"
    std::wstring fireredPuncDir;
};
```

## Settings UI 变更

### Recognition Tab 布局

当前：
```
82  ASR model     [ComboBox]
134 Model folder  [Edit] [Browse]
186 Threads       [ComboBox]  [x] Enable VAD  [x] Partial result
238 Postprocess   [ComboBox]
```

新布局：
```
82  ASR model       [ComboBox]
134 Model folder    [Edit] [Browse]
186 Threads         [ComboBox]  [x] Enable VAD  [x] Partial result
238 Postprocess     [ComboBox]
286 Punc engine     [CT-Transformer ▼]
```

### 新增控件

```cpp
constexpr int IDC_PUNC_ENGINE = 2013;  // 标点引擎选择
```

- **Punc engine** ComboBox：`"CT-Transformer"` / `"FireRed Punc"`

当 `postprocess` 为 `"none"` 时，Punc engine 下拉框灰显。

## Worker 实现

### 新增依赖

```python
# 延迟导入，只在启用 FireRedPunc 时加载
try:
    import torch
    from transformers import AutoTokenizer
    PYTORCH_AVAILABLE = True
except ImportError:
    PYTORCH_AVAILABLE = False
```

### FireRedPuncState 类

```python
class FireRedPuncState:
    def __init__(self):
        self.lock = threading.Lock()
        self.key = None
        self.model = None
        self.tokenizer = None

    def clear(self):
        with self.lock:
            self.key = None
            self.model = None
            self.tokenizer = None

    def add_punctuation(self, text, model_dir, threads):
        if not text:
            return text, 0, False, ""

        key = os.path.abspath(model_dir)
        loaded = False

        with self.lock:
            if self.key != key or self.model is None:
                self.model = load_firered_punc(model_dir)
                self.tokenizer = load_firered_punc_tokenizer(model_dir)
                self.key = key
                loaded = True

            started = time.perf_counter()
            result = self.model.process([text])
            punct_ms = int((time.perf_counter() - started) * 1000)
            text = result[0]["punc_text"]

        return text, punct_ms, loaded, ""
```

### recognize 流程变更

```python
# 现有标点处理
postprocess = request.get("postprocess", "itn")
if response.get("ok") and postprocess in {"itn", "punct", "llm"}:
    punc_engine = request.get("punc_engine", "ct_transformer")

    if punc_engine == "firered":
        text, punct_ms, punct_loaded, warning = FIRERED_PUNC.add_punctuation(
            response.get("text", ""),
            request.get("firered_punc_dir", ""),
            request.get("threads", "auto"),
        )
    else:
        text, punct_ms, punct_loaded, warning = PUNCT.add_punctuation(
            response.get("text", ""),
            request.get("threads", "auto"),
            request.get("punctuation_model", ""),
        )

    response["text"] = text
    response["punc_engine"] = punc_engine
    response["postprocess"] = "punctuation"
    response["punct_ms"] = punct_ms
    response["punct_loaded"] = punct_loaded
```

## 模型文件结构

```
models/
  FireRedPunc/
    model.pth.tar                # ~407 MB，标点模型权重
    chinese-lert-base/           # ~890 MB，BERT tokenizer
      config.json
      tokenizer.json
      vocab.txt
      ...
    chinese-bert-wwm-ext_vocab.txt  # ~110 KB
    out_dict                     # ~33 B
```

## Python 嵌入式 Runtime 方案

### 目标

将 Python 解释器和所有依赖打包到 `runtime/` 目录，用户解压即用，不需要自己装 Python 和 pip install。

### 目录结构

```
VoxType/
  build/
    VoxType.exe
  runtime/                          # 嵌入式 Python，不提交 git
    python.exe                      # Python 3.10 embeddable
    python310.zip
    python310.dll
    Lib/
      site-packages/
        sherpa_onnx/                # ASR 引擎
        numpy/                      # 数值计算
        torch/                      # PyTorch CPU-only (~200 MB)
        transformers/               # HuggingFace Transformers
        tokenizers/
        safetensors/
        huggingface_hub/
        sentencepiece/
        requests/
        ...
    Scripts/
      pip.exe                       # 用于后续更新依赖
    python310._pth                  # 控制 import 路径
  models/                           # 不打包，用户单独下载
    sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26/
    sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/
    silero_vad.int8.onnx
    sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/
    FireRedPunc/                    # 可选，用户需要 FireRedPunc 时下载
  asr_worker.py
  build.bat
```

### 体积估算

| 组件 | 大小 |
|------|------|
| Python 3.10 embeddable | ~30 MB |
| sherpa-onnx | ~50 MB |
| numpy | ~30 MB |
| torch (CPU-only) | ~200 MB |
| transformers + 依赖 | ~80 MB |
| 其他 (requests, sentencepiece 等) | ~30 MB |
| **runtime 总计** | **~420 MB** |

### Worker 启动逻辑变更

`main.cpp` 中 `StartWorkerProcess()` 修改：

```cpp
// 当前
L"python " + QuoteArg(script)

// 改为：优先 runtime/python.exe，fallback 系统 python
std::wstring pythonExe = AppRootDir() + L"\\runtime\\python.exe";
if (GetFileAttributesW(pythonExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    pythonExe = L"python";  // fallback
}
pythonExe + L" " + QuoteArg(script)
```

### python310._pth 配置

嵌入式 Python 需要配置 import 路径：

```
python310.zip
Lib/site-packages
.
```

确保 `sherpa_onnx`、`torch`、`transformers` 等都能被 import。

### 构建脚本 setup_runtime.ps1

```powershell
# 1. 下载 Python embeddable
$url = "https://www.python.org/ftp/python/3.10.11/python-3.10.11-embed-amd64.zip"
Invoke-WebRequest $url -OutFile python-embed.zip
Expand-Archive python-embed.zip -DestinationPath runtime/
Remove-Item python-embed.zip

# 2. 安装 pip
Invoke-WebRequest https://bootstrap.pypa.io/get-pip.py -OutFile runtime/get-pip.py
runtime/python.exe runtime/get-pip.py --no-warn-script-location
Remove-Item runtime/get-pip.py

# 3. 配置 ._pth（取消注释 import site）
(Get-Content runtime/python310._pth) -replace '#import site', 'import site' |
    Set-Content runtime/python310._pth

# 4. 安装依赖
runtime/python.exe -m pip install --no-cache-dir `
    sherpa-onnx numpy `
    torch --index-url https://download.pytorch.org/whl/cpu `
    transformers sentencepiece

# 5. 清理 pip 缓存
runtime/python.exe -m pip cache purge
Remove-Item -Recurse -Force runtime/pip -ErrorAction SilentlyContinue
```

### 传播方式

```
VoxType-v0.2.0.zip        # ~450 MB（不含模型）
  ├── build/VoxType.exe
  ├── runtime/
  ├── asr_worker.py
  └── README.md

用户操作：
1. 解压 zip
2. 下载模型到 models/（或运行下载脚本）
3. 运行 exe
```

### 注意事项

- `runtime/` 不提交 git（加入 `.gitignore`）
- `models/` 不提交 git（已有）
- `setup_runtime.ps1` 提交 git，方便开发者构建
- `build.bat` 不变，只编译 C++ 部分
- Python 版本选 3.10：sherpa-onnx、PyTorch、transformers 都稳定支持

## 实施步骤

1. **`asr_worker.py`**：新增 `FireRedPuncState` 类和 `FIRERED_PUNC` 全局实例
2. **`asr_worker.py`**：修改 `recognize` 流程，增加 `punc_engine` 分支
3. **`main.cpp`**：`Config` 增加 `puncEngine` / `fireredPuncDir` 字段
4. **`main.cpp`**：Settings 增加 Punc engine 下拉框
5. **`main.cpp`**：更新 `LoadConfig()` / `SaveConfig()` / `BuildRecognizeRequest()`
6. 测试 FireRedPunc vs CT-Transformer 的标点质量和耗时

## 风险与对策

| 风险 | 对策 |
|------|------|
| PyTorch 未安装 | `PYTORCH_AVAILABLE=False` 时，Settings 中 FireRed Punc 选项灰显 |
| transformers 未安装 | 同上，延迟导入，失败时 fallback 到 CT-Transformer |
| 首次加载慢（~2-3s） | HUD 显示 "Loading punc model..."，后续缓存不重复加载 |
| 内存增加 ~500 MB | 只在用户显式选择 FireRedPunc 时才加载，不选不占内存 |
| 模型目录不存在 | Worker 返回明确错误，HUD 显示 "FireRedPunc model not found" |

## 向后兼容性

- `punc_engine` 默认 `"ct_transformer"`，现有配置无需修改
- `firered_punc_dir` 默认 `""`，不影响现有行为
- PyTorch 未安装时功能完全不受影响，只是 FireRedPunc 选项不可用
