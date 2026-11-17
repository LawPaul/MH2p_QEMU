# MH2P / Tegra T124 source preview

This is an experimental, AI-assisted QEMU fork, not an upstream QEMU
contribution or a complete head-unit emulator. See the
[code provenance policy](../../docs/devel/code-provenance.rst).
See the [main guide](../../README.md) for the scope of this MH2P preview.
Inherited shared helpers do not imply support for other head-unit platforms.

The obsolete MHI2 machine, peripherals, launchers, profiles and boot evidence
have been removed. Reused filesystem/image helpers now live in this directory;
the diagnostic graphics transport, renderer and regression sources live in
[glforward/](glforward/README.md). Tegra30 GPIO, I2C, NOR and timer models remain
because the MH2P board uses them. MHI2-only CWM adapters, codec forwarding,
shader profiles and artwork overrides are not retained. Active graphics
controls use the `MH2P_*` prefix; private launchers using the old prefix must
be updated.

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
| Audio | Original DIRANA AXI fault handled; native I2C/SPI controller completion corrected | Full audio, DSP execution/IPC and surviving audio services |
| Persistence | Emulated RCC persistence service on RSTP with RCC and M4 NvM defaults; backend mappings initialize; one original-backend key round-trips across a restart | Native RCC/M4 persistence path, written M4-owned data, broad consumer coverage |

These are diagnostic observations, not a supported end-user HMI launcher.
The complete paired MMX/RCC harness still depends on private local tooling
and prepared assets. It is not included in this preview. In particular,
[start_hmi_runtime.sh](start_hmi_runtime.sh) runs **inside an already
prepared diagnostic guest**; it does not launch QEMU or the complete HMI.

The diagnostic shell can run at priority 30 without changing original
application or watchdog priorities. High-volume console captures perturb
scheduling and must be separated from endurance checks.

A low-intrusion paired audio control reached 180.249 seconds on the RCC
virtual hardware counter. No application-thread watchdog errors were
captured in that control, but two original DSP watchdog resets and native
ProcessManager shutdown occurred. DIRANA, amplifier, microphone and SYS_LINK
processes were absent at the final sample. This is not a healthy audio or HMI
endurance result; zero capture/helper exit statuses do not establish guest
health.

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

RCC I2C transfer termination asserts register-access-ready (ARDY), including
when the transfer ends with NACK. The NACK remains visible: completion does
not mean that an amplifier or microphone acknowledged the access, and no
audio slave is supplied by this controller behavior.

RCC McSPI master transfers also complete on an unattached SSI bus: SPI has
no acknowledgement phase. Such accesses report a guest error and receive
QEMU's undriven SSI bus value, not an audio-chip identity or status response.
Controller transfer completion does not demonstrate DIRANA initialization
or audio processing.

The original RCC audio DSP executable targets TI TMS320C6000. The default
board has no enabled C6000 execution model; DSP clock/crossbar register
coverage is not DSP execution or functioning MessageQ IPC. Original DSP watchdog
recovery remains an unsupported-hardware boundary, not a reason to suppress
watchdogs or supply fabricated heartbeat acknowledgements.
The DSP1 reset registers expose the documented held-reset state, with no
release-history bits, in a fixed powered domain. Status acknowledgement and
asserting both resets are
supported; releasing either reset remains explicitly unsupported without a
connected DSP execution backend. This avoids an external data abort on the
original loader's initial `RM_DSP1_RSTST` (`0x4ae06414`) read without
inventing a completed DSP reset or boot. Power-state changes are likewise
unsupported.
DSP1 root-clock gating is tracked independently; enabling that clock does not
clear reset, complete the module wakeup or execute DSP instructions. The
original loader proceeds to request subsystem reset release, then polls the
release-history bit. With no connected DSP/MMU backend that release is
rejected and the loader does not complete; this is not functioning IPC.
Both DSP1 external MMU register banks use the shared DRA7 MMU model, with
independent TLBs and interrupt outputs.

The private native-backend experiment can be selected with
`-machine experimental-dsp1-backend=/absolute/path/to/libc64xemu_backend.so`.
Use `-machine experimental-power-clocks=on` for the separately modeled
power/clock configuration registers used by the original image.
It loads trusted host code, not firmware, and requires private embedding ABI
version 4; no backend library is bundled with the preview. With this
option the DSP1 reset and boot-vector registers drive real backend lifecycle,
and DSP memory transactions use RCC RAM through MMU0 when both MMU enables
are set. Local L2/L1 SRAM windows are shared with the host. DSP-local system
and MMU apertures bypass external translation and route to their documented
L3 addresses. The DSP1 EDMA channel controller at `0x40d10000` is distinct
from the system EDMA controller at `0x43300000`. Its currently exercised
register model includes event/enable/clear state, DRAE/QRAE, interrupt-enable
state, QDMA event enables, and PaRAM storage. It does not schedule channels,
perform PaRAM transfers, model the transfer controllers at `0x40d05000` and
`0x40d06000`, or fabricate completion interrupts; unsupported accesses still
fail explicitly.
Firmware-free tests check translated instruction fetch and stores, shared local SRAM,
held-core subsystem release, clock pause, MMU faults, MMU reset and safe
DSP-initiated local reset, plus local system/MMU/SRAM routing.
Set `MH2P_DSP_BACKEND` when running the RCC qtests to include these tests.

DSP1 crossbar selectors drive CorePac events 32-95; the two DSP MMUs also
drive fixed events 25/26. Source rising edges are queued and delivered before
the next native batch, avoiding backend re-entry during MMIO callbacks.
Firmware-free tests cover real mailbox conditions, source masking and rerouting,
CPU interrupt selection, clock gating and native-generated mailbox events.
Delivery does not consume messages or acknowledge application IPC.

McASP2 write-FIFO configuration, level accounting, and writes are modeled,
but no McASP-to-EDMA request is generated. The FIFO does not drain: serial
clocks, request timing, McASP data movement, and sustained audio IPC remain
unimplemented.
Complete C66x/CorePac coverage and DPLL-derived execution timing also remain
unfinished. Execution uses a bounded diagnostic quota of 1,000 cycles per
100 microseconds of virtual time, not a claimed hardware clock rate.
For local scheduling experiments, the writable QOM property
`diagnostic-cycles-per-tick` on `/machine/dsp1` accepts 1 through 100,000
cycles per unchanged 100-microsecond interval. It can also be selected at
launch with `-global mh2p-rcc-dsp.diagnostic-cycles-per-tick=N`.
The default remains 1,000; reset preserves the selected quota, and clock
gating still stops execution. This is a bounded diagnostic control, not a
DPLL-derived clock model or a hardware-frequency recommendation. Increasing
it changes the DSP/peripheral timing ratio and host execution cost.
In two local original-image controls, a quota of 2,000 removed the observed
communication-task starvation: the DSP returned its version, verification
result and repeated ping replies, and the original host version wrapper
reported success. The 1,000-cycle control left messages queued with the
communication task ready but not scheduled. These controls still timed out
waiting for full HMI input readiness at 420 host seconds. They do not
establish a calibrated clock, complete audio, or end-to-end startup health.
Backend or memory errors stop native execution explicitly; they do not
produce readiness or heartbeat acknowledgements. The opt-in device is
unmigratable. Do not use this experiment as an audio-endurance pass.
QOM properties `pc`, `cycles`, `failed` and `error` on `/machine/dsp1`
provide execution diagnostics, not firmware health. DRA7 EDMA state is
maintained by QEMU's separate controller model, not the backend's
C6455-derived EDMA test hooks.
The read-only `registers` property returns 97 unsigned words in the order
A0-A31, B0-B31, control registers 0-31, then PC. The
`cached-register-memory` property reports resident data-cache bytes around
register-held addresses; a missing entry means no resident line, not zero
memory. Neither diagnostic executes instructions, accesses host memory/MMIO,
fills caches or writes back dirty lines. Both report an error when no backend
instance exists.

A longer paired control at the explicit 2,000-cycle quota, retaining the
private backend's existing floating-point assumptions, was observed in the
original DSP firmware's `C$$EXIT` loop at 32.295 RCC virtual seconds
(628,940,000 DSP cycles). The backend still reported `failed=false` and continued counting
instructions in that terminal loop. Successful earlier ping replies, an
empty backend error and increasing cycle counts therefore cannot establish
DSP survival. Instruction history traces the exit to the native McASP1
receive high-clock reset timeout: the guest requests global control
`0x1f02`, but reads `0x1f00` until its polling counter expires. That receiver
selects an external serial clock, which the current MLB high-clock peer does
not supply; the DRA7 initialization sequence requires a running external
ACLKR for global-control synchronization. The default configuration still
has no external receive serial clock. Forcing the reset readback or extending
the guest timeout would conceal that missing hardware, not fix it. This is
not a five-minute endurance pass or a backend-detected execution fault.

