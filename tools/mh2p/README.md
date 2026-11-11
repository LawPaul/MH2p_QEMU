# MH2P / Tegra T124 source preview

This is an experimental, AI-assisted QEMU fork, not an upstream QEMU
contribution or a complete head-unit emulator. See the
[code provenance policy](../../docs/devel/code-provenance.rst).
See the [main guide](../../README.md) for the scope of this MH2P preview.
Inherited shared helpers do not imply support for other head-unit platforms.

## Current status and release boundaries

The tested local profile is Porsche **MH2p_ER_POG35_P2865**, MMX variant
**22**, with Quickboot board **699-61857-5017-202**. Other firmware trains,
variants and boards have not been validated.

| Area | Observed locally | Not established |
| --- | --- | --- |
| MMX startup | Original QNX startup/kernel, serial console, early drivers, NOR stage-two loading and mNAND app mounting | Complete unmodified normal boot |
| RCC startup | Original DRA72x manager, IPU startup and PCIe networking in diagnostic controls | Complete companion/vehicle hardware |
| Original HMI | Display initialization on three fresh controls; six reversible native Home/Media input cycles | Sustained health or complete backend applications |
| Networking | Original RCC network PID survives native input; two late interface/ping snapshots pass in a bounded control | Endurance, reconnect/recovery or all DSI services |
| Graphics | Diagnostic GLES bridge, native display-controller scanout and native RSTP input | Native GPU command rendering |
| Ignition | Experimental frame-level CAN wake lets original code set KL15; original IPC watchdog renewals observed | Complete vehicle profile, validated CAN CRC/counters or complete SubCPU peer |
| Audio | Original DIRANA faulting stores pass with modeled Cortex-A15 asynchronous AXI error reporting | Full audio; amplifier and microphone watchdog failures remain |
| Persistence | Diagnostic local configuration and read probes exist | Durable RCC-backed persistence/restart recovery |

These are diagnostic observations, not a supported end-user HMI launcher.
The complete paired MMX/RCC harness still depends on private local tooling
and prepared assets. It is not included in this preview. In particular,
[start_hmi_runtime.sh](start_hmi_runtime.sh) runs **inside an already
prepared diagnostic guest**; it does not launch QEMU or the complete HMI.

The latest bounded control verifies the diagnostic shell itself at priority
30 without changing original application or watchdog priorities. This
resolves a diagnostic-console timeout in that control, not a hardware
readiness problem. Original amplifier/microphone watchdog records remain.
A roughly eleven-minute host observation includes boot and only about
46 seconds of RCC virtual-clock advancement; it is not eleven minutes of
healthy HMI uptime.

Response times vary. First visible transitions in the fresh controls took
roughly 7-14 host seconds; subsequent transitions ranged from about 0.16
to 2 seconds. These samples do not establish consistent performance.
Navigation, Telephone and Car Menu startup spinners remain. Home/Media
transitions are not proof that those applications or audio work.

No MH2P release archive has been produced. Linux local results do not prove
macOS portability. The portable-build workflow includes firmware-free board
and evidence tests, but its MH2P additions have not been validated on GitHub.
See the [publication checklist](../PUBLICATION.md) before publishing.

## Build

Use an out-of-tree build directory to avoid overwriting an existing build:

```sh
# Run from the checkout root.
SOURCE="$PWD"
BUILD=/path/to/new-mh2p-build
mkdir -p "$BUILD"
cd "$BUILD"
"$SOURCE/configure" --target-list=arm-softmmu --disable-docs \
  --disable-gtk --disable-sdl --disable-werror --disable-tools \
  --disable-guest-agent
ninja -j4 qemu-system-arm tests/qtest/mh2p-test tests/qtest/mh2p-rcc-test
cd "$SOURCE"
```

Install a C compiler, Git, Python 3.11 or newer, Ninja, pkg-config and the GLib, pixman
and zlib development packages first. Configure may fetch pinned QEMU build
dependencies. Early boot and board tests do not require a QNX SDK, ARM
cross compiler, graphics shim or proprietary firmware.

## Prepare your own firmware

