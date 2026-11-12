#!/usr/bin/env python3
"""Games on a mounted iOS rootfs: rootfs_games.py <mountpoint>
Every out/games/build/*.app goes to /Applications/<name>.app, owned root:wheel, like Apple's own apps. The image
has no LaunchServices database (it is built on first boot by scanning /Applications), so a fresh flash registers
them by itself. Build the apps first (tools/games/build-*.sh). Third-party games without a licence are for this
phone only: never commit or redistribute the .app.
"""
import os
import shutil
import sys

root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
build = os.path.join(here, '..', 'out', 'games', 'build')
if not os.path.isdir(build):
    print('  [games] none built')
    sys.exit(0)
for name in sorted(os.listdir(build)):
    if not name.endswith('.app'):
        continue
    src, dst = os.path.join(build, name), os.path.join(root, 'Applications', name)
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst, symlinks=True)
    for d, dirs, files in os.walk(dst):
        os.chown(d, 0, 0)
        os.chmod(d, 0o755)
        for f in files:
            p = os.path.join(d, f)
            os.chown(p, 0, 0)
            os.chmod(p, 0o755 if f == name[:-4] else 0o644)
    print('  [games] /Applications/%s (%d files)' % (name, sum(len(f) for _, _, f in os.walk(dst))))
