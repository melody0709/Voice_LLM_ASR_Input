"""Legacy Portable-package entry point.

The release authority is now build.bat -> CMake/Ninja -> canonical runtime
payload. Keep this small wrapper so existing local automation cannot silently
recreate the retired build/ and release/ packaging path.
"""

from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent


def main() -> int:
    print("package.py is a compatibility wrapper; use build.bat --package-portable directly.", flush=True)
    completed = subprocess.run(
        ["cmd.exe", "/d", "/c", str(ROOT / "build.bat"), "--package-portable"],
        cwd=ROOT,
        check=False,
    )
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