Keep all firmware updates, extractions, device captures and generated media
**outside the checkout**. Supply your own legally obtained update. Do not
commit or distribute firmware, device identities, keys, prepared disks,
QNX libraries or firmware-derived overlays.

The tested archive contains `MH2p_ER_POG35_P2865_OTA/Data/`. After extracting
it with a compatible archive decoder:

```sh
DATA=/path/to/MH2p_ER_POG35_P2865_OTA/Data
MEDIA=/path/to/private-mh2p-media
LOADIMG="$DATA/MMX2P.mifs-stage1_CLU28_MMX2P_PAG_ER_G35_070PROD/22/nvflash/alpinesecureboot/common/loadimg.img"
DTB="$DATA/MMX2P.quickboot_CLU28_MMX2P_PAG_ER_G35_070PROD/22/nvflash/alpinesecureboot/699-61857-5017-202_3398/tegra_dtb_8_signed.dtb"

python3 tools/mh2p/prepare.py --loadimg "$LOADIMG" --dtb "$DTB" \
  --output "$MEDIA/stock"
python3 tools/mh2p/prepare.py --loadimg "$LOADIMG" --dtb "$DTB" \
  --output "$MEDIA/shell" --diagnostic-shell
```

Output directories must be new. Preparation validates pinned inputs, image
bounds and ImageFS checksums, and writes hashes and patch metadata to a
manifest. Input files are not modified.

The diagnostic mode replaces only the startup script with a shell launcher
and deliberately skips normal services. Native executables are not patched.
Do not use the stage-one `tegra_dtb_14_signed.dtb`, which is not an FDT.
The Quickboot DTB above contains the tested board tree. Hash pinning is not
signature verification; this machine does not reproduce secure boot.

| Input | SHA-256 |
| --- | --- |
| `loadimg.img` | `b22c54ad94c664786ea8f0502fb8795787a45b59f1fc25c488cc081f58fdf9e6` |
| Quickboot DTB | `dea94a02537dff8db75a1b5cfbf3ce4c8d4fe2579c95bd0812722c28b5e8f5c5` |

### Optional NOR and mNAND

The stage-one loader can read stage two through the emulated NOR controller:

```sh
python3 tools/mh2p/prepare_nor.py \
  --stage2 "$DATA/MMX2P.mifs-stage2_CLU28_MMX2P_PAG_ER_G35_070PROD/20/main_stage2.img" \
  --output "$MEDIA/nor"
```

Without an explicitly supplied authorized base capture, this produces an
**incomplete** flash image: unspecified areas remain erased. Do not invent
factory provisioning or use generated media on physical hardware.

[prepare_mnand.py](prepare_mnand.py) builds a separate app disk from
user-supplied assets. [prepare_rcc_nor.py](prepare_rcc_nor.py) prepares
RCC diagnostic media. Use each tool's `--help` for its required inputs and
guarded options; none supplies a complete head-unit image or launcher.

## Interactive diagnostic shell

```sh
"$BUILD/qemu-system-arm" -M mh2p \
  -kernel "$MEDIA/shell/startup.bin" -dtb "$MEDIA/shell/board.dtb" \
  -display none -monitor none -nic none -serial null -serial stdio
```

UART B is the second serial port, so the first `-serial null` is required.
At `mh2p#`, enter `echo MH2P_SHELL_OK:$((6 * 7))` and require the actual
`MH2P_SHELL_OK:42` response, not an echo of the command. The shell lacks
full job control; Ctrl-C with this stdio backend terminates QEMU.

Use the stock media to trace original startup instead. Unsupported devices
can fault; reaching early userspace is not full normal boot.

## Reproducible checks

Firmware-free synthetic-image, protocol and evidence tests:

```sh
PYTHONPATH=python:tools/mh2p QTEST_QEMU_BINARY="$BUILD/qemu-system-arm" \
  python3 -m unittest discover \
  -s tools/mh2p -p 'test_*.py'
QTEST_QEMU_BINARY="$BUILD/qemu-system-arm" "$BUILD/tests/qtest/mh2p-test"
QTEST_QEMU_BINARY="$BUILD/qemu-system-arm" "$BUILD/tests/qtest/mh2p-rcc-test"
```

