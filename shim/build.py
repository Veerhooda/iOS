#!/usr/bin/env python3
"""
Build script for OnePlus 2 (MSM8994) M1 Boot Shim.
Compiles start.S, links ELF, extracts binary, injects LK DTB offset at 0x2C,
appends the 7 stock DTBs, and packages out/shim-boot.img with mkbootimg.
"""

import os
import subprocess
import struct
import sys

def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    shim_dir = os.path.join(repo_root, "shim")
    out_dir = os.path.join(repo_root, "out")
    os.makedirs(out_dir, exist_ok=True)

    start_s = os.path.join(shim_dir, "start.S")
    main_c = os.path.join(shim_dir, "main.c")
    console_c = os.path.join(shim_dir, "console.c")
    fdt_c = os.path.join(shim_dir, "fdt.c")

    start_o = os.path.join(out_dir, "start.o")
    main_o = os.path.join(out_dir, "main.o")
    console_o = os.path.join(out_dir, "console.o")
    fdt_o = os.path.join(out_dir, "fdt.o")

    shim_ld = os.path.join(shim_dir, "shim.ld")
    shim_elf = os.path.join(out_dir, "shim.elf")
    shim_bin = os.path.join(out_dir, "shim.bin")
    dtb_blob = os.path.join(repo_root, "dtb", "control_exp", "dtbs.blob")
    kernel_out = os.path.join(out_dir, "shim_kernel.bin")
    ramdisk = os.path.join(repo_root, "dtb", "boot_unpacked", "ramdisk")
    boot_img = os.path.join(out_dir, "shim-boot.img")
    mkbootimg = os.path.join(repo_root, "tools", "mkbootimg.py")

    cflags = [
        "-O2", "-ffreestanding", "-nostdlib", "-fno-pic",
        "-mstrict-align", "-fno-stack-protector", "-mgeneral-regs-only",
        "-Wall", "-Wextra"
    ]

    print("[1/5] Compiling shim sources with clang...")
    subprocess.run(["clang", "-target", "aarch64-none-elf", "-c", start_s, "-o", start_o], check=True)
    subprocess.run(["clang", "-target", "aarch64-none-elf"] + cflags + ["-c", main_c, "-o", main_o], check=True)
    subprocess.run(["clang", "-target", "aarch64-none-elf"] + cflags + ["-c", console_c, "-o", console_o], check=True)
    subprocess.run(["clang", "-target", "aarch64-none-elf"] + cflags + ["-c", fdt_c, "-o", fdt_o], check=True)

    print("[2/5] Linking shim.elf with ld.lld...")
    subprocess.run([
        "ld.lld", "-T", shim_ld, start_o, main_o, console_o, fdt_o, "-o", shim_elf
    ], check=True)

    print("[3/5] Extracting raw binary with llvm-objcopy...")
    objcopy = "/opt/homebrew/opt/llvm/bin/llvm-objcopy"
    if not os.path.exists(objcopy):
        objcopy = "llvm-objcopy"
    subprocess.run([
        objcopy, "-O", "binary", shim_elf, shim_bin
    ], check=True)

    with open(shim_bin, "rb") as f:
        raw_shim = bytearray(f.read())

    # Pad to 4096-byte boundary
    rem = len(raw_shim) % 4096
    if rem != 0:
        raw_shim.extend(b"\x00" * (4096 - rem))
    shim_len = len(raw_shim)
    print(f"      Shim binary size: {shim_len} bytes (0x{shim_len:x})")

    # Inject Little Kernel DTB offset at 0x2C
    struct.pack_into("<I", raw_shim, 0x2C, shim_len)
    val_at_2c = struct.unpack_from("<I", raw_shim, 0x2C)[0]
    assert val_at_2c == shim_len, f"Offset 0x2C check failed: {val_at_2c} != {shim_len}"
    print(f"      Injected DTB offset at 0x2C: 0x{val_at_2c:08x} ({val_at_2c})")

    print("[4/5] Appending stock DTB blob (7 DTBs)...")
    with open(dtb_blob, "rb") as f:
        dtb_data = f.read()
    assert dtb_data[:4] == b"\xd0\x0d\xfe\xed", "DTB magic check failed!"

    with open(kernel_out, "wb") as f:
        f.write(raw_shim)
        f.write(dtb_data)
    total_kernel_size = os.path.getsize(kernel_out)
    print(f"      Combined kernel size: {total_kernel_size} bytes ({total_kernel_size // 1024} KB)")

    print("[5/5] Generating boot image with mkbootimg...")
    cmdline = (
        "androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x37 "
        "ehci-hcd.park=3 lpm_levels.sleep_disabled=1 boot_cpus=0-3 "
        "loop.max_part=7 androidboot.selinux=permissive buildvariant=userdebug"
    )
    subprocess.run([
        sys.executable, mkbootimg,
        "--header_version", "0",
        "--kernel", kernel_out,
        "--ramdisk", ramdisk,
        "--pagesize", "4096",
        "--base", "0x00000000",
        "--kernel_offset", "0x00008000",
        "--ramdisk_offset", "0x01000000",
        "--tags_offset", "0x00000100",
        "--cmdline", cmdline,
        "-o", boot_img
    ], check=True)

    boot_img_size = os.path.getsize(boot_img)
    print(f"\n[SUCCESS] Generated boot image: {boot_img} ({boot_img_size} bytes, {boot_img_size // 1024} KB)")

    # Verify boot image structure
    with open(boot_img, "rb") as f:
        hdr = f.read(4096)
        magic = hdr[:8]
        assert magic == b"ANDROID!", f"Invalid boot.img magic: {magic}"
        k_sz, k_addr, r_sz, r_addr = struct.unpack_from("<IIII", hdr, 8)
        print(f"  Header verification:")
        print(f"    Kernel size: {k_sz} bytes, Load address: 0x{k_addr:08x}")
        print(f"    Ramdisk size: {r_sz} bytes, Load address: 0x{r_addr:08x}")

        # Check kernel first block
        k_data = f.read(k_sz)
        app_dtb_off = struct.unpack_from("<I", k_data, 0x2C)[0]
        assert app_dtb_off == shim_len, f"DTB offset in boot.img mismatch: {app_dtb_off} != {shim_len}"
        fdt_magic = k_data[app_dtb_off:app_dtb_off + 4]
        assert fdt_magic == b"\xd0\x0d\xfe\xed", f"FDT magic not found at offset {app_dtb_off}!"
        print(f"    DTB offset at 0x2C: 0x{app_dtb_off:08x} -> FDT magic 0x{fdt_magic.hex()} verified!")

if __name__ == "__main__":
    main()
