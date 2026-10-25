#!/usr/bin/env python3
"""
tools/mkop2root.py - Stage Apple iOS 12.5.7 Root Filesystem for OnePlus 2

Mounts the official iOS 12.5.7 rootfs DMG, creates a case-sensitive HFS+ image,
copies all files preserving permissions/symlinks/hardlinks, applies essential
bring-up tweaks for non-Apple hardware, and unmounts cleanly.

Usage:
  sudo python3 tools/mkop2root.py [SRC_DMG] [OUT_IMG] [SIZE_GB]
Default:
  SRC_DMG = out/038-87140-050.dmg
  OUT_IMG = out/ios12-userdata.dmg
  SIZE_GB = 6
"""

import glob
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

BUILD = '16H81'
VERSION = '12.5.7'
MOBILE_UID = 501
MOBILE_GID = 501


def safe_chown(path, uid, gid):
    try:
        os.chown(path, uid, gid)
    except PermissionError:
        pass


def safe_chmod(path, mode):
    try:
        os.chmod(path, mode)
    except PermissionError:
        pass


def run(cmd, check=True):
    print(f"[RUN] {' '.join(cmd)}")
    return subprocess.run(cmd, check=check, capture_output=True, text=True)


def load_plist(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save_plist(path, data, uid=0, gid=0, mode=0o644, fmt=plistlib.FMT_BINARY):
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)
    safe_chown(path, uid, gid)
    safe_chmod(path, mode)


def merge_pref(root, name, values):
    pref_dir = os.path.join(root, 'private/var/mobile/Library/Preferences')
    os.makedirs(pref_dir, exist_ok=True)
    safe_chown(pref_dir, MOBILE_UID, MOBILE_GID)
    safe_chmod(pref_dir, 0o755)

    path = os.path.join(pref_dir, f"{name}.plist")
    data = load_plist(path) if os.path.exists(path) else {}
    data.update(values)
    save_plist(path, data, MOBILE_UID, MOBILE_GID, 0o600)
    print(f"  [PREF] {name}: {sorted(values.keys())}")


def tweak_launchd(root):
    daemons_dir = os.path.join(root, 'System/Library/LaunchDaemons')
    log_env = {'OS_ACTIVITY_DT_MODE': 'YES', 'CFLOG_FORCE_STDERR': '1'}
    console_jobs = {'com.apple.mobileactivationd', 'com.apple.SpringBoard'}

    if os.path.isdir(daemons_dir):
        removed_count = 0
        console_count = 0
        for fname in os.listdir(daemons_dir):
            if fname.startswith('com.apple.CommCenter') and fname.endswith('.plist'):
                try:
                    os.remove(os.path.join(daemons_dir, fname))
                    removed_count += 1
                except Exception:
                    pass
                continue
            if not fname.endswith('.plist'):
                continue
            fpath = os.path.join(daemons_dir, fname)
            try:
                data = load_plist(fpath)
                label = data.get('Label', '')
                changed = False
                if '_PanicOnCrash' in data:
                    del data['_PanicOnCrash']
                    changed = True
                if label in console_jobs:
                    data['StandardOutPath'] = '/dev/console'
                    data['StandardErrorPath'] = '/dev/console'
                    data.setdefault('EnvironmentVariables', {}).update(log_env)
                    changed = True
                    console_count += 1
                if changed:
                    save_plist(fpath, data)
            except Exception:
                pass
        print(f"  [LAUNCHD] LaunchDaemons: {removed_count} CommCenter removed, {console_count} console redirected")

    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    if os.path.exists(path):
        st = os.stat(path)
        plist = load_plist(path)
        daemons = plist.get('LaunchDaemons', {})
        renamed = 0

        for job in daemons.values():
            if '_PanicOnCrash' in job:
                job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
                renamed += 1

        # Remove CommCenter jobs to avoid crash loops without baseband
        removed = [k for k, j in daemons.items() if j.get('Label') == 'com.apple.CommCenter']
        for k in removed:
            del daemons[k]

        for job in daemons.values():
            if job.get('Label') in console_jobs:
                job['StandardOutPath'] = '/dev/console'
                job['StandardErrorPath'] = '/dev/console'
                job.setdefault('EnvironmentVariables', {}).update(log_env)

        save_plist(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
        print(f"  [LAUNCHD] {renamed} _PanicOnCrash renamed, {len(removed)} CommCenter jobs removed")


def tweak_directories(root):
    # Ensure /dev exists for devfs mount
    os.makedirs(os.path.join(root, 'dev'), exist_ok=True)
    safe_chmod(os.path.join(root, 'dev'), 0o755)

    # Containers structure
    containers = os.path.join(root, 'private/var/containers')
    for sub, uid in [('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)]:
        d = os.path.join(containers, sub)
        os.makedirs(d, exist_ok=True)
        safe_chown(d, uid, uid)
        safe_chmod(d, 0o755)

    # Core private/var paths
    for p in ['private/var/log', 'private/var/run', 'private/var/tmp', 'private/var/mobile']:
        full = os.path.join(root, p)
        os.makedirs(full, exist_ok=True)
        if 'mobile' in p:
            safe_chown(full, MOBILE_UID, MOBILE_GID)
        safe_chmod(full, 0o755)


def tweak_audio(root):
    # Disable VirtualAudio HAL plugin which crashes without real DSP
    hal_plugin = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    disabled_dir = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
    if os.path.exists(hal_plugin):
        os.makedirs(disabled_dir, exist_ok=True)
        shutil.move(hal_plugin, disabled_dir)
        print("  [AUDIO] VirtualAudio.plugin disabled")

    # Disable camera recorder filter prewarm without GPU
    for conf in glob.glob(os.path.join(root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        try:
            st = os.stat(conf)
            data = load_plist(conf)
            data['StartRecorderServer'] = False
            save_plist(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, plistlib.FMT_XML)
            print("  [AUDIO] StartRecorderServer set to False")
        except Exception as e:
            print(f"  [WARN] Failed to patch {conf}: {e}")


def tweak_activation(root):
    # Seed activation ark
    ark_dir = os.path.join(root, 'private/var/mobile/Library/internal')
    os.makedirs(ark_dir, exist_ok=True)
    safe_chown(ark_dir, MOBILE_UID, MOBILE_GID)
    safe_chmod(ark_dir, 0o755)

    ark_file = os.path.join(ark_dir, 'data_ark.plist')
    save_plist(ark_file, {
        '-ActivationState': 'Activated',
        '-BrickState': False,
        '-BuildVersion': BUILD,
        '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True,
        '-PreSydroDataMigrationCompleted': True,
    }, MOBILE_UID, MOBILE_GID, 0o600)
    print("  [ACTIVATION] Mobile activation state seeded as Activated")


def apply_tweaks(root):
    print("[TWEAKS] Applying iOS 12.5.7 system bring-up tweaks...")
    tweak_launchd(root)
    tweak_directories(root)
    tweak_audio(root)
    tweak_activation(root)

    # Skip setup assistant and disable single-core watchdog kills
    merge_pref(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}
    })
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False
    })
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True,
        'SetupFinishedAllSteps': True,
        'SetupVersion': 11,
        'daemonizeNeedsToRun': False
    })
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in (
            'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
            'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
            'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
            'SilentICDPUpgrade'
        )
    })
    print("[TWEAKS] All bring-up tweaks applied successfully!")


