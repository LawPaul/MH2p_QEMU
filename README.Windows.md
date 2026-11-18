# MH2p QEMU on native Windows

This branch adds a native Windows build of the MH2p emulator, an ANGLE (Direct3D 11) host renderer
for the GL bridge, and a live HMI window with touch input. It builds on the work in
[README.md](README.md); read that first. Everything here is experimental, and the same limits apply:
no firmware is included, and nothing here proves a working head unit.

Tested with firmware profile **MH2p_ER_POG35_P2865** (MMX variant 22) on Windows 11 with
Intel Iris Xe graphics.

## What this branch adds

| Area | Change |
|---|---|
| Build | Native MSYS2 UCRT64 build; symlink-free install-tree fallback; Windows compile fixes |
| MMX↔RCC pairing | PCIe config/outbound links over TCP (`listen:HOST:PORT` / `connect:HOST:PORT`), because Windows cannot pass inherited socketpair fds |
| GL bridge | Synchronous replies over a TCP **socket** chardev on Windows (no FIFO chardev there) |
| Display stack | Opt-in `experimental-permissive` modes (GK20A, DC, host1x, SOR, VIC); GK20A BAR1 + GMMU walk behind the Tegra SMMU; PMC DPD registers. With these, the original `devg-nvrm`, `nvdisp`, `screen` and `displaymanager` come up |
| RCC | McSPI clocks start enabled when IPU1 firmware is loaded directly, as the bootloader leaves them (fixes the M4 `T_M4_Accl_Ctrl` bus fault) |
| Renderer (`glhost`) | ANGLE default display; TCP transport (`MH2P_GL_CONNECT`); opt-in CPU ASTC decode (`MH2P_ASTC_SOFTWARE=1`) because ANGLE/D3D11 has no ASTC; opt-in live window with touch (`MH2P_GL_WINDOW=1`, `MH2P_GL_INPUT`) |

The permissive display modes, software ASTC decoding and live window are opt-in.
Other changes are active by default, including the TMP451 temperature sensor,
the BCM4359 PCIe model, DSP1 reset-release handling, and TCG TLB changes.
The Windows port does not establish full head-unit readiness or working audio.

## Build QEMU

In an MSYS2 **UCRT64** shell:

```sh
pacman -Syu          # run on its own first; it restarts the runtime
pacman -S --needed git mingw-w64-ucrt-x86_64-{toolchain,meson,ninja,glib2,pixman,pkgconf,python}

mkdir build-win && cd build-win
../MH2p_QEMU/configure --target-list=arm-softmmu --disable-docs --disable-gtk --disable-sdl \
  --disable-werror --disable-tools --disable-guest-agent --disable-rust
ninja qemu-system-arm.exe
```

Put `C:\msys64\ucrt64\bin` on `PATH` when running `qemu-system-arm.exe` outside the MSYS2 shell.

## Build the renderer (glhost) with ANGLE

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-angleproject
```

CPU ASTC decode needs [ARM astc-encoder](https://github.com/ARM-software/astc-encoder)
(Apache-2.0, tested with 5.2.0), cloned outside the repo, for example to `../astc-encoder`:

```sh
A=../astc-encoder/Source; O=../build-glhost/astc; mkdir -p $O
for f in $A/astcenc_*.cpp; do
  g++ -O2 -std=c++17 -DASTCENC_DECOMPRESS_ONLY -DASTCENC_SSE=41 -DASTCENC_AVX=0 \
      -DASTCENC_POPCNT=1 -DASTCENC_F16C=0 -DASTCENC_NEON=0 -msse4.1 -mpopcnt \
      -I$A -c $f -o $O/$(basename $f .cpp).o
