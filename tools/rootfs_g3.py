#!/usr/bin/env python3
"""G3: put the Mesa draw test on a mounted iOS rootfs: rootfs_g3.py <mountpoint>
   /usr/local/bin/op2-air-run                       out/mesa-ios/out/air_run (tools/mesa-ios.sh)
   /System/Library/LaunchDaemons/com.op2.air-run.plist   runs it once at boot, output to /dev/console
                                                    (lands in ramoops: logs/daily-*.log after a round)"""
import os, plistlib, shutil, sys
root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
src_shell = os.path.join(here, '..', 'out', 'shell', 'build', 'op2shell')
use_shell = os.path.exists(src_shell)
src = src_shell if use_shell else os.path.join(here, '..', 'out', 'mesa-ios', 'out', 'air_run')
dst = os.path.join(root, 'usr/local/bin/op2-air-run')
os.makedirs(os.path.dirname(dst), exist_ok=True)
shutil.copyfile(src, dst)
os.chown(dst, 0, 0)
os.chmod(dst, 0o755)
log = '/dev/console'   # into the kernel log, which ramoops keeps (as mkop2root.py does for SpringBoard)
if use_shell:
    job = {
        'Label': 'com.op2.shell',
        'ProgramArguments': ['/usr/local/bin/op2-air-run'],
        'RunAtLoad': True,
        'KeepAlive': False,
        'AbandonProcessGroup': True,
        'UserName': 'root',
        'StandardOutPath': log,
        'StandardErrorPath': log,
    }
else:
    job = {
        'Label': 'com.op2.air-run',
        'ProgramArguments': ['/usr/local/bin/op2-air-run',
                             '/System/Library/Frameworks/QuartzCore.framework/default.metallib', 'clear_frag'],
        'EnvironmentVariables': {'FD_MESA_DEBUG': 'sysmem'},
        'RunAtLoad': True,
        'KeepAlive': False,
        'StandardOutPath': log,
        'StandardErrorPath': log,
        'UserName': 'root',
    }
plist = os.path.join(root, 'System/Library/LaunchDaemons/com.op2.air-run.plist')
with open(plist, 'wb') as f:
    plistlib.dump(job, f, fmt=plistlib.FMT_XML)
os.chown(plist, 0, 0)
os.chmod(plist, 0o644)
print('  [G3] op2-air-run (%d bytes) + com.op2.air-run.plist installed' % os.path.getsize(dst))