def main():
    if os.geteuid() != 0:
        print("[NOTE] Running as non-root user. Filesystem tweaks will preserve available permissions.")

    src_dmg = sys.argv[1] if len(sys.argv) > 1 else 'out/038-87140-050.dmg'
    out_img = sys.argv[2] if len(sys.argv) > 2 else 'out/ios12-userdata.dmg'
    size_gb = int(sys.argv[3]) if len(sys.argv) > 3 else 6

    if not os.path.exists(src_dmg):
        sys.exit(f"Error: Source DMG not found: {src_dmg}")

    print(f"=== OnePlus 2 iOS 12.5.7 Root Stager ===")
    print(f"  Source DMG : {src_dmg}")
    print(f"  Output Image: {out_img} ({size_gb} GB)")

    # 1. Mount source DMG
    print(f"[STEP 1] Mounting source DMG...")
    res = subprocess.run(['hdiutil', 'attach', '-plist', '-readonly', '-nobrowse', src_dmg], check=True, capture_output=True)
    pl_src = plistlib.loads(res.stdout)
    src_mnt = None
    src_dev = None
    for ent in pl_src.get('system-entities', []):
        if 'mount-point' in ent:
            src_dev = ent['dev-entry']
            src_mnt = ent['mount-point']
            break

    if not src_mnt:
        sys.exit(f"Failed to find mount point in hdiutil plist:\n{res.stdout}")
    print(f"  Source mounted at: {src_mnt} ({src_dev})")

    try:
        # 2. Create blank HFS+ image
        print(f"[STEP 2] Creating {size_gb}GB blank disk image...")
        if os.path.exists(out_img):
            os.remove(out_img)

        run(['hdiutil', 'create', '-size', f'{size_gb}g', '-layout', 'NONE', '-fs', 'Case-sensitive HFS+', '-volname', 'iOSRoot', out_img])

        # 3. Attach destination image
        print(f"[STEP 3] Attaching destination image...")
        res_dst = subprocess.run(['hdiutil', 'attach', '-plist', '-nobrowse', out_img], check=True, capture_output=True)
        pl_dst = plistlib.loads(res_dst.stdout)
        dst_dev = None
        dst_mnt = None
        for ent in pl_dst.get('system-entities', []):
            if 'mount-point' in ent:
                dst_dev = ent['dev-entry']
                dst_mnt = ent['mount-point']
                break

        if not dst_mnt:
            sys.exit(f"Failed to find destination mount point in hdiutil plist:\n{res_dst.stdout}")
        print(f"  Destination mounted at: {dst_mnt} ({dst_dev})")

        try:
            # 4. Copy files via rsync
            print(f"[STEP 4] Copying iOS 12.5.7 rootfs files (this may take a few minutes)...")
            run(['rsync', '-aH', '--exclude=.HFS+*', '--exclude=.Trashes', '--exclude=.fseventsd', '--exclude=.file', '--exclude=.ba', '--exclude=.mb', f"{src_mnt}/", f"{dst_mnt}/"])
            print("  Copy completed successfully!")

            # 5. Apply tweaks
            apply_tweaks(dst_mnt)

            # 6. Sync
            run(['sync'])
        finally:
            print(f"[CLEANUP] Detaching destination volume {dst_mnt}...")
            run(['hdiutil', 'detach', dst_mnt, '-force'], check=False)

    finally:
        print(f"[CLEANUP] Detaching source volume {src_mnt}...")
        run(['hdiutil', 'detach', src_mnt, '-force'], check=False)

    print(f"\n[SUCCESS] Generated iOS 12.5.7 Rootfs Image: {out_img} ({os.path.getsize(out_img)} bytes)")


if __name__ == '__main__':
    main()