done
g++ -O2 -std=c++17 -I$A -c tools/mh2p/glforward/host/astc_soft.cpp -o $O/astc_soft.o
ar rcs $O/libastcsoft.a $O/*.o

gcc -O2 -D_GNU_SOURCE -DGLHOST_ASTC_SOFTWARE tools/mh2p/glforward/host/glhost.c \
  -o ../build-glhost/glhost.exe \
  $O/libastcsoft.a \
  -lstdc++ -lEGL.dll -lGLESv2.dll -lws2_32 -lgdi32 -pthread -lm
```

Without astc-encoder, leave out `-DGLHOST_ASTC_SOFTWARE`, `astc_soft.cpp` and `libastcsoft.a`. The
HMI then renders without its ASTC artwork on hosts that have no native ASTC.


## Prepare media

Use the repo's tools as described in [README.md](README.md): `tools/mh2p/prepare.py`,
`prepare_nor.py`, `prepare_rcc_nor.py`, and an mNAND image laid out like the firmware's
stage-two `/etc/fstab`. Keep firmware and generated media **outside** the checkout.

## Run: paired MMX + RCC

Pick free local TCP ports. Below, `P_SB`, `P_CFG`, `P_OUT` are the PCIe sideband, config and
outbound links; `P_GL` is the GL bridge; `P_QMP` is QMP for window input.

MMX (start first; it listens):

```
qemu-system-arm -M mh2p,pcie-fd=listen:127.0.0.1:P_CFG,pcie-outbound-fd=listen:127.0.0.1:P_OUT \
  -kernel <mmx-media>/startup.bin -dtb <mmx-media>/board.dtb -bios <nor>/nor.bin \
  -drive if=sd,index=0,format=raw,snapshot=on,file=<mnand>/mnand.bin \
  -display none -nic none -icount shift=0,align=off,sleep=on \
  -global gk20a.experimental-engine-init=on -global gk20a.experimental-permissive=on \
  -global tegra124-sor.experimental-configuration=on -global tegra124-sor.experimental-permissive=on \
  -global tegra124-dc.experimental-window-scanout=on -global tegra124-dc.experimental-permissive=on \
  -global tegra124-vic.experimental-execution=on -global tegra124-vic.experimental-permissive=on \
  -global tegra124-host1x.experimental-permissive=on \
  -chardev socket,id=mh2p-pcie-sideband,host=127.0.0.1,port=P_SB,server=on,wait=off \
  -chardev socket,id=glbridge,host=127.0.0.1,port=P_GL,server=on,wait=off \
  -qmp tcp:127.0.0.1:P_QMP,server=on,wait=off \
  -chardev mh2p-rstp,id=rstp -serial chardev:rstp \
  -serial tcp:127.0.0.1:<console-port>,server=on,wait=off
```

RCC (connects; retries for up to 120 s):

```
qemu-system-arm -M mh2p-rcc,pcie-fd=connect:127.0.0.1:P_CFG,pcie-outbound-fd=connect:127.0.0.1:P_OUT,\
experimental-ipu1-firmware=<fw>/filled-dra7-ipu1-fw.xem4,experimental-ipu1-smp=on \
  -kernel <fw>/ifs-dra74x-vayu-evm.bin -display none -nic none -icount shift=0,align=off,sleep=on \
  -chardev socket,id=mh2p-pcie-sideband,host=127.0.0.1,port=P_SB,reconnect-ms=200 \
  -drive if=pflash,format=raw,snapshot=on,file=<rcc-nor>/nor.bin \
  ...   (devices, EEPROM and serial ports as in README.md)
```

Serial ports:
- MMX `serial0` is the RSTP link to the emulated RCC side (`mh2p-rstp`); `serial1` is the console.
- RCC `serial2` is its console.

Renderer, started alongside (Windows `cmd`):

```
set MH2P_GL_CONNECT=127.0.0.1:P_GL
set MH2P_GL_FRAME=<out>\frame.ppm
set MH2P_ASTC_SOFTWARE=1
set MH2P_GL_WINDOW=1
set MH2P_GL_INPUT=127.0.0.1:P_QMP
glhost.exe
```

### Live window, touch and ignition

- `MH2P_GL_WINDOW=1` shows each published display frame in a window titled
  "MH2P HMI (glhost)". glhost still renders offscreen; the window is a GDI view of the same frame.
  While the window is open, `MH2P_GL_FRAME` is rewritten at most once per
  `MH2P_GL_FRAME_INTERVAL_MS` (default 1000; 0 = every frame). Without the window, every frame is
  written.
- With `MH2P_GL_INPUT` pointing at the MMX QMP socket:
  - left-button press, drag and release are sent as absolute pointer events
    (`input-send-event`);
  - the `mh2p-rstp` chardev turns these into native RSTP keypanel touch records (channel 0x40)
    for the original `hidcontroller`, so no guest change is needed;
  - the mouse wheel is the volume encoder;
  - Esc, Backspace, Enter, Home, the arrow keys, PgUp/PgDn and +/- are sent as keys.
- **Ignition:** the `mh2p-rstp` chardev's `clamp-state` property (bit 0 = S, bit 1 = KL15) reaches
  the original OOC. Set it over QMP:
  ```
  {"execute":"qom-set","arguments":{"path":"/chardevs/rstp","property":"clamp-state","value":3}}
  ```
  The guest logs `Clamp State: S 1, 15 1`.

## Running the original HMI

Reaching the original HMI needs:
- a car-sized `misc1` partition with the HMI resources, and
- guest-side preloads and boot-script overrides (glbridge EGL for `displaymanager` and the J9 HMI,
  shared bridge regions for the unprivileged HMI, emulation-scaled HMI timeouts).

That launcher tooling is **not included** here.

If you build your own, two guest settings matter:
- `GLBRIDGE_FAST_SURFACES=1 GLBRIDGE_FAST_PUBLISH=1` for every `libmh2p-screen.so` client.
  Without them, each frame does a full NvRmSurface write and verify readback, which starves the
  guest UART driver. The RSTP link, and with it touch input, then stops a few minutes after the
  HMI appears.
- The HMI takes several minutes of wall time to appear under `-icount`, and touch responses take
  tens of seconds.

## Known gaps

- **Speed:** one vCPU under `-icount`, on the default configure build (debug info, QOM cast checks). Untried options: multi-threaded TCG
  with `-smp 4` without icount on the MMX, and a release build.
- **Services:** services stay "Starting…". OOC logs "service manager alive timeout" and asks the
  RCC to reset the MMX (`RESET_TYPE_FAST`).
- **IPC:** no C66x DSP model, so IPC between the chips stays down.
- **Wi-Fi:** no BCM4359, so PCIe port 1 stays down.
- **GPU:** no FECS/GPCCS Falcon, so GPU power-on fails cleanly. Rendering goes through the GL
  bridge instead.
- **Hardware drivers:** the thermal, DVFS, lane-mux, audio (APB DMA), USB/xHCI and SDMMC3
  drivers fault on unmodelled hardware.
- **Video:** the cockpit MOST video encoder is not ported to Windows.
- **Missing data:** no navigation map data and no factory persistence values.
