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
import sqlite3
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
    console_jobs = {'com.apple.SpringBoard'}

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

    # Native launchd disabled database in /var/db/com.apple.xpc.launchd/disabled.plist
    db_dir = os.path.join(root, 'private/var/db/com.apple.xpc.launchd')
    os.makedirs(db_dir, exist_ok=True)
    safe_chmod(db_dir, 0o755)
    disabled_plist = os.path.join(db_dir, 'disabled.plist')
    save_plist(disabled_plist, {
        'com.apple.fairplayd.H2': True,
        'com.apple.ReportCrash': True,
        'com.apple.ReportCrash.Jetsam': True,
        'com.apple.ReportCrash.SimulateCrash': True,
        'com.apple.CrashHousekeeping': True,
        'com.apple.spindump': True,
        'com.apple.DumpPanic': True,
        'com.apple.DumpBasebandCrash': True,
        'com.apple.OTACrashCopier': True,
        'com.apple.ProxiedCrashCopier.ProxyingDevice': True,
        'com.apple.crash_mover': True,
        'com.apple.crashreportcopymobile': True,
        'com.apple.analyticsd': True,
        'com.apple.aggregated': True,
        'com.apple.aggregated.addaily': True,
        'com.apple.bluetoothd': True,
    }, 0, 0, 0o644)
    migrated_flag = os.path.join(db_dir, 'disabled.migrated')
    if not os.path.exists(migrated_flag):
        with open(migrated_flag, 'w') as f:
            pass
        safe_chmod(migrated_flag, 0o644)
    print("  [LAUNCHD] Native /var/db/com.apple.xpc.launchd/disabled.plist written")


