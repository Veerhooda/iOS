#!/bin/bash
# Stage Apple's open source HFS for OnePlus 2 built-in filesystem (bsd/op2_hfs).
set -euo pipefail

XNU_DIR="/Users/veerhooda/Downloads/OnePlus 2/xnu/xnu-4903.270.47"
TARGET="$XNU_DIR/bsd/op2_hfs"

if [[ -d "$TARGET" ]]; then
    echo "Target directory $TARGET already exists."
    exit 0
fi

hfs_sha=31d5ec238dfab485e78a9a30b8b92d13e8914f03
hfs_tmp=$(mktemp -d)
trap 'rm -rf "$hfs_tmp"' EXIT

echo "Fetching apple-oss-distributions/hfs at $hfs_sha..."
git init -q "$hfs_tmp"
git -C "$hfs_tmp" fetch -q --depth 1 \
    https://github.com/apple-oss-distributions/hfs "$hfs_sha"
git -C "$hfs_tmp" checkout -q FETCH_HEAD

python3 - "$hfs_tmp" "$TARGET" <<'PY'
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
target = Path(sys.argv[2])
target.mkdir(parents=True, exist_ok=True)

# Copy core and hfs_encodings
for directory in ('core', 'hfs_encodings'):
    shutil.copytree(source / directory, target / directory)

# Guard AppleKeyStore include in core/hfs_iokit.h
path = target / 'core' / 'hfs_iokit.h'
text = path.read_text()
include = '#include <AppleKeyStore/AppleKeyStoreFSServices.h>\n'
first = 'int hfs_unwrap_key('
end = '__END_DECLS'
if text.count(include) == 1 and text.count(first) == 1 and text.count(end) == 1:
    text = text.replace(include, '#if CONFIG_PROTECT\n' + include + '#endif\n')
    i, j = text.index(first), text.index(end)
    text = text[:i] + '#if CONFIG_PROTECT\n' + text[i:j].rstrip('\n') + '\n#endif\n\n' + text[j:]
    path.write_text(text)
PY

# Copy TargetConditionals.h from SDK
SDK_PATH=$(xcrun --sdk macosx --show-sdk-path)
cp "$SDK_PATH/usr/include/TargetConditionals.h" "$TARGET/"
echo "Staged hfs-627.100.6 into $TARGET"