For local clock-routing experiments, machine properties
`experimental-mcasp1-rx-clock-hz` through
`experimental-mcasp8-rx-clock-hz` connect an explicit external ACLKR input
(0, disconnected, by default; maximum 1 GHz). These are independent of
the MLB high-clock peer. They require experimental power clocks, module
enablement and asynchronous RX external-clock selection; synchronous RX
continues to use the transmitter clock and frame sync, bypassing both RX
generators regardless of the receive frame-source setting.
Reset changes synchronize after one
selected serial-clock period, including receiver state-machine release into
its initial frame-sync wait. An asynchronous internal RX frame-sync generator
is not implemented and its reset-release acknowledgement remains withheld.
Disconnecting the clock stalls pending changes
without clearing already latched state. This fixture does not establish the
real board's clock routing or frequency. It supplies no frame sync, samples,
receive FIFO data or DMA events, and is not an audio-health verdict.

A paired original-firmware control with the explicit 3.072 MHz McASP1
receive-clock fixture completed the five-minute DSP-survival observation.
The exact acceptance sample was 300.025970 RCC virtual seconds; the final
DSP sample reached 300.096222 seconds and 5,984,976,000 cycles. Across 973
samples there was no backend fault, terminal firmware exit, counter reset
or cycle regression. Native logs contained no fatal M4 watchdog, native DSP
watchdog reset or completed ProcessManager shutdown. All eight required
watchdog/control threads in `DIRANA3_APP`, `PWR_AMP`, `MIC_ADC` and
`SYS_LINK` remained present in the final capture, including `SLK_CYC`.
The bounded paired run and all helpers exited successfully after 9,868.62
host seconds; this was five guest minutes, not five host minutes.
McASP1 and McASP8
receive reset readback reached `0x1e`; the latter also requires synchronous
receive to bypass its local frame generator, not just its clock generator.
Original firmware, watchdog deadlines, diagnostic DSP quota and the six
existing floating-point assumptions were unchanged. This establishes
survival under the explicit experimental clock fixture, not physical board
clock routing or an operational receive audio stream. Receive samples/DMA
remain unimplemented, and amplifier I2C NACKs still occur.

The endurance run averaged 32.9 host seconds per RCC virtual second.
A subsequent bounded performance experiment did not establish an end-to-end
speedup from caching external DSP instruction decoding and packet validation,
so those candidate changes were discarded. Timing the backend step calls in
that experiment measured 20.16 host seconds inside the DSP backend over
196.74 elapsed seconds; in a later 20-million-cycle window, approximately
3.88 of 28.65 host seconds were inside the backend. The remaining time is
outside those calls, not accounted for by DSP interpretation. These are
startup-window measurements, not a complete endurance-run profile. The
deterministic RCC `icount shift=0` profile and guest pacing remain unchanged;
the long runtime must not be attributed solely to the DSP interpreter.

The original RCC loader now creates its IPC device nodes and loads/starts
the unchanged DSP image through its own allocated mappings. TIMER10 is connected
to the existing counter/IRQ/clock/reset model. The power-clock experiment also
provides reset-only McASP configuration state: format, slot, clock-divider,
serializer and disabled FIFO configuration, with independent instances and
documented reset values. McASP1/2 have 16 serializers and McASP3-8 have four.
Clock-source selection while disabled does not enable a module or serial clock.
This configuration scaffolding does not implement the complete OCP clock-gating
protocol or active audio data path. McASP2 TX FIFO enable/status/write/flush
are modeled, but the FIFO does not drain.

McASP global-control writes record a pending reset-release request. Without
connected serial clocks, the synchronized global-control readback stays reset;
the requested value is not a readiness acknowledgement. Read-only machine
properties `mcasp1-reset-request` through `mcasp8-reset-request` expose that
distinction. PRCM module enable/disable and clock-source selection are
modeled; external receive serial clocks require the explicit fixture above.
Clock adjustment remains unsupported.
With experimental power clocks enabled, direct-start boot restores the
original IPL's McASP1/2/3/6/8 handoff: MODULEMODE=2 and AHCLKX source MLBP_CLK
(`0x0d000002`). Starting an IPL instead retains hardware-reset clock state
so that the IPL performs those writes itself. Enabling the module does not
release serial resets, create a MediaLB lock, or generate DMA events.
Mailbox EOI accesses preserve the existing level IRQ and FIFO conditions; they
do not consume messages or acknowledge application IPC.

An earlier fault-free capture exceeding 200 million native cycles was stuck in
a GateMP list traversal. Cache-aware execution tracing identified a backend
instruction-timing defect: predicated-off protected loads failed to generate
their four mandatory NOP cycles. A firmware-free regression fails before the
timing fix and passes afterward. The original loader now proceeds through the
observed DRA7 EDMA shadow-register setup and reaches 200 million native cycles
without a backend fault. That capture reports `ER=8`, `EER=0`, but it predates
removal of an unverified FIFO-enable event injection; the ER value is not
evidence of a firmware-generated McASP request. McASP2's write-FIFO remains
empty and its serial reset request remains pending. This proves neither an
EDMA transfer nor audio/watchdog health or sustained HMI.
In a post-FIFO-correction, pre-clock-handoff 22.6-million-cycle
isolated-loader capture, McASP2's
PRCM clock control is `0x0d030000`: the AHCLKX source selector is MLBP_CLK,
but IDLEST is 3 and MODULEMODE is 0. Firmware has requested TX high-clock
reset release (`XHCLKRST`), while EDMA PaRAM sets 3, 64 and 65 describe a
128-byte transfer from DSP memory to the McASP2 data port. ER, EER, QER and
QEER are all zero, as are QCHMAP entries; no transfer is active. The isolated
loader stops the original service manager, so the module/clock ownership path
must be checked with original startup before implementing transfer-controller
behavior. This capture is not audio-health or watchdog evidence.
The isolated loader probe stops the original service manager and is not a
paired host-attachment or audio-endurance test. Successful loader startup and
cycle accumulation are not successful audio IPC or watchdog endurance.
In a bounded paired startup with the original production service manager
retained, the DSP ran 86.677 million cycles without a backend fault. McASP2
still selected MLBP_CLK with MODULEMODE=0 and IDLEST=3; its TX FIFO was empty,
XHCLKRST remained pending, DRA7 EDMA ER was zero, and all eight QCHMAP entries
were zero. EDMA event-enable and QDMA-enable state were not captured in this
run. This 180-second diagnostic stop is not evidence of an audio transfer,
watchdog renewal or sustained HMI health. The capture is recorded in
`original-rcc-audio-dsp-analysis-20261110-b/native-rcc-integration.json`.

A bounded paired capture reached 86.684 million native DSP cycles without a
backend fault. The RCC log reported PCIe endpoint initialization, and MMX
enumeration found the TI `104c:b801` endpoint at `B3:D0:F0` behind `B2:D0:F0`;
the inactive link on bus 1 is a different bridge path, not evidence that the
RCC endpoint is absent. In that run, `devnp-mib-rcc.so` repeatedly reported
`rcc_attach(): RC is not initialized, loop.` Static disassembly shows
`rcc_attach` polls the PCIe subsystem dword at `0x5100002c` until it equals
`0xdeadbeef`. A later bounded capture passively observed the native MMX
initializer changing this value from the QEMU reset value `0x00010000` to
`0xdeadbeef`; PCI command memory-space enable and BAR0/BAR1 assignment appeared
at the same time. The unmodified MMX crossboard initializer exited
successfully, retry messages ceased, and the original `rcc0` interface was
configured with successful bidirectional ICMP checks. This resolves the
identified RC-ready predicate without a QEMU model change. Startup proceeds
past the retry loop, but the service manager later reports no
`StartUpResponse` from `MIC_ADC`. The same boot log records startup-response
timeouts for several other applications, so the `MIC_ADC` timeout is a
diagnostic lead, not an established audio root cause; the retained PID record
does not include an application-level process inventory. The DSP reached
164.752 million cycles without a backend fault, while McASP2 remained
module-disabled, its TX FIFO empty, and EDMA event/enable/QDMA state clear. The
paired run was forcibly stopped by the diagnostic wrapper timeout and does not
establish audio transfer, watchdog renewal or sustained HMI health. The
capture is recorded in
`original-rcc-audio-dsp-analysis-20261110-b/native-rcc-integration.json`.

A separate paired audio capture confirms that this short startup timeout is
not, by itself, evidence that the audio services fail to start: its RCC log
later records `MIC_ADC`, `SYS_LINK`, `PWR_AMP`, `WAV_PLAY`, `AUDIO_ASI` and
`Audio` startup markers. That capture still failed native health checks:
the first logged watchdog mismatch was `DIRANA3_APP`'s `SND_MON` at 144.110
seconds (5082 begins, 5081 ends), followed by `SYS_LINK`'s `SLK_CYC` at
161.043 seconds (172 begins, zero ends) and `PWR_AMP`'s `SND_MON` at 166.281
seconds. `SYS_LINK` later warned of a DSP watchdog at 219.486 seconds and
reset it at 229.671 seconds. This is an event ordering, not a proven causal
chain. In the paired run, a genuine six-byte SPI transaction completed but
returned six undriven zero bytes because no audio peripheral is modeled;
`PWR_AMP` also logged repeated I²C NACKs for address `0x6d` on controller 5.
These observations identify unanswered device interactions, not the correct
hardware responses. Keep these separate captures distinct: the PCIe-handoff
capture stopped before its later audio-service startup window.