The full suite also needs Python's `qemu.qmp` and `cryptography` packages and Korn shell
(`ksh`, or the explicit `MH2P_TEST_KSH` executable path) for preparation
and installer tests. Run it in an environment containing those dependencies;
QEMU's isolated build venv does not necessarily contain them.
Optional graphics/compiler checks need the corresponding host tools, and
some tests skip without an explicitly configured RL78 or Bluetooth test
backend. Passing synthetic tests does not establish original firmware
compatibility. Missing required dependencies are errors, not passing tests.

With your own prepared media:

```sh
python3 tools/mh2p/check_boot.py --qemu "$BUILD/qemu-system-arm" \
  --stock "$MEDIA/stock" --shell "$MEDIA/shell" \
  --output "$MEDIA/boot-check"
```

This checks early startup and shell execution across reset, not full HMI.
The output directory must be new. Logs and manifests remain private.

### Original display initialization evidence

[hmi_readiness.py](hmi_readiness.py) validates retained native display evidence:

```sh
python3 tools/mh2p/hmi_readiness.py /path/to/private/display-capture.log
```

The original `Fw.Display` success message comes from **QNX slog**, not
only JVM stdout. The paired diagnostic owner uses
`display_wait_command(slog=True)` to capture `sloginfo` and retain JVM
stdout separately. Log-source errors, original display failures and explicit
timeouts override success. Bare strings and echoed search commands cannot
satisfy the verifier. Its file-only default remains for synthetic fixtures.
Display initialization success alone does not establish sustained health.

### Existing private live-session diagnostics

These clients require a running compatible diagnostic owner, which this
preview does not supply:

```sh
python3 tools/mh2p/boot_status.py --directory /path/to/private/run
python3 tools/mh2p/hmi_live_control.py \
  --control-directory /path/to/private/run health
python3 tools/mh2p/hmi_live_control.py \
  --control-directory /path/to/private/run persistence-health
```

Boot status is recorded progress, not host/process/service liveness.
Live commands are serialized; do not attach competing console clients.
Keep control sockets, VNC and diagnostic listeners local. Guest logs,
screenshots and dumps may contain personal data even when source does not.

### QNX pbuffer smoke client

[build_graphics_smoke.py](build_graphics_smoke.py) builds an isolated ARM
guest client and bridge libraries using an `arm-none-eabi` compiler.
[prepare_graphics_smoke.py](prepare_graphics_smoke.py) injects them into
a separate diagnostic IFS; [check_guest_graphics.py](check_guest_graphics.py)
checks actual guest output. Run each with `--help`.
These experiments demonstrate guest/host transport, not native GPU execution.

## Model boundaries and development tools

The models include bounded subsets of MMX/RCC clocks, reset, GPIO, UART,
I2C/SPI, storage, PCIe networking, HOST1X and display DMA. Unsupported device
operations are not promises of compatibility. In particular, GK20A execution,
full SOR/link behavior, complete vehicle hardware and all audio peripherals
are unfinished. The original GPU driver still reaches an unsupported
`TOP_DEVICE_INFO(0)` read at GPU offset `0x22700`; do not replace missing
hardware evidence with guessed enumeration tables or readiness stubs.

The graphics bridge is explicitly diagnostic. Development helpers can
change copied configuration, add local persistence or load diagnostic
libraries; inspect their manifests and options before use.
[build_graphics_development.py](build_graphics_development.py) rebuilds
adapters in an existing diagnostic IFS and verifies source/resource hashes;
it is not a stock-firmware-to-HMI builder.

CarPlay, Bluetooth, Wi-Fi, trusted-kernel and physical-unit ModKit helpers
are research tools, not supported preview features. No active CarPlay
backend, phone compatibility or physical-unit installation safety is claimed.
The separately generated CarPlay ModKit package and all firmware-derived
payloads are excluded from emulator publication.

Release follow-up remains: resolve native watchdog failures, prove
endurance and cold-start behavior, consolidate a user-reproducible paired
launcher, then validate a clean build and honestly scoped release.
