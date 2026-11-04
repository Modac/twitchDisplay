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

# The font.bin this firmware belongs to (fonts/font.bin, built by
# tools/make_fonts.py): its CRC is compared with the font partition's.
import os
import struct
font_bin = os.path.join(env.subst("$PROJECT_DIR"), "fonts", "font.bin")
try:
    with open(font_bin, "rb") as f:
        header = f.read(16)
    font_crc = struct.unpack("<I", header[12:16])[0] if header[:8] == b"TDFONT01" else 0
except OSError:
    font_crc = 0
env.Append(CPPDEFINES=[("FONT_BIN_CRC", "0x%08xUL" % font_crc)])
print("font.bin CRC: %08x" % font_crc)