The same older candidate also has a control with intermediate console dumps
disabled. It records no application-thread watchdog or SIGWD failures, but
still records DSP watchdog resets and a later orderly application shutdown.
This does not isolate console dumping as the cause of the earlier thread
failures, but it means the earliest DIRANA3_APP mismatch is not established
as an unavoidable startup failure. Its M4 UART7 log does record
`[IPC WD] REBOOT ! ! !` at M4 tick 61010, followed by reset requests and
the native transition from `ON_SWDL` to `IOC` at tick 61063, starting SoC
standby. A later native-DSP capture repeats the IPC watchdog reset request
at tick 61011 and the standby transition at tick 61091. These are M4
timestamps, not A15 log timestamps. Both retained traces have five retries
with expected challenge value still 1 and zero accepted-renewal branch hits:
the first valid reply never arrives, rather than successful renewals later
ceasing. An earlier capture has 37 accepted renewals and no IPC timeout.
The underlying first-reply failure remains unresolved; working PCIe
networking or missing A15 thread-watchdog messages must not be interpreted
as native watchdog health.

[hmi_readiness.py](hmi_readiness.py) optionally checks this independent
failure evidence with `--m4-uart /path/to/rcc-m4-uart7.log` alongside the
display console capture. A timestamped native IPC watchdog reboot, M4
fatal watchdog loop or M4 power-controller watchdog reset request fails
the check even if display initialization succeeded. Normal watchdog setup
and status messages do not fail it. An empty or short M4 capture contains
no such failure evidence, but is not a watchdog-health pass.

For RCC audio captures, add `--rcc-audio-log /path/to/rcc-boot-0.log`.
A native `[SLK_CYC]` timestamped `[SL_DSP_Watchdog]Reset: DSP WD Detection`
line fails the check even if display initialization or subsequent DSP replies
succeeded. A watchdog warning alone is not a reset. Command echoes are not
accepted as native reset records, and absence of reset records does not
establish sustained DSP or audio health. The same option rejects a native
`[PMR_RBT]` completed process-shutdown record. Use captures ending before
intentional guest shutdown when checking an endurance interval.

Short original-QNX controls show that the unchanged A15 `IPC_APP` can
initialize its `IPC_CYC` thread at its native priority 1, waiting on the
network stack. This takes approximately 3.3 host seconds in isolation and
4.3 seconds with original MOST running. The older failing audio thread
snapshot instead has thread 5 unnamed and READY. Scheduling/startup progress
is therefore a lead, but neither a demonstrated scheduler defect nor grounds
to raise firmware priorities. A separate bounded native-IPU1 control did
not reach an accepted renewal; loader release and service liveness are not
a successful watchdog exchange.

The RCC architectural counter uses the DRA7 COUNTER_REALTIME master
counter, nominally 6.144 MHz, not its 20-MHz SYS_CLK1 input. The original
QNX qtime syspage expects 6,144,000 cycles/second and 6,144 timer ticks per
millisecond. Previously the board configured 20 MHz: isolated, unchanged
QNX `sleep 1` and `sleep 2` consumed only 0.311 and 0.618 modeled seconds,
measured against the independent 32-kHz counter. With the board setting
corrected, they consume 1.000 and 1.996 seconds; the complete control takes
about five host seconds. The separate TIMER2 SYS_CLK1 counter remains
20 MHz.

This is a board clock correction, not a change to firmware watchdog limits
or priorities. Generic ARM timers currently round their period to integer
nanoseconds: at the nominal 6.144-MHz setting they use 162-ns ticks,
approximately 0.47% fast. The firmware-free
[test_rcc_counter_clock.py](test_rcc_counter_clock.py) executes guest
counter reads and a virtual-timer expiry, checking their rate against the
independent 32-kHz clock with this precision limit. The RCC qtest also
checks the configured frequency across reset and preserves the 32-kHz
and 20-MHz peripheral counter rates. Original IPC_APP still initializes
IPC_CYC at priority 1 alongside MOST after the correction, in a roughly
6.5-host-second control.

A separate bounded RCC-only native control, with the corrected clock,
original M4 firmware, original IPC_APP and the previously verified external
SubCPU wakeup replay, observes two genuine accepted watchdog replies:
challenge 1 receives 2, challenge 3 receives 4, and M4 advances its next
challenge to 5 with elapsed count reset. Both ring-ready words are set by
the original applications. This takes approximately 26 host seconds.
No internal IPC reply or readiness value is injected.

The isolated control must start IPC_APP after M4's original shared-ring
clear, not before loading M4. Starting A15 IPC first lets that later clear
erase A15's ready word; the resulting no-reply observation is a control
startup-order defect, not evidence that native transport is broken.
The paired production path already loads M4 before launching IPC_APP.
The paired probe also rejects timestamped native IPC reboot and
power-controller watchdog reset requests, even with healthy register
snapshots; it retains its legacy rejection of fatal application-watchdog
text without timestamps.

These controls establish a short native watchdog exchange, not audio
operation or sustained HMI health. Older captures using the incorrect
clock are not corrected-clock endurance evidence.

An initial corrected-clock paired control is deliberately capped at two
host minutes, with fatal M4/audio-watchdog checks active before HMI
control readiness. It reaches original RCC PCIe link enable and MMX
broker launch, but not HMI control readiness or an input/rendering check
within that budget. This is an incomplete startup observation, not a
watchdog-health pass or evidence of a firmware watchdog timeout. The
private bounded launcher initially reduced the RCC PCIe initializer
poll from 25 guest seconds to one; subsequent host-paced polling is
described below. Firmware startup, watchdog settings and priorities
are unchanged.
Paired controls must also verify current graphics-adapter source hashes:
stale diagnostic adapters are rebuilt, never accepted by bypassing their
provenance checks.

Two shorter 45-host-second startup-cost controls use the existing opt-in
TCG round-robin execution/idle counters. In their first measured RCC
windows, about 9.77 CPU seconds per 10 host seconds are spent inside guest
execution, advancing 1.31 and 1.29 guest seconds respectively. Changing
only the broad TaskManager diagnostic plugin to its IPC-only mode reduces
trace output from about 1.8 MiB to 16 KiB but shows no startup speedup.
Guest execution here includes device and plugin callbacks; these counters
do not identify a guest hot loop or prove instruction latency is the cause.
MMX initially executes heavily, then spends about 9.90 of a 10-second
window idle while its guest clock advances about 9.88 seconds. Its combined
storage mount/stage-two loading command takes about 14.3--14.6 host seconds;
this is not an isolated storage-throughput measurement. Neither shortened
capture reaches HMI readiness. Their explicit deadline failures must not
be treated as successful health checks, and narrowing the trace is not a
demonstrated performance fix.

Initial per-core profiles are superseded: a firmware-free shared-M4-block
test demonstrates that plugin callbacks can report the translating CPU
instead of the executing CPU under single-threaded TCG. The plugin code
generator incorrectly treats non-parallel execution as single-vCPU
execution and embeds a constant CPU index. The local debugging correction
loads the executing CPU index at runtime. The reproducer detects 781,221
wrong-core callbacks before the correction and none afterward.
An earlier inline-instrumented diagnostic caused an early M4 terminal
exception, including when host sampling was removed; those captures are
invalid performance baselines. That inline failure has not been retested
with the CPU-index correction and must not be assigned a root cause from
these observations. No inline instrumentation or host signal sampler is
enabled by the retained private profile option.

The corrected 45-host-second callback profile identifies CPU 0 as A15,
CPUs 1/2 as IPU1 M4, and CPUs 3/4 as IPU2 M4. Block-entry-weighted
instruction estimates are 835.3 million, 409.5 million, 711.2 million,
zero and zero respectively. These are not host CPU-time shares or exact
retired-instruction counts; callbacks add overhead and a block can exit
before all its instructions execute.

