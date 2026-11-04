#!/usr/bin/env python3
"""Checks lib/LogoImage (pngToLogo) against a Pillow reference.

Generates test PNGs (all colour types, every row filter, image data split
into many IDAT chunks, transparency, broken and unsupported files), converts
them with the real C++ code (logo_test.cpp) and compares every logo with an
exact area-average reference computed from the same RGB565 values. Allowed
difference: 1 RGB565 step (rounding).

Needs: g++, zlib, Python with numpy and Pillow (pip install numpy pillow).
Run: python3 logo_quality.py
"""
import os, struct, subprocess, sys, tempfile, zlib
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
rng = np.random.default_rng(1)

def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

def write_png(path, pixels, colortype, idat_size=8192, extra=b'', depth=8, interlace=0, filters=True):
    """PNG writer that cycles through all five row filters"""
    h, w = pixels.shape[:2]; bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colortype]
    rows = pixels.reshape(h, -1).astype(int); raw = b''; prev = np.zeros(rows.shape[1], int)
    for y in range(h):
        f = y % 5 if filters else 0; cur = rows[y]; out = []
        for i in range(len(cur)):
            a = cur[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            out.append((cur[i] - [0, a, b, (a + b) >> 1, paeth(a, b, c)][f]) & 255)
        raw += bytes([f]) + bytes(out); prev = cur
    z = zlib.compress(raw, 9)
    data = b''.join(chunk(b'IDAT', z[i:i + idat_size]) for i in range(0, len(z), idat_size))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, colortype, 0, 0, interlace))
                           + extra + data + chunk(b'IEND', b''))

def generate(d):
    u8 = lambda *shape: rng.integers(0, 256, shape, dtype=np.uint8)
    a = np.zeros((70, 70, 4), np.uint8); a[..., 0] = 255; a[..., 3] = np.linspace(0, 255, 70, dtype=np.uint8)[None, :]
    write_png(f'{d}/ok_alpha_gradient_70.png', a, 6)
    write_png(f'{d}/ok_rgba_multi_idat_70.png', u8(70, 70, 4), 6, idat_size=500)
    write_png(f'{d}/ok_rgb_70.png', u8(70, 70, 3), 2)
    write_png(f'{d}/ok_gray_70.png', u8(70, 70, 1), 0)
    write_png(f'{d}/ok_gray_alpha_70.png', u8(70, 70, 2), 4)
    pal, alpha = u8(16, 3), u8(16)
    write_png(f'{d}/ok_palette_70.png', rng.integers(0, 16, (70, 70, 1), dtype=np.uint8), 3, extra=chunk(b'PLTE', pal.tobytes()))
    write_png(f'{d}/ok_palette_trns_70.png', rng.integers(0, 16, (70, 70, 1), dtype=np.uint8), 3,
              extra=chunk(b'PLTE', pal.tobytes()) + chunk(b'tRNS', alpha.tobytes()))
    write_png(f'{d}/ok_exact_64.png', u8(64, 64, 3), 2)
    write_png(f'{d}/ok_enlarge_50.png', u8(50, 50, 3), 2)
    write_png(f'{d}/ok_max_80.png', u8(80, 80, 4), 6)
    write_png(f'{d}/bad_too_large_81.png', u8(81, 81, 3), 2)
    write_png(f'{d}/bad_interlaced_70.png', u8(70, 70, 3), 2, interlace=1, filters=False)
    write_png(f'{d}/bad_16bit_70.png', u8(70, 70, 2), 0, depth=16, filters=False)
    open(f'{d}/bad_jpeg.png', 'wb').write(b'\xff\xd8\xff\xe0' + bytes(200))
    good = open(f'{d}/ok_rgb_70.png', 'rb').read()
    open(f'{d}/bad_truncated_70.png', 'wb').write(good[:len(good) // 2])
    open(f'{d}/bad_corrupt_data_70.png', 'wb').write(good[:60] + bytes(b ^ 0x5A for b in good[60:200]) + good[200:])

def reference(path):
    a = np.asarray(Image.open(path).convert('RGBA')).astype(float)
    q = (a[..., :3] * a[..., 3:4] / 255.0).astype(int)                   # blend onto black
    r5, g6, b5 = q[..., 0] >> 3, q[..., 1] >> 2, q[..., 2] >> 3            # RGB565 like the firmware
    img = np.stack([(r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)], -1).astype(float)
    h, w = img.shape[:2]
    def weights(src, dst):
        W = np.zeros((dst, src))
        for o in range(dst):
            lo, hi = o * src / dst, (o + 1) * src / dst
            for i in range(int(lo), min(src, int(np.ceil(hi)))): W[o, i] = max(0, min(hi, i + 1) - max(lo, i))
        return W / W.sum(1, keepdims=True)
    ref = np.einsum('oy,yxc,px->opc', weights(h, 64), img, weights(w, 64))
    return np.stack([np.rint(ref[..., 0] * 31 / 255), np.rint(ref[..., 1] * 63 / 255), np.rint(ref[..., 2] * 31 / 255)], -1)

def main():
    with tempfile.TemporaryDirectory() as d:
        generate(d)
        exe = f'{d}/logo_test'
        subprocess.run(['g++', '-std=gnu++17', '-O2', '-w', f'{HERE}/logo_test.cpp', f'{REPO}/lib/LogoImage/LogoImage.cpp',
                        f'-I{REPO}/lib/LogoImage', f'-I{HERE}/stubs', '-lz', '-o', exe], check=True)
        files = sorted(f'{d}/{f}' for f in os.listdir(d) if f.endswith('.png'))
        out = subprocess.run([exe] + files, capture_output=True, text=True, check=True).stdout
        results = dict((line.split('\t')[0], line.split('\t')[1:]) for line in out.strip().splitlines())
        failed = 0
        for f in files:
            name, (state, *err) = os.path.basename(f), results[f]
            if name.startswith('bad_'):
                ok = state == 'ERROR'; info = err[0] if err else 'accepted!'
            else:
                if state != 'OK': ok, info = False, err[0]
                else:
                    got = np.fromfile(f + '.565', '<u2').reshape(64, 64)
                    g = np.stack([got >> 11, (got >> 5) & 63, got & 31], -1)
                    diff = int(np.abs(g - reference(f)).max()); ok = diff <= 1; info = f'max diff {diff}'
            failed += not ok
            print(f'{"ok  " if ok else "FAIL"} {name:28s} {info}')
        print('\nall logos within 1 RGB565 step' if not failed else f'\n{failed} FAILED')
        return 1 if failed else 0

if __name__ == '__main__':
    sys.exit(main())
