# MH2P diagnostic graphics bridge

These are the guest shim, host renderer and regression sources reused by
MH2P diagnostics. They were relocated from the former MHI2 tooling; the old
MHI2 media builders and launchers are not retained.

This is an experimental host GLES/EGL forwarding path, not native Tegra GPU
emulation or a complete HMI launcher. See the [MH2P guide](../README.md) for
the current evidence and limitations.

Build the host renderer from the repository root:

```sh
bash tools/mh2p/glforward/build_host.sh /tmp/mh2p-glhost
```

The build requires a C compiler and pkg-config packages `egl` and `glesv2`.
FFmpeg is not required. The default output is
`${MH2P_BUILD_DIR:-/tmp}/mh2p-glhost`.

The firmware-free transport/pixel regression uses a separate output directory:

```sh
python3 tools/mh2p/check_graphics.py \
  --qemu build/qemu-system-arm --renderer /tmp/mh2p-glhost \
  --output /path/to/new-diagnostic-output
```

Guest adapter builders are [build_graphics_smoke.py](../build_graphics_smoke.py)
and [build_graphics_development.py](../build_graphics_development.py).
They require an ARM cross compiler; preparing a diagnostic guest additionally
requires separately supplied private assets.

Active graphics controls are `MH2P_GL_FRAME`, `MH2P_GL_NO_FRAME_DUMP`,
`MH2P_GL_CAPTURE`, `MH2P_GL_TRACE`, `MH2P_GL_DUMP_CLIENTS`,
`MH2P_GL_SLOW_MS`, `MH2P_GL_OPSTATS` and `MH2P_SHADER_CACHE`.
The index regression additionally accepts `MH2P_GLHOST`.
Old `MHI2_*` controls are no longer read; update private diagnostic launchers.

The Screen adapter retains the MH2P window, pixmap, ASTC and verified program
cache paths. The non-Screen smoke adapter supports pbuffers only and reports
unsupported native-window/image requests explicitly. Removed paths include
the Tegra3 CWM adapter, legacy codec/cluster forwarding, hard-coded MHI2 shader
translations and menu-artwork overrides. Their retired protocol records are
rejected rather than interpreted as MH2P services.
