#!/usr/bin/env python3
"""Focused test: tweak_graphics keeps Apple GPU bundles intact.

Regression coverage for the stuck-at-Apple-logo fix: OP2 must NOT rename
AGXGLDriver/AGXMetalA8 to .disabled (ios-sweet leaves them intact and reaches
the home screen with CPU rendering). Run: python3 tools/test_tweak_graphics.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mkop2root import tweak_graphics  # noqa: E402

BUNDLES = ['AGXGLDriver.bundle', 'AGXMetalA8.bundle']


def make_root(with_disabled=False):
    root = tempfile.mkdtemp(prefix='op2gputest')
    ext = os.path.join(root, 'System/Library/Extensions')
    os.makedirs(ext)
    for b in BUNDLES:
        d = os.path.join(ext, b + ('.disabled' if with_disabled else ''))
        os.makedirs(d)
        with open(os.path.join(d, 'Info.plist'), 'w') as f:
            f.write('x')
    return root


def test_bundles_kept_intact():
    root = make_root()
    tweak_graphics(root)
    ext = os.path.join(root, 'System/Library/Extensions')
    for b in BUNDLES:
        assert os.path.isdir(os.path.join(ext, b)), f'{b} must stay in place'
        assert not os.path.exists(os.path.join(ext, b + '.disabled')), \
            f'{b} must NOT be renamed to .disabled'


def test_disabled_bundles_restored():
    root = make_root(with_disabled=True)
    tweak_graphics(root)
    ext = os.path.join(root, 'System/Library/Extensions')
    for b in BUNDLES:
        assert os.path.isdir(os.path.join(ext, b)), f'{b} must be restored'
        assert not os.path.exists(os.path.join(ext, b + '.disabled')), \
            f'{b}.disabled must be gone after restore'


def test_idempotent():
    root = make_root()
    tweak_graphics(root)
    tweak_graphics(root)
    ext = os.path.join(root, 'System/Library/Extensions')
    for b in BUNDLES:
        assert os.path.isdir(os.path.join(ext, b))


if __name__ == '__main__':
    test_bundles_kept_intact()
    test_disabled_bundles_restored()
    test_idempotent()
    print('PASS: tweak_graphics keeps GPU bundles intact (3 checks)')
