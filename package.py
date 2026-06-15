"""打包开箱即用的 7z 发布包"""
import subprocess
import datetime
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).parent
BUILD_DIR = ROOT / "build"
MODELS_DIR = ROOT / "models"
OUTPUT_DIR = ROOT / "release"

def get_version():
    """从 resource.h 读取版本号"""
    res_path = ROOT / "src" / "app" / "resource.h"
    major = minor = patch = 0
    for line in res_path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#define APP_VERSION_MAJOR"):
            major = int(line.split()[-1])
        elif line.startswith("#define APP_VERSION_MINOR"):
            minor = int(line.split()[-1])
        elif line.startswith("#define APP_VERSION_PATCH"):
            patch = int(line.split()[-1])
    return f"{major}.{minor}.{patch}"

def main():
    if not BUILD_DIR.exists():
        print(f"错误: {BUILD_DIR} 不存在，请先运行 build.bat")
        sys.exit(1)

    exe = BUILD_DIR / "VoxType.exe"
    if not exe.exists():
        print(f"错误: {exe} 不存在，请先运行 build.bat")
        sys.exit(1)

    version = get_version()
    date_str = datetime.datetime.now().strftime("%Y%m%d")
    archive_name = f"VoxType_v{version}_{date_str}.7z"

    OUTPUT_DIR.mkdir(exist_ok=True)
    archive_path = OUTPUT_DIR / archive_name

    sevenz = shutil.which("7z") or shutil.which("7z.exe")
    if not sevenz:
        print("错误: 未找到 7z 命令，请安装 7-Zip 并添加到 PATH")
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmpdir:
        staging = Path(tmpdir) / f"VoxType_v{version}"
        staging.mkdir()

        print(f"版本: v{version}")
        print(f"正在准备文件...")

        # 1. 主 exe + dll + aria2c
        shutil.copy2(exe, staging / exe.name)
        print(f"  + {exe.name}")
        for f in BUILD_DIR.iterdir():
            if f.suffix == ".dll":
                shutil.copy2(f, staging / f.name)
                print(f"  + {f.name}")
        
        # 复制 aria2c.exe
        aria2c = ROOT / "dll" / "aria2c.exe"
        if aria2c.exists():
            shutil.copy2(aria2c, staging / "aria2c.exe")
            print(f"  + aria2c.exe")

        # 2. 内置 VAD 模型
        models_staging = staging / "models"
        models_staging.mkdir()
        for name in ["silero_vad.int8.onnx", "fireredvad_stream_vad_with_cache.onnx"]:
            src = MODELS_DIR / name
            if src.exists():
                shutil.copy2(src, models_staging / name)
                print(f"  + models/{name}")

        # 3. 下载脚本
        dl_script = ROOT / "download_models.ps1"
        if dl_script.exists():
            shutil.copy2(dl_script, staging / "download_models.ps1")
            print(f"  + download_models.ps1")

        # 4. README + 火山引擎接入指南
        readme = ROOT / "README.md"
        if readme.exists():
            shutil.copy2(readme, staging / "README.md")
            print(f"  + README.md")
        volc_guide = ROOT / "doc" / "volcengine_asr_guide_zh.md"
        if volc_guide.exists():
            shutil.copy2(volc_guide, staging / "volcengine_asr_guide_zh.md")
            print(f"  + volcengine_asr_guide_zh.md")

        # 删除旧包，避免 7z a 追加残留文件
        archive_path.unlink(missing_ok=True)
        # 打包
        print(f"正在打包: {archive_path}")
        cmd = [sevenz, "a", str(archive_path), str(staging)]
        result = subprocess.run(cmd, capture_output=True, text=True)

        if result.returncode != 0:
            print(f"错误: {result.stderr}")
            sys.exit(1)

    size_mb = archive_path.stat().st_size / (1024 * 1024)
    print(f"完成: {archive_path} ({size_mb:.1f} MB)")

if __name__ == "__main__":
    main()