The corrected capture also resolves individual block starts within M4
page `0x99000`. Original Thumb disassembly places dominant blocks
`0x99bb0`, `0x99bc2` and `0x99bb6` in the shared-memory gate routine at
`0x99b98`: it writes a per-core byte, reads the word at the gate object's
offset 4, and retries when the word is greater than `0x100`. Core 1
(IPU1's second core, QEMU CPU 2) additionally executes three byte
load/store pairs before retrying. The caller at `0x9da18` checks a
per-core nesting value before entering this routine. Sparse register and
memory snapshots consistently identify gate object `0x805a3a9c` and retry
word `0x101`, with register core IDs agreeing with callback CPU indices.
CPU 1 records 730,919 entries and 730,919 acquisition-path hits; CPU 2
records 1,171,245 entries and 1,171,244 acquisition-path hits. They record
25,857,276 and 20,813,062 retries respectively. This is substantial
contention with continuing progress, not a permanently deadlocked gate
or the extreme imbalance suggested by the invalid earlier attribution.
It does not establish a scheduler defect or justify bypassing the gate.

For development startup, RCC instruction-count pacing is a useful
explicit experiment. A same-build isolated original-M4/IPC_APP control
verifies two real watchdog renewals (1 to 2, then 3 to 4) in 10.60 host
seconds at shift 2, versus 25.75 seconds at shift 0, approximately 2.4
times faster for this control. Neither challenges nor replies are
injected. This changes virtual nanoseconds per aggregate instruction,
not the hardware counter frequencies or firmware watchdog intervals,
and is not a measured target-CPU frequency or full-HMI speedup.
The firmware-free counter/expiry test runs at both shifts 2 and 5.
The existing paired-probe option is
`--experimental-rcc-icount-shift 2`; MMX remains at its separate default.
The private paired launcher now forwards an explicit
`--development-rcc-icount-shift=2` and records it in startup metadata.
Normal defaults remain unchanged. A 45-host-second paired check at
shift 2 advances RCC further but still does not reach HMI control
readiness, rendering or input acceptance.

Longer, bounded 120-host-second paired controls at RCC shifts 2 and 4
both reach native PCIe link-up, the MMX PCI server and MMX broker startup,
but neither completes the original RCC `ifconfig rcc0` query before the
deadline. Shift 4 is not an accepted development profile. Native PCI
server logs identify a separate 20-guest-second wait for PCIe port 0,
followed by "link is not active"; the `/dev/pci` wait takes about 21 host
seconds in both captures. This is not a storage-transfer measurement
or justification to force a disconnected link up.

The private quick-start harness now omits repeated RCC `sloginfo` dumps
while retaining the initializer process inventory, native process-liveness
and link checks, and network acceptance. An earlier shift-2 control reduced
the post-link capture from about 16 KiB to 3 KiB but still stopped at the
incomplete RCC interface query.
This is diagnostic-output reduction, not a demonstrated HMI startup fix.
Normal verbose diagnostics remain available, and no watchdog limits,
firmware priorities or default clock settings are changed.
The first bounded network-poll fix checked once per RCC guest second
instead of sleeping 15 guest seconds after a missing `rcc0` sample.
The verbose eight-sample, 15-second polling profile is unchanged. With
the native DSP running, the former sleep could outlast the entire
420-host-second diagnostic: only about 12 DSP-timer seconds advanced in
that window. This was an observer delay, not proof of a stuck network
driver. A fresh control with the shorter poll configured `rcc0` at its
second sample, passed both native ping directions, initialized the
original HMI display locally and reached live control setup near the
deadline. It still failed the 420-second input-verification diagnostic;
RCC display attachment, full readiness and the 120-second startup target
remain unverified. An optional private `--native-startup-waits` capture
observes the original threads after the first missing-interface sample;
the first capture found ProcessManager waiting on process loading,
before NETWORK_MGR had started. It does not change service priorities,
invent network interfaces or shorten firmware timeouts.
The short-poll repeat also completed a native touchscreen round trip
`100221 -> 262 -> 100221`, but its health observer reached the deadline
before accepting the result. A further control using the existing `--lean`
profile, without the heavy console/lifecycle and SYS_LINK instruction
tracers, completed rendering and reversible native input in 390.12 host
seconds and shut down normally. Both native ping directions passed; no
DSP backend fault was recorded. This passes the extended diagnostic only:
the unchanged 120-second startup target, RCC display attachment, native
RCC persistence and sustained readiness remain unmet or unverified.
A subsequent cycle-equivalent backend batching experiment also completed
both native ping directions and reversible input, in 396.14 host seconds.
It preserves per-cycle execution and first-fault handling but does not
demonstrate a startup speedup over the 390.12-second control.
An opt-in RCC display monitor now samples every guest second in bounded
mode (25 seconds in verbose mode), including native thread/register maps
and listeners. Its first capture observes the original `Display_ASI`
listening on `10.0.0.16:21436`, after an initial agent-start error followed
by successful retry. This excludes a permanently absent display listener,
not a late or otherwise unsuccessful MMX attachment. This heavier diagnostic
itself exceeded the startup window; it is not an input/readiness pass.
The monitor reader is now stopped before QEMU's console cleanup, including
timeout shutdown, so it cannot race a closed console during teardown.
A lean follow-up without IPC memory snapshots again passes both native
ping directions and the native input round trip, in 402.12 host seconds.
Removing those snapshots therefore does not demonstrate a startup speedup
either; the remaining delay cannot be attributed to that observer alone.
Passive host timing of every native console command further identifies
two one-guest-second RCC startup samples taking 29.60 and 39.23 host
seconds, a network-retry sleep/sample taking 56.54 seconds, and the original
two-packet RCC ping taking 61.59 seconds. The latter is a diagnostic
inter-packet wait, not evidence that the first packet or driver hung.
Bounded network checks now request two ICMP packets with 100 ms spacing
(`ping -c 2 -i 0.1`) while retaining the original strict two-reply validation
in both directions. Verbose checks keep their original one-second packet
spacing. This changes only the diagnostic workload, not guest firmware
timeouts or the emulated clock rate.
Both bounded RCC initializer and interface polls now sample immediately
and retry after 100 ms of host time, rather than issuing a guest sleep.
Retries stop after 120 attempts or 180 host seconds, whichever comes first;
each native console command retains its own timeout. The original process
identities and interface checks remain mandatory, and verbose diagnostic
guest sleeps are unchanged.
The final host-paced control completes both native ping directions,
original local HMI rendering and a reversible native touchscreen transition
in 325.47 host seconds, versus 400.63 for the fully timed control before
these changes (about 19% less elapsed time). The two transitions change
780763 and 921316 content pixels and complete in 3.77 and 2.43 host seconds.
The owner and all helpers exit cleanly. RCC ping still takes 44.09 host
seconds under native load despite the shorter requested interval, so not
all of its latency was the diagnostic timer. The 120-second startup target,
RCC display attachment, native persistence and full readiness remain unmet
or unverified; this is a measured diagnostic-wait improvement, not a full
emulator acceptance pass.
The missing RCC display attachment was a harness artifact. The 16550 model
writes one byte per chardev write. QEMUMachine's AF_UNIX console socket was
read only during harness commands, so it filled after a few hundred bytes.
The RCC UART then correctly held THRE clear (LSR 0x00, IER 0xf, timestamped
PCIe/UART trace), which blocked RCC console writers in the startup chain and
delayed the eso agents and `clientRCC_disp` until the next RCC command. The
harness now relays each console continuously into an unbounded host buffer
(disable with `--no-console-relay`). The guest stream is unchanged, and no
firmware, timeout or clock was altered. With the relay and the DSP alive,
the original MMX connection to `Display_ASI` (`10.0.0.16:21436`) becomes
established about 65 host seconds after rendering. Rendering and native input
remain verified (368.6 s including that wait), and all five RCC broker
connections to MMX port 21100 are present at the normal display check. A
normal relay control completes in 320.45 host seconds; at its single
display check, 21436 is not yet established. The 120-second target,
persistence and sustained readiness remain unmet or unverified.
An opt-in targeted RCC thread-inventory control also reaches only the
`pidin` table header before its deadline, so these captures do not
establish a network-driver deadlock specifically. RCC process-query
progress during concurrent native startup remains unresolved.

For a local development experiment, the paired probe accepts
`--experimental-concurrent-startup` together with `--network` (or `--asi`).
After the original MMX initializer exits and its post-link registers
verify, this resumes RCC while MMX starts its PCI server and performs
crossboard setup. The default retains frozen diagnostic checkpoints.
Pre-peer/reset-release sequencing and all native PCI, BAR, crossboard
and bidirectional-network checks remain required; this option neither
forces readiness nor changes firmware priorities or guest clocks.
Its selection is recorded in the probe result. Concurrent execution
alone is not evidence of full HMI readiness.
A bounded shift-2 control verifies both guests running during the MMX
PCI-server wait and advances native M4 ticks to about 25 guest seconds,
versus about 17 with the frozen checkpoints. MMX broker startup remains
about 89.5 host seconds after the first prerequisite in both controls.
A concurrent shift-4 control reaches about 78 native M4 guest seconds,
but still does not complete the RCC network query within 120 host
seconds. Neither combination is an accepted HMI development profile.
With the corrected UART and the redundant post-link inventory replaced
by checks that the original manager and IPC PIDs remain alive, a later
shift-4 control fails a native M4 application watchdog at tick 46100.
It is a real guest failure, not merely an observer timeout. At shift 2,
the original RCC interface query completes and MMX receives both ping
replies from RCC without packet loss. Quick mode now reuses that validated
RCC interface capture rather than querying it twice; it still requires
both native ping directions. The return ping does not complete within
the 120-host-second control, so bidirectional-network and HMI acceptance
remain incomplete.
The optional `mh2p_pcie_rpc` trace records the real operation, translated
peer address, transfer size, returned value/status and host nanoseconds
spent completing each PCIe RPC. It is off by default and can be enabled
per guest with QMP `trace-event-set-state`; it does not change transactions,
cache memory or manufacture completions. The firmware-free
[test_pcie_config.py](test_pcie_config.py) verifies actual reverse-memory
data, trace fields and runtime enable/disable behavior.
The firmware-free timer check additionally verifies WFI wakeup and
delivery of virtual-timer interrupt 27 through the GIC at shifts 2 and 5,
not only timer status-bit expiry.
Read-only native instruction-word capture identifies the frequently
sampled A15 PC `0xfc405758` as the instruction immediately after WFI
at `0xfc405754`, not a demonstrated busy-spin loop. The same capture
also observes later kernel execution. At the sampled idle points,
the GIC reports no eligible interrupt; the console UART has no
pending received byte. These observations narrow the diagnosis but
do not establish why native networking and console completion remain
delayed, or validate a ready-to-use HMI.

The local RCC UART model now gives its transmit FIFO the DRA7 depth
of 64 bytes instead of inheriting the generic 16550 core's 16 bytes.
[test_rcc_uart.py](test_rcc_uart.py) uses a real undrained Unix-socket
backend to create backpressure, verifies the full flag remains clear
through 63 queued bytes and asserts at 64, then checks the exact drained
byte sequence. It repeats after a system reset. The original model fails
the capacity assertion; the corrected model passes. This is a verified
transmit-capacity correction, not a complete OMAP UART implementation
or evidence that full HMI startup is fixed. Other boards retain their
existing serial FIFO behavior.

The opt-in RCC machine property `experimental-uart-efr=on` separates
the bank-B Enhanced Feature Register
(`LCR == 0xbf`, offset `0x08`) from the normal IIR/FCR alias. Previously,
the original driver read interrupt status as its feature settings and
wrote FIFO control when it intended to update EFR. A firmware-free
regression reproduces the incorrect `0xc1` reset read and verifies
independent EFR/FIFO state with byte/word accesses after both UART soft
reset and system reset. Original-QNX register traces now show EFR reads
of `0x00` and `0x50`, rather than FIFO-status bits.

The same opt-in also selects a 64-byte receive FIFO and standard DRA7
receive thresholds of 8, 16, 56, and 60 bytes. The serial core retains
its default 16550 thresholds for other users. Firmware-free loopback
tests check every threshold, exact FIFO capacity, overrun preservation,
and soft/system resets with the option both enabled and disabled.
Separating EFR alone exposed a coupled receive-threshold bug: the original
M4's `FCR=0x77` selected a four-byte 16550 threshold rather than the intended
sixteen-byte DRA7 threshold. Although all 40 original SubCPU wakeup-frame
bytes reached RHR intact, the native startup gate was not released.
With corrected receive geometry, the original M4 decodes the wakeup frame,
writes its own startup token, and the original manager again reaches its
PCIe initializer in the short matched control. The observer never writes
the token. This fixes the earlier experimental startup regression, not
full HMI acceptance. A subsequent paired control reaches native PCIe and
verifies both MMX-to-RCC ping replies, but still does not complete the
RCC return-ping command or HMI input verification within 120 host seconds.

Enhanced flow control and TCR/TLR/SCR threshold overrides remain
unimplemented; enhanced flow-control selections are explicitly logged.
The option remains experimental and off by default while those remaining
enhanced-register behaviors and paired HMI startup are investigated.

Short original-QNX controls verify both numeric and ordinary loopback
pings in about 1.1 host seconds without the stock manager workload.
With the original dual M4 firmware and IPC_APP running, two genuine
watchdog renewals and a loopback ping still complete; that ping takes
about 3.35 host seconds. These controls do not validate the paired HMI.
During a stalled paired startup, the native virtual timer is rearmed
12,053 times with 6,144-count steps and continues asserting interrupts.
Optional PCIe RPC tracing records 1,610 MMX transactions taking about
44 ms cumulatively and 6,764 RCC transactions taking about 82 ms,
all with successful transport status. These recorded RPC costs do not
explain a many-second stall; no peer-memory caching or synthetic
network replies were added.

A private console control confirms the original QNX shell enables Emacs
line editing. Disabling it on the RCC console, without changing service
priorities or firmware, allows one paired control to complete both native
ping directions and proceed to uploading MMX HMI runtime scripts. A repeat
with both consoles configured this way still times out during the RCC
return ping. Combining line-mode consoles with experimental EFR/RX
geometry receives the first RCC return-ping reply but does not complete
the second reply within the bound. This is not yet a reproducible startup
fix. Disabling shell editing alone also exposes the terminal's canonical
line-length limit: a longer observer command executes with its completion
token truncated and terminal bells in place of the excess characters.
A separate private control disables canonical input and terminal echo,
then verifies a 320-character assignment and its original shell exit
status. This permits long RCC commands without that truncation, but does
not establish a paired startup speedup. The early MMX image has no usable
`stty`; its corresponding terminal experiment can only run after stage two.

The 120-second HMI-input acceptance bound remains unchanged. A separately
labelled 300-second diagnostic completes genuine bidirectional ping and
the original Java HMI display wait, then is rejected after about 252 host
seconds by the original M4 IPC watchdog at tick 61616. Display connection
to RCC and native input remain unverified. The longer diagnostic is not
a pass of the shorter acceptance test, and no watchdog deadline or
service priority was changed.

Later diagnostic thread captures show the original IPC_APP responder
still unnamed and READY at priority 1, whereas isolated working controls
initialize it as IPC_CYC. The native load monitor also reports 100% A15
load. Pacing the ignition fixture against RCC guest time does not prevent
the IPC-watchdog reset. Returning to RCC instruction shift 0 leaves the
same responder READY and does not complete HMI input within a separate
300-second diagnostic; that shorter native-time capture is not evidence
of watchdog stability. The blocked responder initialization remains an
open issue; no firmware priority workaround was applied.

An exact-code-checked execution trace narrows this further: with RCC shift
2, the original responder enters its priority-lowering call but does not
reach the caller afterward before the native IPC-watchdog reset. A native
register capture places thread 5 READY at the return from that syscall,
before naming the thread or opening its socket. Virtual addresses alone
are not adequate trace identities here: unrelated QNX executables reuse
them, so the private tracer checks the original executable bytes as well.

A separate, explicit shift -2 instruction-budget control reaches the
caller's next instruction, completes the native naming call, and returns
a socket handle from the original socket-open call. No native priority,
counter frequency, or watchdog deadline is changed; firmware-free tests
also verify the counter rate, timer expiry, and interrupt delivery at that
pacing. This supports timing-induced starvation, not a proven socket
deadlock. It is not a usable-startup fix: the paired control still exceeds
the 300-host-second diagnostic bound, with M4 UART progress only through
tick 5222. That short native interval cannot establish watchdog stability.
The experimental pacing is not promoted to a default.

Subsequent native CPU-time samples identify substantial kernel and original
service work, with no idle-thread growth over the measured interval.
An interrupt capture records 64,315 architectural timer acknowledgements
over roughly 61.8 native seconds, not a gross interrupt storm. The remaining
experiment therefore changes the distribution of the instruction budget
between heterogeneous CPUs, rather than firmware priorities or timer rates.

The local, default-off `TCG_RR_FIRST_CPU_WEIGHT` experiment accepts an
integer from 1 through 16 and requires icount. It weights the first CPU's
share of the aggregate round-robin instruction budget; the other CPUs retain
unit weight. An unset option preserves the existing distribution. This is
not a cycle-accurate model of the hardware's clock ratio. Firmware-free tests
verify increased A15 progress with continued M4 execution, unchanged
architectural counter/timer behavior, and rejection of invalid options.
The private paired control applies weight 4 only to RCC, leaving MMX unchanged.

The first weighted paired boot, with RCC shift 0 and the explicit UART and
console experiments, completes both native ping directions, the original
Java display wait, and the original RCC display connection. It also verifies
reversible native RSTP touch with actual rendered content changes: screen
100221 to 262 and back, first visible changes after approximately 2.31 and
1.45 host seconds respectively. The original IPC initializer completes its
priority change, naming and socket opening; the original M4 accepts
challenge/reply pairs 1/2 through 13/14. These are native replies, not
injected watchdog responses.

This is a rendering/input breakthrough, not sustained health acceptance.
The run finishes after approximately 255 host seconds, exceeding the normal
120-second startup bound, and has insufficient native-time coverage to
establish watchdog endurance. The experiment remains opt-in. A separate
bounded endurance diagnostic requires repeated visual/input checks and
further original IPC renewals after at least 90 native seconds since boot
and 60 native seconds after control readiness. It cannot turn a slow startup
into a pass of the shorter acceptance test, and does not validate audio/DSP
or full HMI readiness.

The repeat completes the same reversible visual/input check, with first
changes after approximately 2.30 and 1.18 host seconds. It continues past
the earlier IPC failure window with 63 accepted original renewal samples
and roughly 74.4 seconds on the RCC native counter. It then fails explicitly
on the original `SYS_LINK` DSP-watchdog reset at timestamp 74.888, before the
90-native-second target or second input check. No DSP execution backend was
enabled in either weighted run. This confirms repeatable initial input and
longer IPC progress, but is a failed endurance control, not a stability pass;
the next comparison enables the existing real native DSP backend rather than
suppressing that watchdog.

That comparison again verifies reversible native visual/input changes.
The original DSP executable reaches 730.08 million backend cycles without
a backend fault or an observed execution reset, but the original
`SYS_LINK` DSP watchdog still resets at timestamp 74.174. The final RCC
counter sample is approximately 73.7 native seconds, with 62 accepted
original M4 IPC renewal samples. DSP-command handle errors appear earlier
in the original RCC log. DSP instruction execution alone therefore does not
restore the host/DSP service connection; tracing that connection and handle
acquisition is the next target. Neither endurance control passes, and no
native watchdog or health failure is excluded from the result.

Read-only, exact-code-checked tracing subsequently confirms that original
`SYS_LINK` IPC startup succeeds and its local receive queue is created,
but opening `dspapp_inmsgq` repeatedly returns -5 through
`NameServer_getUInt32`. The original `MessageQ_open` implementation stores
the invalid destination ID `0xffff`; later command sends encounter that
same invalid ID. The subsequent `appe_inmsgq0` open is not reached in these
captures. This is failure to obtain the DSP queue initially, not a queue
handle shown to be working and then lost.

The private native DSP tracer has positive controls at the original entry
and idle-callback dispatcher. These execute while the traced DSP queue-setup
code is not reached during the bounded input check. This narrows the next
investigation to DSP application initialization before queue publication;
it does not by itself identify a specific peripheral as the cause.
The original DSP resource trace reports rpmsg service registration, which
is not publication of the application MessageQ name.

A subsequent unsaturated startup trace reaches the original system-setup
completion and the McASP-output setup stage, including its initializer at
`0x9506ef00`, but does not show that initializer completing before the
bounded input check ends. Application queue setup is still not reached.
This identifies the next initialization path to trace, not yet a proven
serial-clock or reset-register defect. A fetched continuation address in
the DSP's branch-delay interval is not evidence that the callee returned.
The narrower initializer trace reaches the stream-open call at
`0x9506f174`, targeting `0x950db9c0` with the original `/mcasp1` name,
without reaching the later initializer instructions. This further narrows
the driver path; it still does not establish which nested operation blocks.
That diagnostic repeat fails its native-input check with
`Missing original diagnosis response`, so it is not another input or
stability pass.

Further read-only tracing identifies the actual stream driver callback at
`0x95066f20` and its McASP2 high-clock reset wait at `0x950cd2e0`.
The original code requests `0x200`, reads zero back, and enters an OS sleep
before polling again. Timer5 is routed through DSP event 67 to INT14,
but the original timer writes enable overflow interrupts and start
auto-reload with both counter and reload equal to zero. At its selected
20 MHz input, the first overflow is approximately 214.75 native seconds
away, not a millisecond scheduler tick. This is a separate initialization
lead: neither a shorter timer reload nor clock-reset completion is injected.
Subsequent successful input repeats still do not establish DSP watchdog
endurance.

The zero reload was reproduced outside QEMU using the unchanged original
DSP unsigned-64-bit division routine: 19,200,000 divided by 1000 returned
zero rather than 19,200. Local backend debugging identified two instruction
defects: reversed `.S` subtraction read the wrong operand fields, and the
C66x unconditional doubleword `OR` updated only the low destination word.
With both corrected in the separate private candidate, the original
division returns 19,200 and native startup writes Timer5 reload
`0xffffb500` itself. No timer period or firmware instruction is patched.
The corrected timer lets the original McASP reset wait advance through
its timeout path, reaching the original `bsp_mcasp.c` line 4530 assertion
and DSP exit loop. Thus the missing McASP clock/reset completion remains
a real next boundary; correct arithmetic alone does not publish the DSP
application queue or establish watchdog health.

An unsaturated, all-ARM-core bus trace finds no McASP2 clock enable in
QNX/IPU startup; the DSP only changes its clock-source selector. Local
inspection of the original IPL explains this: its clock setup explicitly
selects MLBP_CLK and enables McASP1/2/3/6/8 before QNX startup.
The experimental direct-start handoff now preserves those settings and
the PRCM model accepts valid module enable/disable writes. A native repeat
confirms McASP2 `0x0d000002`, with the firmware-generated Timer5 reload
unchanged. The DSP still reaches the original reset-timeout assertion:
MediaLB also fails its original lock poll, and no external MLBP clock or
frame source is modeled by default. These are distinct from PRCM module enablement;
neither lock nor serial reset readiness is fabricated.

The MLBP-to-McASP high-clock source divider (`CM_CLKSEL_MLBP_MCASP`,
`0x4ae06130`) retains the documented divide-by-1/2/4/8/16 selections;
reserved encodings and incorrect access widths are rejected. This
configuration register does not create an input clock. The final native
McASP2 configuration clears HCLKXM, bypassing its internal AUXCLK divider.
Here "external" is relative to the McASP IP: DRA7 integration routes
that input from PRCM's MCASP2_AHCLKX, whose selected source is MLBP_CLK,
not necessarily from an externally driven McASP package pin. See
SPRUHZ6L section 24.6.4.2.1 and table 24-421.
A separate native trace confirms that MOST releases INIC reset on
GPIO8 pin 19 before enabling its MediaLB interface, but lock still fails.

The public OS81110 hardware reference (Microchip DS60001289B, sections 7
and 12.1) distinguishes MediaLB operation from MOST network lock: local
INIC control messages remain possible with the network PLL unlocked.
MediaLB power-up availability depends on the INIC's persistent port/pin
configuration; its factory default is 3-pin at 512 times the frame rate,
not the 6-pin configuration requested by this RCC firmware. The original
P2865 update package specifically supplies OS81110 V01.02.05 firmware and
a separate customer-configuration image, rather than only naming that
family in generic support strings. Its packaged customer configuration
differs from the configuration embedded in the original MOST executable;
neither has yet been decoded into port/pin settings. Factory defaults must
not be assumed to describe this board. A future external clock/MediaLB
model must establish those settings and keep clock presence, frame synchronization,
MOST network lock, and control-message transport distinct.

For local clock-path experiments only,
`-machine experimental-power-clocks=on,experimental-mlb-clock-peer=on`
provides a nominal 98.304 MHz MediaLB clock while GPIO8 pin 19 releases
INIC reset. This is an explicitly selected 48 kHz clock-only peer, not a
decoded OS81110 customer configuration. PRCM source selection, module mode,
the shared high-clock divider and McASP bit-clock configuration determine
whether a serial clock reaches a requested reset transition. High-clock,
bit-clock and serializer reset bits synchronize after a serial-clock
interval. Transmit serializer release flushes its buffer and raises
per-serializer XRDY and aggregate XDATA for active transmitters. With
XBUSEL selecting CFG, buffer writes retain data and clear readiness
without entering the FIFO. XDATA supports W1C and its enabled interrupt routes through the IRQ
crossbar. Transmit state-machine release transfers each preloaded XBUF
into its shift register and makes the buffer ready for the next word.
An empty buffer raises XUNDRN and derived XERR, with W1C and enabled
interrupt routing. An underrun mutes the modeled shifter until the
transmit state machine is reset.
The DATA-port apertures accept aligned word writes with XBUSEL selecting
DATA and WFIFO enabled; all offsets within an aperture alias the same
FIFO port. With serializer DMA requests enabled, a complete
WNUMDMA group moves from the FIFO to active serializer buffers in index
order. Partial groups wait without consuming data. This service currently
requires WNUMDMA to equal the active transmitter count; other counts are
logged as unsupported.
Internal TDM frame generation supports 2..32 slots and 8..32-bit slot
widths in four-bit increments. Slot cadence is derived from the actual
selected serial clock, retaining fractional-nanosecond phase across slots.
Only active TXTDM slots transfer another word into each shifter, refill
serializer buffers and permit further DMA requests. TXTDMSLOT reports
the current slot. Clock loss cancels pending serial service; reset clears
the timer. This is word-level serial timing, not an audio output sink.
External frame input, receive state-machine operation, DIT and burst
timing remain unsupported; their reset requests are not acknowledged as
implemented behavior.
The experimental DSP EDMA path can fill WFIFO through the DATA aperture.
Programmable DSP DMA selectors and global or access-enabled shadow-region
channel enables gate transfers. The supported subset is AB-synchronized
frames with ACNT=4, BCNT=WNUMEVT (1..64), signed source/destination BIDX
and CIDX, and linked PaRAM descriptors. Non-static descriptors support
multiple frames; static descriptors remain limited to CCNT=1. Normal and
early final or intermediate completion can set shared EDMA pending bits and
pulse the global and shadow interrupt outputs. For early notification, the
model snapshots one request per McASP, updates PaRAM and signals completion
at submission, then reads and delivers the frame on a later timer step.
Chaining remains unsupported. Transfers wait for enough FIFO space and
preserve actual source words. With the DSP backend present, source reads use
MMU1 when enabled by
DSP_SYS_MMU_CONFIG bit 4, not the CPU's MMU0. Translated sources must resolve
to RAM; unsupported descriptors and source faults leave the FIFO unchanged,
latch a missed event, and log the failure. Reprogramming the descriptor permits
retry. A fault after early notification is logged and blocks that DMA path;
the transfer-controller error registers are not modeled. DMA servicing is
deferred in 1 ns steps, not calibrated bus timing or a general TPTC queue.
This is not general EDMA emulation.
No MediaLB frame lock, MOST network lock, MediaLB control replies or
completion interrupts, or watchdog responses are invented.
The option defaults off.
A bounded original-firmware repeat observes McASP2 clock-reset readback
`0x300` and successive native requests `0x200`, `0x300`, then `0x700`.
That clock-only repeat preceded the serializer-buffer implementation; the
DSP ultimately reached its original exit loop. It validates clock-path
progress, not completed McASP initialization, DSP application-queue
publication or watchdog endurance.
A subsequent native serializer repeat reads back `0x700`, raises XDATA
with serializer 0 empty, and advances to the original `0xf00` request
for transmit state-machine release. DSP EDMA shadow region 2 has channel
3 enabled, but the FIFO stayed empty before host DMA support was added.
That repeat preceded transmit state-machine support, and the DSP still
exited. Serializer progress is not DSP startup success.
A native repeat with the DMA subset fills WFIFO to 63 words after
preloading serializer 0, clears XDATA, and preserves the original
shadow-region-2 channel-3 enable and linked descriptors. The original
firmware still requests `0xf00` while readback remains `0x700`, then reaches
its exit loop. This validates DMA preloading, not transmit state-machine
operation, DSP queue publication or watchdog endurance.
A subsequent transmit-release repeat reads back `0xf00` and consumes the
next FIFO word (62 remain), without an underrun. It still exits before
frame generation was implemented, and its paired HMI input check timed out;
this is not a startup or stability pass.
With internal frame timing implemented, another bounded native repeat reads
back `0x1f00` and continues FIFO service without a reported transmit error.
Nevertheless the DSP reaches its original exit loop, and the paired HMI
input check again times out after 300 host seconds. The cause of this exit
was subsequently traced to an original DSP opcode exception at `0x950aeb38`.
An experimental DSP backend now supports the comparison, unsigned-to-float
conversion and reciprocal instructions reached in successive runs. The next
failure was a floating-point multiplication. The backend now has an opt-in,
diagnostic-only normal-range multiply subset; exceptional operands and
overflow/underflow explicitly fault rather than silently produce a result.
It subsequently reached `FSUBSP`, `MPYSP2DP`, and `FSUBDP`; bounded
diagnostics for those instructions are also available, but do not cover all
floating-point inputs. A bounded `MPYSPDP` diagnostic is also available in the
private backend; it rejects unmodeled floating-point inputs and conservative
pipeline hazards rather than claiming full instruction fidelity. A subsequent
bounded run reached `SPDP`; normal and zero conversions are now also modeled
experimentally, while special values still fault.
The next bounded run reached `FMPYDP`; an experimental normal/zero subset is
now available in the private backend. HMI input still timed out.
A later bounded run passed `FMPYDP` and reached `DPSP`; a normal/zero
conversion diagnostic is also available, but special inputs and extreme
results are not yet modeled. The next diagnostic reached `FADDSP`, for
which a bounded `.L` implementation is now available.
A subsequent diagnostic reached `ABSDP`; normal and zero inputs are modeled
experimentally, while special values still fault.
A further bounded run progressed past that fault and reached an unimplemented
`CMPLTDP` instruction. A bounded diagnostic for normal/zero inputs now reads
its operands in separate pipeline stages; special values fault explicitly.
A subsequent run passed the comparisons and reached `.L FADDDP`, for which
a similarly limited diagnostic is available, including exact zero-operand
handling. The following run reached `CMPGTDP`, which also has a bounded,
staged diagnostic; subsequent runs reached `DPTRUNC` and `INTDP`. The former
has an in-range normal/zero diagnostic, and the latter converts signed
integers exactly. A later run reached the `.S` form of `FSUBDP`, now
available with the same bounded subset as its `.L` form. The next run reached
the `.S` form of `FADDSP`, for which the same bounded diagnostic as the `.L`
form is available. The next run reached `CMPEQDP`; a staged normal/zero
comparison diagnostic is now available. Both network pings succeeded in
that run, but HMI control was not ready before the input verification
deadline and the DSP entered its exit path. The next run reached an
`FSUBDP` with a denormal operand.
With a shorter, still bounded diagnosis-listener poll, a later diagnostic
reached the original listener and completed one reversible native touch
round. This took 320 host seconds, exceeding the 120-second acceptance
deadline; the DSP still faulted on the same denormal operand. It is not
an HMI readiness or endurance pass.
The private backend now handles zero operands in `FSUBDP` and a bounded
`SPTRUNC` subset. A separate, default-off local assumption treats a normal
minus denormal `.L FSUBDP` like the documented `SUBDP` case; the `FSUBDP`
entry does not establish that equivalence. With that assumption enabled,
the original DSP progressed to `FSUBSP` with an infinity operand. A second,
independent default-off assumption handles that `.L FSUBSP` case as the
related `SUBSP` instruction; the `FSUBSP` entry does not establish that
equivalence. With both assumptions enabled, later diagnostics reached
same-sign infinity `FADDSP` and normal-times-infinity `MPYSP`. The private
backend now handles those documented cases and reached an infinity input to
`SPTRUNC`. Its documented infinity and finite-overflow saturation is also
implemented in the private backend. A subsequent diagnostic reached
`CMPGTSP`, then `INTSP` after that comparison was modeled. A signed `.L`
`INTSP` diagnostic is also available in the private backend; its `.S` form
is not modeled. A further diagnostic reached a packed signed-32 conversion
whose manual entry inconsistently describes its source. A third, separate
default-off assumption enables only exact pair conversions for local
investigation; inexact operands still fault. The next run reached `RCPDP`,
now modeled with a host-computed reciprocal seed that is not claimed to be
bit-exact to hardware. A later run reached equal-normal `FSUBDP` operands.
A fourth, independent default-off assumption enables cancellation using the
related FADDDP signed-zero rule, which FSUBDP's own entry does not state.
With these assumptions enabled, a subsequent run reached `CMPLTSP`, now
modeled as an S-unit comparison with its documented FAUCR flags. The next
run reached `INTDPU`, now modeled as an exact unsigned-to-double conversion.
A subsequent run reached `MPYDP`; the private backend has a bounded staged
diagnostic for normal/zero operands, with conservative resource guards.
With that candidate and all four opt-in assumptions enabled, a further
diagnostic reached a `SUBDP` encoding. It completed one reversible screen
round but exceeded the 120-second startup deadline; RCC `MessageQ_open`
failures remained. The DSP observer also exited with a connection reset during
shutdown, so the launcher reported failure.
The next candidate added a bounded `.L` `SUBDP` diagnostic and advanced to a
conservative `MPYDP` unit-overlap guard; the input round and pings again passed
only in the extended window. The observer shutdown-reset fix completed in that
run. A later candidate permits another `MPYDP` at E8 as specified by the
instruction scheduling table and advanced to `FSUBSP` with two zero operands.
A separate default-off diagnostic assumption uses the related `SUBSP` zero
rule; another independently gated assumption covers normal-minus-zero and
zero-minus-normal. Neither is established by the `FSUBSP` manual entry. The
next run reached ordinary `SUBSP`; the private backend now models a bounded
`.L` subset. A later run reached `ADDSP`, for which a bounded `.L` subset
has also been added. The next run reached McASP1 receive FIFO enable and
stopped on the unsupported write. With the experimental clock peer enabled,
the model now accepts a preconfigured receive FIFO while the receiver is in
reset, retaining an empty FIFO without inventing samples or DMA requests.
Receive serialization, input and DMA are still unimplemented. These remain
diagnostics, not full DSP support.
With that receive configuration, a subsequent capture kept the DSP running
through the input check without a recorded DSP exception. One reversible HMI
round and both network pings succeeded, but the diagnostic took 312.5 host
seconds and full readiness remained false. RCC logged earlier
`MessageQ_open` failures; successful DSP queue publication and RCC queue
opening have not been verified. Repeated McASP2/3 transmit DMA descriptors
used multiple AB-synchronized frames and final completion interrupts in
early-completion mode. The bounded path now models a separate submission
stage for those descriptors. In a native diagnostic, McASP2's descriptor
count and source advanced across linked buffers while the transmit FIFO was
serviced. The DSP queue-creation call returned a non-null handle, and a
separate SYS_LINK trace recorded successful opens of both `dspapp_inmsgq`
and the APPE queue after retries. This does not establish successful DSP
requests, audible output, DSP watchdog renewal, or full DSP/RCC readiness.
The earlier RCC `MessageQ_open` failures were from M4 and must not be
conflated with those SYS_LINK results.
An endurance diagnostic with the refactored DMA candidate again recorded
both SYS_LINK opens, one reversible input round, and 27 accepted original
M4 IPC renewal samples. It reached about 38.4 native seconds from boot
before the 1800-host-second diagnostic limit, short of the 90-native-second
target. The guest also logged a SYS_LINK `SLK_ALV` thread-watchdog warning;
the second input round and full readiness were not verified.
The experimental RCC interrupt crossbars now propagate a changed input only
to selectors using that source; programming or resetting a selector still
refreshes all outputs. In one sampled native comparison this reduced the
interrupt-propagation samples substantially and advanced the DSP from
63.1 million to 68.7 million cycles at roughly 302 host seconds. Both
runs completed a reversible input round, but the comparison is diagnostic,
not proof of a repeatable speedup or a 120-second acceptance pass.
The experimental display scanout now stops its fallback DMA read at the
first failing page rather than traversing the entire framebuffer after an
SMMU fault; it still reports the translation and window DMA faults. A
firmware-free regression reproduces the former fault-log storm and verifies
the bound. A subsequent native startup diagnostic passed one reversible input
round in 319 host seconds, but did not encounter that fault and did not
verify the RCC display connection. It therefore demonstrates neither a
native fault-path speedup nor complete readiness.
The DSP EDMA interrupt enable and pending registers now have shared global
state with region access masks, rather than independent shadow copies.
Normal completion pulses the DSP's global and region 0-3 event inputs and
the corresponding device interrupt crossbar inputs. Transfers and normal
completion happen synchronously; early completion precedes a later delivery
step. This does not model actual transfer-controller latency or full queueing.
A native diagnostic with the shared-register candidate completed one
reversible input round and both network checks, without a recorded DSP
exception, but took 338.1 host seconds and did not verify full readiness.
These assumptions are not claims of hardware fidelity.
Several diagnostic runs completed a reversible HMI round but still exceeded
the 120-second acceptance deadline.
These are not complete floating-point implementations.
DSP application startup and watchdog health remain unverified. The reciprocal
seed meets the documented precision bound but is not a bit-exact hardware seed.

Shortening RCC's round-robin instruction slices was also tested and
rejected: 1,000- and 16,384-instruction caps advanced only 0.84 and 1.09
guest seconds in a 10-host-second window and did not improve startup.
The experimental cap code and launcher hook were removed; no scheduling
quantum override remains from that experiment.

There is a separate, reproducible native RPC-forwarder startup failure.
Retained paired captures record `irpc_comm_forwarder` aborting in thread 13.
Its captured abort stack contains the return address of the original
listener constructor's failed-bind abort call. The original process manager
launches it with local IPv6 address `fd53:7cb8:383:3::73`; the original network
manager assigns that address through the vehicle `dm0`/VLAN path. The
captures report missing `dm0`, VLAN setup failure and an unreachable IPv6
gateway. Successful PCIe `rcc0` IPv4 ping does not validate this separate path.

A short isolated original-QNX control reproduced the abort without that
IPv6 address. With only a local loopback address fixture, the unchanged
forwarder instead retained TCP listeners on ports 42519, 42520 and 29171.
The controls took approximately four and five host seconds respectively.
They disable the original broker-wait option to reach listener creation
without booting the HMI; they do not prove broker connectivity, vehicle
Ethernet/VLAN emulation, watchdog endurance or that the forwarder causes
the HMI's visible stalls. No loopback alias is added to normal startup.

A separate original-driver attach control fails in approximately 2.4 host
seconds with `ti814x_findphy:895 - No PHY found` and `Unable to find a PHY`.
The GMAC scaffold deliberately completes MDIO transactions without an
external PHY acknowledgement; it does not implement full Ethernet TX/RX.
This identifies a missing device/transport path, not a reason to synthesize
PHY acknowledgements or to alias the vehicle address during normal startup.

The graphics bridge is explicitly diagnostic. Development helpers can
change copied configuration, add local persistence or load diagnostic
libraries; inspect their manifests and options before use.
[build_graphics_development.py](build_graphics_development.py) rebuilds
adapters in an existing diagnostic IFS and verifies source/resource hashes;
it is not a stock-firmware-to-HMI builder.

For a local GLES latency experiment, build separate baseline and candidate
directories from the same diagnostic IFS using
`--experimental-poll-spins 0` and `--experimental-poll-spins 4096`
(up to 65536). The budget permits that many additional MMIO polls before
sleeping on each reply chunk or bridge-lock acquisition. It never resets
while that wait is pending; after exhaustion the existing sleep and reply
timeout behavior remains. The default is zero.
The compiler commands and selected budget are recorded in the manifest.
Compare identical HMI actions, host CPU time, wall time, guest time and
watchdog health; a larger budget is not an established performance improvement.
Host-side reply waiting already avoids many guest sleeps.

A short original-QNX Screen-adapter check used two runs per budget, in
order 0, 4096, 65536, 65536, 4096, 0. Each performed 1000 synchronous
`glGetError()` queries and verified rendered pixels. Guest timer readings
were 377 microseconds per batch at zero and 378 at either nonzero budget;
all six frames had identical hashes. These are emulated guest-time readings
under icount, not host query latency. Whole-client host wall times ranged
from 0.167 to 0.202 seconds, without a demonstrated improvement. This
uncontended smoke check does not cover full-HMI transitions, bridge-lock
contention or watchdog endurance; polling remains disabled by default.

### Emulated RCC persistence service

The original MMX `rccpersistencebackend -r /dev/rstp/persistence` talks to
the RCC PERSISTENCE application on RSTP channel `0x60`. The native RCC path
is not available: the RCC M4 never enables its UartP link without its
vehicle peer. The MMX-side `mh2p-rstp` chardev therefore emulates the RCC
service on that channel. It is a model of the protocol, not the RCC
application:

- Reads subscribe and are answered with an update carrying the stored
  value. Accepted writes are stored and answered with the same update, as
  the RCC's `nvdPersis_Write_Update_Res_Uart` does. The backend refreshes
  mapped partition keys only from such updates; without the echo a
  successful write read back as the old value.
- `qom-set /chardevs/<rstp> persistence-file <path>` keeps values in a host
  file that survives a QEMU restart.
- Without a catalog, every missing key gets `PE_NO_SUCH_KEY`.
  [rcc_persistence_catalog.py](rcc_persistence_catalog.py) extracts the key
  catalog from your own RCC `/App/PERSISTENCE` ELF; load it with the
  `persistence-catalog` property. Then unknown namespaces and keys get
  `PE_NO_SUCH_NAMESPACE`/`PE_NO_SUCH_KEY` (writes to them are rejected), RCC
  built-in defaults are returned, and RCC-owned keys without a value get
  `PE_IO_ERROR`, like a fresh RCC.
- Keys owned by the RCC M4 (vehicle coding and identification) are served
  by the M4 itself on the real unit, from its NvM cache. The emulated M4 boots
  with a blank EEPROM and so serves its NvM ROM defaults. `--m4-image` with
  your own RCC M4 `app-ipu1.xem4` adds those defaults to the catalog (120 keys
  for this firmware). M4 keys without a default get `PE_NO_SUCH_KEY`; leaving
  them unanswered stalls MMX consumers, including the HMI display wait. Four
  keys still get `PE_NO_SUCH_KEY` at boot:
  - `0x5F22:0x2425` and `0x5F34:0x4500` are absent from the M4 routing table,
    and the real M4 answers them the same way.
  - `0x5F22:0x9CE` has no NvM mapping; the real M4 would answer
    `PE_IO_ERROR`.
  - `0x286F058C:0x2` is a live M4 value that is not modelled.
- `--provision-rcc` zero-fills every RCC-owned key that has no built-in
  default. This approximates a production-provisioned unit so the backend
  initializes its mappings; the zeros are not real calibration or coding
  data.

Example catalog command:
`rcc_persistence_catalog.py PERSISTENCE.elf catalog.txt --provision-rcc
--m4-image app-ipu1.xem4`.

Start the backend the way the firmware does. `rstp` must have received the
RCC INIT first: `main_stage2.2.sh` waits for `/tmp/rstpinitframe` before
`main_stage2.3.sh` starts the backend. If the backend starts earlier, its
open of `/dev/rstp/persistence` fails and it sleeps 5 s. Meanwhile its
4.5 s mapping timers expire, which logs "Mapping (0x2CC0121:0x190) could not
be initialized" and many "status SUBSCRIBED" lines. The RCC answered
promptly in that case; the launch order was wrong.

The backend types values from `persistence_mapping.json`. A mapping without
a type is a blob, and int writes to it fail with `PERS_STATUS_FAILED`; for
example partition `678364556` key `20` (mapped to `0x5F22:0x180B`) must be
written as a blob. In two private paired boots with the provisioned catalog,
a blob written through the original Persistence Service read back in the
same boot and again after a restart from the host file, and the HMI display
wait passed both times. With M4 defaults in the catalog and the original
launch order, a later private restart initialized every mapping without
errors and logged only the four `PE_NO_SUCH_KEY` keys above. That covers one
written key and default M4 data; other consumers, written M4 data and
endurance remain unverified.

### MOST video and cluster research

Static inspection of the original P2865 firmware confirms a native video
encoding path: display management uses the ASI VideoEncoding interface,
and the video encoder service imports IPTE displayable capture and
`NvVideoStreamEncoder*` APIs, with ISO transmit targets `/dev/mlb/isoTX1`
and `/dev/mlb/isoTX2`. Its NVIDIA library supports H.264 with Screen or
EGLStream input; the Screen encode entry converts its buffer using
`NvxScreenCreateNvMediaVideoSurfaceSibling`. The removed legacy bridge's
native-image interpretation was not verified against these Screen buffers.
Encoder settings and callback layouts also need verification before reuse.

MOST support does not establish that the cluster uses MOST in this variant.
The original PAG_HIGH_G35 production display-manager configuration maps
display 4, "Cluster Display", to LVDS2 (INTERNAL2), marks it optional, and
maps display 5, "HUD", to MOST. It separately declares HMI Cluster
displayable 41. These are configuration observations, not proof of an
active physical output or of every vehicle variant's routing.

The original RCC ISO driver also contains split-driver virtual-IP socket
interfaces and MOSTV0 through MOSTV4 endpoints. Its connection setup and
packet framing remain unverified; no synthetic PCIe BAR/channel constants
are established as an MH2P transport implementation. The next bounded checks are
to observe cluster displayable 41 on the Screen bridge, then verify the
native encoder ABI and RCC ISO handoff independently. No end-to-end MH2P
MOST/cluster-video emulation is claimed.

CarPlay, Bluetooth, Wi-Fi, trusted-kernel and physical-unit ModKit helpers
are research tools, not supported preview features. No active CarPlay
backend, phone compatibility or physical-unit installation safety is claimed.
The separately generated CarPlay ModKit package and all firmware-derived
payloads are excluded from emulator publication.

Release follow-up remains: resolve native watchdog failures, prove
endurance and cold-start behavior, consolidate a user-reproducible paired
launcher, then validate a clean build and honestly scoped release.
