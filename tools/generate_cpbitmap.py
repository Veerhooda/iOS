#!/usr/bin/env python3
"""
tools/generate_cpbitmap.py - Generate Apple iOS 12.5.7 CPBitmap files and Wallpaper plists.
Encodes input wallpaper image to 2592x2592 BGRA with exact 28-byte CPBitmap trailer
matching Apple's AppSupport/CPBitmapStore specification:
  - Width: 2592, Height: 2592
  - Format: 1 (BGRA 32bpp)
  - Colorspace len: 0, Info len: 0
  - Image count: 1
  - Magic: 0xdcb543a2 (little-endian: a2 43 b5 dc)
  - Total file size: 26,873,884 bytes
"""

import os
import plistlib
import struct
import sys
from PIL import Image

def generate_cpbitmap(src_img_path, out_cpbitmap_path):
    print(f"[CPBITMAP] Loading {src_img_path}...")
    im = Image.open(src_img_path).convert('RGBA')
    im_resized = im.resize((2592, 2592), Image.LANCZOS)

    # Convert RGBA -> BGRA
    r, g, b, a = im_resized.split()
    im_bgra = Image.merge('RGBA', (b, g, r, a))
    pixel_bytes = im_bgra.tobytes()

    assert len(pixel_bytes) == 2592 * 2592 * 4, f"Invalid pixel buffer size: {len(pixel_bytes)}"

    # 28-byte modern Apple CPBitmap trailer
    # struct format: <IIIIIII
    #   colorspace_len = 0
    #   info_len = 0
    #   width = 2592
    #   height = 2592
    #   format = 1 (BGRA)
    #   image_count = 1
    #   magic = 0xdcb543a2
    trailer = struct.pack('<IIIIIII', 0, 0, 2592, 2592, 1, 1, 0xdcb543a2)

    total_data = pixel_bytes + trailer
    assert len(total_data) == 26873884, f"Invalid total CPBitmap size: {len(total_data)}"

    with open(out_cpbitmap_path, 'wb') as f:
        f.write(total_data)

    print(f"[CPBITMAP] Wrote {out_cpbitmap_path} ({len(total_data)} bytes)")

def generate_wallpaper_options_plist(out_plist_path):
    options = {
        'wallpaperScale': 1.0,
        'parallaxFactor': 0.0,
        'zoomScale': 1.0,
        'cropRect': '{{0, 0}, {2592, 2592}}',
        'portrait': True,
        'supportsCropping': True,
        'hasVideo': False,
        'stillTimeInVideo': 0.0,
        'name': 'WWDC2018',
    }
    with open(out_plist_path, 'wb') as f:
        plistlib.dump(options, f, fmt=plistlib.FMT_BINARY)
    print(f"[PLIST] Wrote {out_plist_path}")

def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default_src = os.path.join(repo_root, 'Library/Wallpaper/iPhone/500.WWDC2018-414w-736h@3x~iphone.jpg')
    if not os.path.exists(default_src):
        # Fallback to mounted volume if local path doesn't have it
        default_src = '/Volumes/iOSRoot 1/Library/Wallpaper/iPhone/500.WWDC2018-414w-736h@3x~iphone.jpg'

    target_dir = sys.argv[1] if len(sys.argv) > 1 else '/tmp/sb_test'
    os.makedirs(target_dir, exist_ok=True)

    cpbitmap_path = os.path.join(target_dir, 'LockBackground.cpbitmap')
    generate_cpbitmap(default_src, cpbitmap_path)

    # Mirror for HomeBackground, OriginalLockBackground, OriginalHomeBackground
    for name in ['HomeBackground.cpbitmap', 'OriginalLockBackground.cpbitmap', 'OriginalHomeBackground.cpbitmap']:
        dst = os.path.join(target_dir, name)
        import shutil
        shutil.copyfile(cpbitmap_path, dst)
        print(f"[CPBITMAP] Copied {name}")

    # Generate options plists
    for name in ['LockWallpaperOptions.plist', 'HomeWallpaperOptions.plist']:
        dst = os.path.join(target_dir, name)
        generate_wallpaper_options_plist(dst)

    print("[SUCCESS] All wallpaper assets generated successfully!")

if __name__ == '__main__':
    main()
