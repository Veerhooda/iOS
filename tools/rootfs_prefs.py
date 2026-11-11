#!/usr/bin/env python3
"""Seed per-user prefs into a mounted iOS rootfs: rootfs_prefs.py <mountpoint>"""
import os, plistlib, sys
root = sys.argv[1]
pref = os.path.join(root, 'private/var/mobile/Library/Preferences/com.apple.Accessibility.plist')
data = {}
if os.path.exists(pref):
    with open(pref, 'rb') as f:
        try:
            data = plistlib.load(f)
        except Exception:
            data = {}
data.update({
    'AssistiveTouchEnabled': False,              # assistivetouchd sat at 53% CPU
    'EnhancedBackgroundContrastEnabled': True,   # Settings > Accessibility > Reduce Transparency
})
os.makedirs(os.path.dirname(pref), exist_ok=True)
with open(pref, 'wb') as f:
    plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
os.chown(pref, 501, 501)
os.chmod(pref, 0o600)
print('  [PREF] com.apple.Accessibility:', data)
