# PlatformIO pre-build script: puts the git version (e.g. "fa42d53" or
# "v1.0-3-gfa42d53-dirty") into the firmware as FW_VERSION.
Import("env")
import subprocess

try:
    version = subprocess.check_output(
        ["git", "describe", "--always", "--tags", "--dirty"],
        cwd=env.subst("$PROJECT_DIR"), stderr=subprocess.DEVNULL,
    ).decode().strip()
except Exception:
    version = "unknown"

env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
print("Firmware version: " + version)