def tweak_directories(root):
    # Ensure /dev exists for devfs mount
    os.makedirs(os.path.join(root, 'dev'), exist_ok=True)
    safe_chmod(os.path.join(root, 'dev'), 0o755)

    # World-writable tmp directories with sticky bit
    for tmp in ['tmp', 'private/tmp', 'private/var/tmp']:
        d = os.path.join(root, tmp)
        os.makedirs(d, exist_ok=True)
        safe_chmod(d, 0o1777)

    # Containers structure
    containers = os.path.join(root, 'private/var/containers')
    for sub, uid in [('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)]:
        d = os.path.join(containers, sub)
        os.makedirs(d, exist_ok=True)
        safe_chown(d, uid, uid)
        safe_chmod(d, 0o755)

    # Core private/var and mobile paths
    for p in [
        'private/var/log', 'private/var/run', 'private/var/mobile',
        'private/var/mobile/Library',
        'private/var/mobile/Library/SpringBoard',
        'private/var/mobile/Library/FrontBoard',
        'private/var/mobile/Library/Preferences',
        'private/var/mobile/Library/Caches'
    ]:
        full = os.path.join(root, p)
        os.makedirs(full, exist_ok=True)
        if 'mobile' in p:
            safe_chown(full, MOBILE_UID, MOBILE_GID)
        safe_chmod(full, 0o755)


def tweak_staged_apps(root):
    # In iOS 12, installd spends 45s validating each of 24 staged apps against FairPlay on first boot.
    # Emptying /private/var/staged_system_apps allows installd to complete in < 0.05s so SpringBoard can transition immediately.
    staged = os.path.join(root, 'private/var/staged_system_apps')
    if os.path.exists(staged):
        shutil.rmtree(staged)
        os.makedirs(staged, exist_ok=True)
        safe_chmod(staged, 0o755)
        print("  [APPS] Cleared /private/var/staged_system_apps to bypass 45s app install timeouts")


def tweak_frontboard(root):
    # Pre-create valid applicationState.db so SQLite doesn't fail with Code 26 (not a database)
    fb_dir = os.path.join(root, 'private/var/mobile/Library/FrontBoard')
    os.makedirs(fb_dir, exist_ok=True)
    safe_chown(fb_dir, MOBILE_UID, MOBILE_GID)
    safe_chmod(fb_dir, 0o755)

    db_path = os.path.join(fb_dir, 'applicationState.db')
    if os.path.exists(db_path):
        os.remove(db_path)

    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    cur.executescript("""
        CREATE TABLE schema(version INT NOT NULL);
        INSERT INTO schema (version) VALUES (1);
        CREATE TABLE key_tab (id INTEGER PRIMARY KEY, key TEXT NOT NULL, UNIQUE(key));
        CREATE TABLE application_identifier_tab (id INTEGER PRIMARY KEY, application_identifier TEXT NOT NULL, UNIQUE(application_identifier));
        CREATE TABLE kvs (
            id INTEGER PRIMARY KEY,
            application_identifier INT REFERENCES application_identifier_tab(id),
            key INT REFERENCES key_tab(id),
            value BLOB,
            UNIQUE(application_identifier, key)
        );
        CREATE INDEX kvs_keys ON kvs(key);
        CREATE INDEX kvs_application_identifiers ON kvs(application_identifier);
        CREATE VIEW kvs_debug AS
            SELECT application_identifier_tab.application_identifier, key_tab.key, value
            FROM application_identifier_tab, key_tab, kvs
            WHERE kvs.application_identifier=application_identifier_tab.id AND kvs.key=key_tab.id;
    """)
    conn.commit()
    conn.close()

    safe_chown(db_path, MOBILE_UID, MOBILE_GID)
    safe_chmod(db_path, 0o644)
    print("  [FRONTBOARD] Seeded valid schema into ~/Library/FrontBoard/applicationState.db")


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


def tweak_migrators(root):
    # Disable all DataClassMigrators so DataMigration finishes instantly in 0 ms
    migrators_dir = os.path.join(root, 'System/Library/DataClassMigrators')
    disabled_dir = os.path.join(root, 'System/Library/DataClassMigrators.disabled')
    if os.path.exists(migrators_dir) and not os.path.exists(disabled_dir):
        shutil.move(migrators_dir, disabled_dir)
        os.makedirs(migrators_dir, exist_ok=True)
        safe_chmod(migrators_dir, 0o755)
        print("  [MIGRATION] Moved DataClassMigrators to DataClassMigrators.disabled; created empty directory")


def tweak_activation(root):
    purplebuddy_dict = {
        'SetupDone': True,
        'SetupFinishedAllSteps': True,
        'SetupVersion': 11,
        'daemonizeNeedsToRun': False,
        'deviceSetup': True,
    }
    # Mobile preferences
    mobile_pref_dir = os.path.join(root, 'private/var/mobile/Library/Preferences')
    os.makedirs(mobile_pref_dir, exist_ok=True)
    safe_chown(mobile_pref_dir, MOBILE_UID, MOBILE_GID)
    safe_chmod(mobile_pref_dir, 0o755)
    save_plist(os.path.join(mobile_pref_dir, 'com.apple.purplebuddy.plist'), purplebuddy_dict, MOBILE_UID, MOBILE_GID, 0o600)

    # Root preferences
    root_pref_dir = os.path.join(root, 'private/var/root/Library/Preferences')
    os.makedirs(root_pref_dir, exist_ok=True)
    safe_chmod(root_pref_dir, 0o755)
    save_plist(os.path.join(root_pref_dir, 'com.apple.purplebuddy.plist'), purplebuddy_dict, 0, 0, 0o600)

    ark_dict = {
        'ActivationState': 'Activated',
        '-ActivationState': 'Activated',
        'BrickState': False,
        '-BrickState': False,
        'BuildVersion': BUILD,
        '-BuildVersion': BUILD,
        'ProductVersion': VERSION,
        '-ProductVersion': VERSION,
        'DataMigrationCompleted': True,
        '-DataMigrationCompleted': True,
        'PreSydroDataMigrationCompleted': True,
        '-PreSydroDataMigrationCompleted': True,
        'LastActivated': 0,
        '-LastActivated': 0,
        'UniqueDeviceID': '0000000000000000000000000000000000000000',
        '-UniqueDeviceID': '0000000000000000000000000000000000000000',
    }

    # 1. Seed activation ark in mobile internal
    ark_dir = os.path.join(root, 'private/var/mobile/Library/internal')
    os.makedirs(ark_dir, exist_ok=True)
    safe_chown(ark_dir, MOBILE_UID, MOBILE_GID)
    safe_chmod(ark_dir, 0o755)
    save_plist(os.path.join(ark_dir, 'data_ark.plist'), ark_dict, MOBILE_UID, MOBILE_GID, 0o600)

    # 2. Seed activation ark in mobile mad (MobileActivationDaemon directory)
    mad_dir = os.path.join(root, 'private/var/mobile/Library/mad')
    os.makedirs(mad_dir, exist_ok=True)
    safe_chown(mad_dir, MOBILE_UID, MOBILE_GID)
    safe_chmod(mad_dir, 0o755)
    save_plist(os.path.join(mad_dir, 'data_ark.plist'), ark_dict, MOBILE_UID, MOBILE_GID, 0o600)

    # 3. Seed activation ark in root lockdown
    lockdown_dir = os.path.join(root, 'private/var/root/Library/Lockdown')
    os.makedirs(lockdown_dir, exist_ok=True)
    safe_chmod(lockdown_dir, 0o755)
    save_plist(os.path.join(lockdown_dir, 'data_ark.plist'), ark_dict, 0, 0, 0o600)

    # 4. Seed valid activation record
    rec_dir = os.path.join(lockdown_dir, 'activation_records')
    os.makedirs(rec_dir, exist_ok=True)
    safe_chmod(rec_dir, 0o755)
    rec_file = os.path.join(rec_dir, 'activation_record.plist')
    save_plist(rec_file, {
        'ActivationState': 'Activated',
        'UniqueDeviceID': '0000000000000000000000000000000000000000',
    }, 0, 0, 0o600)

    # 5. Pre-seed System container for com.apple.mobileactivationd
    sys_cont_uuid = '2C778CD2-4523-440F-8B58-D3C4E5E9C57D'
    sys_cont_dir = os.path.join(root, f'private/var/containers/Data/System/{sys_cont_uuid}')
    os.makedirs(os.path.join(sys_cont_dir, 'Library/internal'), exist_ok=True)
    safe_chmod(sys_cont_dir, 0o755)
    save_plist(os.path.join(sys_cont_dir, '.com.apple.mobile_container_manager.metadata.plist'), {
        'MCMMetadataIdentifier': 'com.apple.mobileactivationd',
        'MCMMetadataUUID': sys_cont_uuid,
        'MCMMetadataSchemaVersion': 1,
        'MCMMetadataType': 12,
    }, 0, 0, 0o644)
    save_plist(os.path.join(sys_cont_dir, 'Library/internal/data_ark.plist'), ark_dict, 0, 0, 0o600)

    # 6. Pre-seed Shared SystemGroup container for systemgroup.com.apple.mobileactivationd
    grp_cont_uuid = '64D5676B-AE87-4BF6-BE4A-D871FF4E05DC'
    grp_cont_dir = os.path.join(root, f'private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobileactivationd')
    os.makedirs(os.path.join(grp_cont_dir, 'Library/internal'), exist_ok=True)
    safe_chmod(grp_cont_dir, 0o755)
    save_plist(os.path.join(grp_cont_dir, '.com.apple.mobile_container_manager.metadata.plist'), {
        'MCMMetadataIdentifier': 'systemgroup.com.apple.mobileactivationd',
        'MCMMetadataUUID': grp_cont_uuid,
        'MCMMetadataSchemaVersion': 1,
        'MCMMetadataType': 13,
    }, 0, 0, 0o644)
    save_plist(os.path.join(grp_cont_dir, 'Library/internal/data_ark.plist'), ark_dict, 0, 0, 0o600)

    # 7. Surgical binary patches for mobileactivationd
    patch_mobileactivationd(root)

    # 8. Surgical binary patches for SpringBoard
    patch_springboard(root)

    # 9. Surgical binary patches for backboardd
    patch_backboardd(root)

    print("  [ACTIVATION] Mobile activation state seeded as Activated across all paths (internal, mad, lockdown, system containers) and binaries patched")


def patch_mobileactivationd(root):
    path = os.path.join(root, 'usr/libexec/mobileactivationd')
    if not os.path.exists(path):
        return
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    patched = False
    # 1. 0x3609c: tbz w0, #0x0, 0x100036130 (a0 04 00 36) -> nop (1f 20 03 d5)
    if data[0x3609c:0x360a0] == bytes.fromhex('a0040036'):
        data[0x3609c:0x360a0] = bytes.fromhex('1f2003d5')
        print("  [PATCH] mobileactivationd: nop tbz at 0x3609c (skip SEP migration)")
        patched = True

    # 2. 0x3718c: tbz w20, #0x0, 0x100037a88 (f4 47 00 36) -> nop (1f 20 03 d5)
    if data[0x3718c:0x37190] == bytes.fromhex('f4470036'):
        data[0x3718c:0x37190] = bytes.fromhex('1f2003d5')
        print("  [PATCH] mobileactivationd: nop tbz at 0x3718c (skip crash branch)")
        patched = True

    # 3. 0x20d438: pointer to @"Unactivated" (0x100213918) -> @"Activated" (0x100213938)
    if data[0x20d438:0x20d440] == bytes.fromhex('1839210001000000'):
        data[0x20d438:0x20d440] = bytes.fromhex('3839210001000000')
        print("  [PATCH] mobileactivationd: fallback ActivationState -> Activated")
        patched = True

    if patched:
        with open(path, 'wb') as f:
            f.write(data)
        safe_chmod(path, 0o755)


def patch_backboardd(root):
    path = os.path.join(root, 'usr/libexec/backboardd')
    if not os.path.exists(path):
        return
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    patched = False

    # 1. 0x25740: -[BKDisplayRenderOverlay presentWithAnimationSettings:]
    # Original: sub sp, sp, #0xc0 (ff 03 03 d1 f8 5f 08 a9)
    # Patch to: mov w0, #0; ret (00 00 80 52 c0 03 5f d6)
    # This prevents ANY render overlay (boot logo level -1, level 2999, bootUI) from ever being presented to the display
    if data[0x25740:0x25748] == bytes.fromhex('ff0303d1f85f08a9'):
        data[0x25740:0x25748] = bytes.fromhex('00008052c0035fd6')
        print("  [PATCH] backboardd: -[BKDisplayRenderOverlay presentWithAnimationSettings:] -> return NO")
        patched = True

    # 2. 0x4ed78: -[BKSystemApplication finishedStartup]
    # Original: nop; ldrsw x8, 0x1000bad3c (1f 20 03 d5 08 fe 35 98)
    # Patch to: mov w0, #1; ret (20 00 80 52 c0 03 5f d6)
    # This informs backboardd that the system app has completed startup
    if data[0x4ed78:0x4ed80] == bytes.fromhex('1f2003d508fe3598'):
        data[0x4ed78:0x4ed80] = bytes.fromhex('20008052c0035fd6')
        print("  [PATCH] backboardd: -[BKSystemApplication finishedStartup] -> return YES")
        patched = True

    # 3. 0x6bf34: -[BKBootUIPresenter _queue_addOverlay]
    # Original: sub sp, sp, #0x60 (ff 83 01 d1)
    # Patch to: ret (c0 03 5f d6)
    if data[0x6bf34:0x6bf38] == bytes.fromhex('ff8301d1'):
        data[0x6bf34:0x6bf38] = bytes.fromhex('c0035fd6')
        print("  [PATCH] backboardd: -[BKBootUIPresenter _queue_addOverlay] -> ret")
        patched = True

    # 4. 0x6c12c: -[BKBootUIPresenter _queue_addUnderlay]
    # Original: sub sp, sp, #0x60 (ff 83 01 d1)
    # Patch to: ret (c0 03 5f d6)
    if data[0x6c12c:0x6c130] == bytes.fromhex('ff8301d1'):
        data[0x6c12c:0x6c130] = bytes.fromhex('c0035fd6')
        print("  [PATCH] backboardd: -[BKBootUIPresenter _queue_addUnderlay] -> ret")
        patched = True

    # 5. 0x6bcf4: -[BKBootUIPresenter _queue_updateOverlay]
    # Original: tbz w21, #0x0, 0x10006bd54 (15 03 00 36)
    # Patch to: b 0x10006be38 (51 00 00 14)
    if data[0x6bcf4:0x6bcf8] == bytes.fromhex('15030036'):
        data[0x6bcf4:0x6bcf8] = bytes.fromhex('51000014')
        print("  [PATCH] backboardd: -[BKBootUIPresenter _queue_updateOverlay] -> remove overlay")
        patched = True

    if patched:
        with open(path, 'wb') as f:
            f.write(data)
        safe_chmod(path, 0o755)


def patch_springboard(root):
    path = os.path.join(root, 'System/Library/CoreServices/SpringBoard.app/SpringBoard')
    if not os.path.exists(path):
        return
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    patched = False
    # 1. -[SBSetupManager isInSetupMode] at 0x2182c -> mov w0, #0; ret (00 00 80 52 c0 03 5f d6)
    if data[0x2182c:0x21834] == bytes.fromhex('08530090019544f9'):
        data[0x2182c:0x21834] = bytes.fromhex('00008052c0035fd6')
        print("  [PATCH] SpringBoard: -[SBSetupManager isInSetupMode] -> return NO")
        patched = True

    # 2. -[SBLockdownManager brickedDevice] at 0x2c587c -> mov w0, #0; ret (00 00 80 52 c0 03 5f d6)
    if data[0x2c587c:0x2c5884] == bytes.fromhex('fd7bbfa9fd030091'):
        data[0x2c587c:0x2c5884] = bytes.fromhex('00008052c0035fd6')
        print("  [PATCH] SpringBoard: -[SBLockdownManager brickedDevice] -> return NO")
        patched = True

    # 3. -[SBLockdownManager lockdownState] at 0x2c58a0 -> mov w0, #2; ret (40 00 80 52 c0 03 5f d6)
    if data[0x2c58a0:0x2c58a8] == bytes.fromhex('f44fbea9fd7b01a9'):
        data[0x2c58a0:0x2c58a8] = bytes.fromhex('40008052c0035fd6')
        print("  [PATCH] SpringBoard: -[SBLockdownManager lockdownState] -> return 2 (Activated)")
        patched = True

    if patched:
        with open(path, 'wb') as f:
            f.write(data)
        safe_chmod(path, 0o755)


def tweak_graphics(root):
    # Disable Apple GPU bundles without real A8 GPU
    ext_dir = os.path.join(root, 'System/Library/Extensions')
    for b in ['AGXGLDriver.bundle', 'AGXMetalA8.bundle']:
        p = os.path.join(ext_dir, b)
        if os.path.exists(p):
            shutil.move(p, p + '.disabled')
            print(f"  [GPU] {b} disabled (forces software rendering)")


def tweak_timezone(root):
    # Link /var/db/timezone/zoneinfo to /usr/share/zoneinfo.default
    tz_dir = os.path.join(root, 'private/var/db/timezone')
    os.makedirs(tz_dir, exist_ok=True)
    zi = os.path.join(tz_dir, 'zoneinfo')
    if not os.path.exists(zi):
        os.symlink('/usr/share/zoneinfo.default', zi)
        print("  [TIMEZONE] /var/db/timezone/zoneinfo -> /usr/share/zoneinfo.default linked")


def tweak_fstab(root):
    # Ensure / is mounted rw in fstab so /private/var and databases are fully writable on OnePlus 2
    fstab = os.path.join(root, 'private/etc/fstab')
    if os.path.exists(fstab):
        with open(fstab, 'w') as f:
            f.write('/dev/disk0s1 / hfs rw,noatime 0 1\n')
        print("  [FSTAB] /dev/disk0s1 set to rw,noatime in /etc/fstab")


def apply_tweaks(root):
    print("[TWEAKS] Applying iOS 12.5.7 system bring-up tweaks...")
    tweak_launchd(root)
    tweak_directories(root)
    tweak_staged_apps(root)
    tweak_frontboard(root)
    tweak_audio(root)
    tweak_graphics(root)
    tweak_timezone(root)
    tweak_fstab(root)
    tweak_migrators(root)
    tweak_activation(root)

    # Pre-seed migration completion to bypass all 67 DataClassMigrators
    migration_dict = {
        'LastSystemVersion': BUILD,
        'DMLastMigrationResults': {
            'buildVersion': BUILD,
            'success': True,
        },
        'DMUserDataDisposition': {
            'SyntheticDidUpgrade': False,
            'buildVersion': BUILD,
        },
        'DataMigrationCompleted': True,
    }
    merge_pref(root, 'com.apple.migration', migration_dict)
    merge_pref(root, 'com.apple.DataMigration', migration_dict)
    merge_pref(root, 'com.apple.datamigrator', {
        'DataMigrationVersion': BUILD,
        'DataMigrationCompleted': True,
        'LastSystemVersion': BUILD,
    })

    # Also mirror into root's Preferences
    root_pref_dir = os.path.join(root, 'private/var/root/Library/Preferences')
    os.makedirs(root_pref_dir, exist_ok=True)
    safe_chmod(root_pref_dir, 0o755)
    for p_name, p_data in [
        ('com.apple.migration', migration_dict),
        ('com.apple.DataMigration', migration_dict),
        ('com.apple.datamigrator', {'DataMigrationVersion': BUILD, 'DataMigrationCompleted': True, 'LastSystemVersion': BUILD}),
    ]:
        p_path = os.path.join(root_pref_dir, f"{p_name}.plist")
        save_plist(p_path, p_data, 0, 0, 0o600)

    # Skip setup assistant and disable single-core watchdog kills
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0, 'com.apple.SpringBoard': 100.0},
        'MCForceShowCloudConfigurationUI': False,
        'SBDontLockAfterMigration': True,
        'SBShowNonDefaultSystemApps': True,
    })
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True,
        'SetupFinishedAllSteps': True,
        'SetupVersion': 11,
        'daemonizeNeedsToRun': False,
        'deviceSetup': True,
    })
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in (
            'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
            'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
            'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
            'SilentICDPUpgrade'
        )
    })
    # Configure backboardd to suppress boot UI overlay and watchdog kills
    backboardd_dict = {
        'BKHideAppleLogoOnLaunch': True,
        'hideAppleLogoOnLaunch': True,
        'BKNoWatchdogs': True,
        'BKEnableStevenoteRotation': False,
        'BKDisableCloneMirroring': False,
        'BKALSUserPreferencesEpoch': 1,
    }
    merge_pref(root, 'com.apple.backboardd', backboardd_dict)
    save_plist(os.path.join(root_pref_dir, 'com.apple.backboardd.plist'), backboardd_dict, 0, 0, 0o600)
    lib_pref_dir = os.path.join(root, 'Library/Preferences')
    os.makedirs(lib_pref_dir, exist_ok=True)
    save_plist(os.path.join(lib_pref_dir, 'com.apple.backboardd.plist'), backboardd_dict, 0, 0, 0o644)

    print("[TWEAKS] All bring-up tweaks applied successfully!")


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--apply':
        apply_tweaks(sys.argv[2])
        run(['sync'])
        print(f"[SUCCESS] Applied all tweaks to {sys.argv[2]}")
        sys.exit(0)

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
