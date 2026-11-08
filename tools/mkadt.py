#!/usr/bin/env python3
"""Build the minimal Apple Device Tree (ADT) for OnePlus 2 (MSM8994).

Target: OnePlus 2 (ONE A2003 / MSM8994) booting Darwin/XNU (xnu-4903 generation).
Identity: iPhone 6 Plus (iPhone7,1 / N56AP) matching iOS 12.5.7 userland.
All MMIO addresses come from the OnePlus 2 live FDT and iomem dump.

Usage:
  python3 tools/mkadt.py OUT.adt [--dump] [--qemu]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM:
# Live FDT / iomem: low bank carve-outs end at 0x10200000.
# High bank has Ramoops at 0xac000000.
# Usable continuous DRAM: 0x10200000 to 0xabffffff (~2.44 GiB).
PHYS_BASE = 0x10200000
MEM_SIZE  = 0x9be00000  # 2494 MiB

# Panic log buffer: 1 MiB carved off the top of DRAM
PANIC_LOG_SIZE = 0x100000

# /arm-io children are relative to this parent base (must not be 0)
SOC_BASE = 0x01000000

# GICv2 on MSM8994
GICD, GICD_SIZE = 0xf9000000, 0x1000
GICC, GICC_SIZE = 0xf9002000, 0x1000

# Ramoops debug console buffer
RAMOOPS_BASE, RAMOOPS_SIZE = 0xac0be000, 0x40000  # 256 KiB console
FB_W, FB_H, FB_STRIDE = 1080, 1920, 1080 * 4

# Internal eMMC host (sdhci@f9824900)
SDHCI, SDHCI_SIZE = 0xf9824900, 0x1a0
SDHCI_CORE, SDHCI_CORE_SIZE = 0xf9824000, 0x800

TIMEBASE = 19200000  # 19.2 MHz generic timer
VTIMER_INTID = 20    # PPI 4 = 16 + 4 = 20

# QEMU virt (-M virt -cpu cortex-a57): GICv2 / CNTFRQ
QEMU_GICD, QEMU_GICD_SIZE = 0x08000000, 0x10000
QEMU_GICC, QEMU_GICC_SIZE = 0x08010000, 0x10000
QEMU_TIMEBASE = 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'oneplus2 boot manifest').digest()
OP2_ECID = 0x4F50320000000001  # 'OP2\0' + 1

N56AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-12/n56ap.adt')


def sanitize_n56ap_node(name, ph, path=N56AP_ADT):
    """Extract an entire node (with all props and children) from real N56AP device tree,
    filtering out placeholder flags (bit 31) and assigning valid sequential phandles."""
    if not os.path.exists(path):
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    def _clean(src):
        n = adt.Node()
        for k, f, v in src.props:
            if k == 'AAPL,phandle' or f:
                continue
            n.props.append((k, 0, v))
        n.props.insert(1, ('AAPL,phandle', 0, ph()[1]))
        for ch in src.children:
            n.children.append(_clean(ch))
        return n
    for c in root.children:
        n = dict((k, v) for k, f, v in c.props).get('name')
        if n == (name.encode() + b'\0'):
            return _clean(c)
    return None


u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False):
    if qemu:
        phys_base, mem_size = 0x40000000, 0x40000000  # 1 GiB DRAM in QEMU virt
    else:
        phys_base, mem_size = PHYS_BASE, MEM_SIZE
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE

    timebase = QEMU_TIMEBASE if qemu else TIMEBASE
    vtimer_intid = QEMU_VTIMER_INTID if qemu else VTIMER_INTID

    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('oneplus2-shim')),
        ('system-firmware-version', cstr('oneplus2-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha1')),
        ('display-scale', u32(3)),
        ('unique-chip-id', struct.pack('<Q', OP2_ECID)),
        ('chip-id', u32(0x7000)),  # Apple A8 (T7000)
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [
        node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])
    ])

    # CPUs: cpu0 (boot) + cpu1 (SMP step 1). Extra cores are gated by the
    # `cpus=N` boot-arg (ml_parse_cpu_topology caps avail_cpus), so cpus=1
    # keeps single-core behavior with this ADT.
    def _cpu_node(idx):
        return node('cpu%d' % idx, [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('arm,cortex-a53') + cstr('ARM,v8')),
            ('reg', u32(idx)),
            ('cpu-id', u32(idx)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(idx)),
            ('l2-cache-id', u32(0)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if idx == 0 else 'off')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(1555200000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1600000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ])
    cpu_nodes = [_cpu_node(0), _cpu_node(1), _cpu_node(2), _cpu_node(3)]
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    if qemu:
        gic_reg = u64(QEMU_GICD - SOC_BASE, QEMU_GICD_SIZE,
                      QEMU_GICC - SOC_BASE, QEMU_GICC_SIZE)
    else:
        gic_reg = u64(GICD - SOC_BASE, GICD_SIZE,
                      GICC - SOC_BASE, GICC_SIZE)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,cortex-a15-gic') + cstr('arm,gic-400') + cstr('arm,gic-v2')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(vtimer_intid)),
        ('reg', gic_reg),
    ])

    arm_io_children = [
        gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('op2,display') + cstr('adbe0,t7000')),
            ('canvas-width', u32(1080)),
            ('canvas-height', u32(1920)),
        ])
    ]

    if qemu:
        arm_io_children.append(
            node('uart0', [
                ph(),
                ('compatible', cstr('arm,pl011')),
                ('reg', u64(0x08000000, 0x1000)),
            ])
        )
    else:
        arm_io_children.append(
            node('sdhci', [
                ph(),
                ('compatible', cstr('qcom,sdhci-msm')),
                ('reg', u64(SDHCI - SOC_BASE, SDHCI_SIZE,
                            SDHCI_CORE - SOC_BASE, SDHCI_CORE_SIZE)),
            ])
        )
        arm_io_children.append(
            node('i2c2', [
                ph(),
                ('compatible', cstr('op2,i2c2') + cstr('qcom,i2c-msm-v2')),
                ('reg', u64(0xf9924000 - SOC_BASE, 0x1000)),
            ])
        )

    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('op2-io')),
        ('compatible', cstr('op2-io') + cstr('arm-io')),
        ('soc-generation', cstr('H7')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], arm_io_children)

    # Ramoops debug console / Framebuffer descriptor for kernel kprintf
    fbcon = node('op2-fbcon', [
        ph(),
        ('reg', u64(RAMOOPS_BASE, RAMOOPS_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold') + cstr('menu')),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    real_product = sanitize_n56ap_node('product', ph)
    product = [real_product] if real_product is not None else []

    options = [node('options', [ph()])]

    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    # Factory identity props (run102 post-mortem): real N56AP carries these as
    # syscfg refs resolved by iBoot; OP2 has no syscfg, so ship literals.
    # Missing serial-number made MGCopyAnswer(@"SerialNumber") return nil,
    # crashing ndoagent (registerDefaults objects[4]) and friends in a
    # SIGABRT loop. Values are synthetic but shape-correct.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('N56AP') + cstr('iPhone7,1') + cstr('oneplus,oneplus2') + cstr('qcom,msm8994') + cstr('AppleARM')),
        ('model', cstr('iPhone7,1')),
        ('target-type', cstr('N56')),
        ('target-sub-type', cstr('N56AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('serial-number', cstr('OP2IOS120400')),
        ('mlb-serial-number', cstr('OP2IOS120400MLB01')),
        ('region-info', cstr('LL')),
        ('model-number', cstr('MGA82LL/A')),
        ('regulatory-model-number', cstr('A1522')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product + options)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} OUT.adt [--dump] [--qemu]", file=sys.stderr)
        sys.exit(1)

    out_file = sys.argv[1]
    is_qemu = '--qemu' in sys.argv
    do_dump = '--dump' in sys.argv

    tree = build(qemu=is_qemu)
    blob = adt.serialize(tree)
    with open(out_file, 'wb') as f:
        f.write(blob)

    print(f"Generated {out_file}: {len(blob)} bytes (qemu={is_qemu})", file=sys.stderr)

    if do_dump:
        parsed_root, _ = adt.parse(blob)
        adt.dump(parsed_root)
