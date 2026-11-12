#!/usr/bin/env bash
# Build ellojess/OrangeTree (an Angry Birds-style SpriteKit game, no licence: personal install only, do not
# redistribute) for this phone: iOS 12.2 arm64 with Xcode 10.3's Swift 5.0.1 (runs under Rosetta). Xcode 10.3's
# ibtool/xcodebuild do not run on this macOS, so: the two storyboards are replaced by the standard programmatic
# window setup (build glue only, applied to a copy), launch images make iOS run it full screen, the .sks levels
# and the texture-atlas PNGs are copied as they are. Output: out/games/build/OrangeTree.app
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
X=/Users/veerhooda/xcode-10.3/Xcode.app/Contents/Developer
SDK=$X/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS12.4.sdk
SWIFTC=$X/Toolchains/XcodeDefault.xctoolchain/usr/bin/swiftc
SRC=$ROOT/out/games/OrangeTree
[ -d "$SRC" ] || git clone -q https://github.com/ellojess/OrangeTree.git "$SRC"
WORK=$ROOT/out/games/work-orangetree
APP=$ROOT/out/games/build/OrangeTree.app
rm -rf "$WORK" "$APP"; mkdir -p "$WORK" "$APP"
cp "$SRC"/OrangeTree/*.swift "$WORK"/

# glue: no storyboards
python3 - "$WORK" <<'PY'
import sys, re
w = sys.argv[1]
p = w + '/AppDelegate.swift'; s = open(p).read()
s = s.replace('''        // Override point for customization after application launch.
        return true''', '''        // op2 build: no storyboards (Xcode 10.3's ibtool does not run on this Mac)
        window = UIWindow(frame: UIScreen.main.bounds)
        window?.rootViewController = GameViewController()
        window?.makeKeyAndVisible()
        return true''', 1)
open(p, 'w').write(s)
p = w + '/GameViewController.swift'; s = open(p).read()
s = s.replace('''    override func viewDidLoad() {''', '''    // op2 build: the view the storyboard made
    override func loadView() {
        view = SKView(frame: UIScreen.main.bounds)
    }

    override func viewDidLoad() {''', 1)
open(p, 'w').write(s)
PY

"$SWIFTC" -sdk "$SDK" -target arm64-apple-ios12.2 -O -module-name OrangeTree -swift-version 5 \
  -Xlinker -rpath -Xlinker /usr/lib/swift -Xlinker -rpath -Xlinker @executable_path/Frameworks \
  -emit-executable -o "$APP/OrangeTree" "$WORK"/*.swift   # the rpaths Xcode adds: the OS Swift runtime (iOS 12.2+)

cp "$SRC"/OrangeTree/Level-*.sks "$APP"/
cp "$SRC"/OrangeTree/Smashing-Things.atlas/*.png "$APP"/   # SKTexture(imageNamed:) finds bundle images

# launch images: without a launch screen iOS runs the app letterboxed at 320x480
python3 - "$APP" <<'PY'
import sys, zlib, struct
def png(path, w, h):
    raw = b''.join(b'\x00' + b'\x00\x00\x00' * w for _ in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
png(sys.argv[1] + '/LaunchImage-736h@3x.png', 1242, 2208)
png(sys.argv[1] + '/LaunchImage-Landscape-736h@3x.png', 2208, 1242)
PY

python3 - "$SRC/OrangeTree/Info.plist" "$APP/Info.plist" <<'PY'
import sys, plistlib
d = plistlib.load(open(sys.argv[1], 'rb'))
d.update({'CFBundleDevelopmentRegion': 'en', 'CFBundleExecutable': 'OrangeTree',
          'CFBundleIdentifier': 'com.op2.orangetree', 'CFBundleName': 'OrangeTree',
          'CFBundleDisplayName': 'Orange Tree', 'CFBundleSupportedPlatforms': ['iPhoneOS'],
          'MinimumOSVersion': '12.2', 'DTPlatformName': 'iphoneos', 'DTSDKName': 'iphoneos12.4',
          'UIDeviceFamily': [1], 'UIRequiredDeviceCapabilities': ['arm64']})
d.pop('UIMainStoryboardFile', None); d.pop('UILaunchStoryboardName', None)
d['UILaunchImages'] = [
    {'UILaunchImageName': 'LaunchImage-736h', 'UILaunchImageMinimumOSVersion': '8.0',
     'UILaunchImageOrientation': 'Portrait', 'UILaunchImageSize': '{414, 736}'},
    {'UILaunchImageName': 'LaunchImage-Landscape-736h', 'UILaunchImageMinimumOSVersion': '8.0',
     'UILaunchImageOrientation': 'Landscape', 'UILaunchImageSize': '{414, 736}'}]
plistlib.dump(d, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
PY
printf 'APPL????' > "$APP/PkgInfo"
codesign -s - --force "$APP"
ls -la "$APP"; file "$APP/OrangeTree"
