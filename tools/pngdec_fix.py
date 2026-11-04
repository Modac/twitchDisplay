# PlatformIO pre-build script for the PNGdec library:
# - its ESP32-S3 SIMD file (s3_simd_rgb565.S) does not build on other chips
#   (the ESP32-C3 has no ESP-DSP headers), so it is left out there
# - its bundled zlib triggers harmless -Wimplicit-fallthrough warnings
Import("env")

mcu = env.BoardConfig().get("build.mcu", "")

def pngdec_sources(env, node):
    path = node.get_path().replace("\\", "/")
    if "/PNGdec/" not in path:
        return node
    if path.endswith("s3_simd_rgb565.S") and mcu != "esp32s3":
        return None
    return env.Object(node, CCFLAGS=env["CCFLAGS"] + ["-Wno-implicit-fallthrough"])

env.AddBuildMiddleware(pngdec_sources)
