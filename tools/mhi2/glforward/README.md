# Experimental MHI2 GLES forwarding

This local prototype replaces the extracted main IFS `libEGL.so` and
`libGLESv2.so` in separate experimental media. It forwards the firmware's GLES
calls to a Mesa surfaceless EGL pbuffer via an optional QEMU MMIO aperture at
`0x5f000000`. The normal media and original firmware archive are preserved.

The MMIO implementation is now a shared
[optional device](../../../hw/misc/glbridge.c), used by both the MHI2 and MH2P
boards. Existing `-chardev ...,id=glbridge` activation and wire semantics are
unchanged. The [MH2P graphics check](../../mh2p/check_graphics.py) can also run
with `--machine mhi2-harman` using synthetic firmware-free fixtures; it verifies
transport backpressure, exact Mesa-rendered pixels, readback and idle reset.
This regression does not establish compatibility of the MHI2 guest shim with
MH2P's native windowing interfaces.

The separate [MH2P QNX smoke client](../../mh2p/README.md#qnx-pbuffer-smoke-client)
also verifies a real guest process using a private copy of this shim. It exposed
and fixes an initialization-order bug: opcode 1 dimensions must be applied
before selecting the first nonzero PID's context, otherwise that context uses
the default 800-by-480 size. Existing default dimensions and wire format remain
unchanged. The ImageFS resource helper now excludes the three inode processing
flags when allocating a new inode; runtime-loaded additions must not inherit
bootstrap/preprocessed-ELF flags from existing boot files.

The starting source was the existing workspace `mib/glforward` prototype.
The current host process communicates over stdin/stdout and filesystem FIFOs;
it does not require a guest network driver or a host listening socket.

Local build: `bash tools/mhi2/glforward/build_local.sh`. This uses the installed
ARM cross compiler. Set `MHI2_WORKSPACE` to your private media workspace,
containing the already extracted `main-ifs/`. The media builder uses the
repository's read-only QNX6 reader and requires
`k3342-full-debug-nor.bin`, plus the prepared/guest-formatted `emmc-full.raw`.
The generated files are `k3342-gl-debug-nor.bin` and `emmc-gl.raw` in that same
analysis directory. Never modify them while a VM is using them.

`fast_startup_local.py` shortens the experimental HMI's DSI/domain timeouts to
1/5 seconds; its purpose is to let startup advance past missing companion
services during rendering tests. The original settings are 20/120 seconds.
It requires `MHI2_EMMC` pointing to a disposable disk and modifies that
disk in place. It is not part of the supported launcher or MH2P readiness tests.

`debug_shell_local.py` launches the current diagnostic image. Set `MHI2_GL=1`,
`MHI2_DEBUG_NOR` and `MHI2_EMMC` to select experimental media. It reads shell
commands from `/tmp/mhi2-diag-commands.txt`; set `MHI2_MEDIA_ROOT` to your
prepared workspace and `QEMU_SYSTEM_ARM` to your binary if it is not in the
checkout's `build/` directory. The image must contain the diagnostic
startup script. Set `MHI2_TIMEOUT` for the host wall-clock deadline. Logs are
saved under the analysis directory's `qemu/runs`.

The host writes `/tmp/glhost_frame.ppm`, shader source captures and diagnostic
logs. Raw command capture is opt-in with `MHI2_GL_CAPTURE=/tmp/mhi2-gl-stream.bin`;
`MHI2_GL_TRACE=1` enables per-command logging and `MHI2_GL_DUMP_CLIENTS=1`
enables duplicate per-client frame dumps. These expensive diagnostics are off
during normal use. Production RCC key-panel input now operates the
radio UI; see [RCC_INPUT.md](../RCC_INPUT.md) for the mouse viewer and evidence.

`MHI2_GL_OPSTATS=1` makes the host renderer log per-opcode counts and handling
time every 10 seconds. With `GLBRIDGE_PROFILE=1`, QEMU's `glbridge profile`
line also reports `w40-wait` (time blocked on the renderer's readback reply)
and `w40-swizzle` (in-place swizzle, only on host-contiguous surfaces).

When QEMU native scanout supplies the display, set `MHI2_GL_NO_FRAME_DUMP=1`
on the host renderer to skip the redundant default PPM preview. This avoids
an additional full-frame GPU readback, CPU copy and file publication on
every display swap; it does not skip swaps, guest readbacks or guest surface
updates. The default preview and its video overlays are not available in
this mode. Explicit per-client dumps and separate cluster-video output
remain independent diagnostics. The default remains preview-enabled for
file-based viewers. Retained PPM outputs pack RGB by row, preserving row
orientation and viewport stride. Input records use a buffered command
stream while large pixel payloads bypass the staging copy.

Fast checks (no firmware boot):

```sh
bash tools/mhi2/glforward/build_host.sh /tmp/check-glhost-io \
    tools/mhi2/glforward/host/check_glhost_io.c
/tmp/check-glhost-io
bash tools/mhi2/glforward/build_host.sh /tmp/mhi2-glhost
python3 tools/mhi2/glforward/check_host_io.py /tmp/mhi2-glhost
bash tools/mhi2/glforward/build_host.sh /tmp/check-pixmap-images \
    tools/mhi2/glforward/host/check_pixmap_images.c
EGL_PLATFORM=surfaceless /tmp/check-pixmap-images
```

The real-Mesa pixmap check also exercises the accelerated 128-by-128 readback
with a nonzero array-buffer binding, verifies exact pixels and both buffer
bindings, then performs a small CPU readback. The acceleration must save and
restore `GL_PIXEL_PACK_BUFFER_BINDING`, not `GL_ARRAY_BUFFER_BINDING`: restoring
the latter as a pixel-pack buffer corrupts state and breaks later CPU readbacks.
This firmware-free regression does not establish publication of the original
MH2P haptic displayable or delivery of raw CarPlay contacts.

The C check covers fragmented/multi-megabyte input, truncation and exact
strided/flipped PPM bytes. The renderer check verifies preview opt-out
without changing GL readback pixels. Use the normal Mesa environment to
choose hardware rendering, or `LIBGL_ALWAYS_SOFTWARE=1` for software GL.

The opt-in MH2P Screen lifecycle supports up to eight independent ES2/ES3
contexts per process, with separate object maps, shader metadata and errors,
per-thread current bindings and eight independently sized drawables. Context
sharing, cross-context native-image imports and many extension entry points
remain unsupported. Legacy MHI2 clients retain their existing per-process path.

The native display manager submits precompiled NVIDIA shaders. `known_shaders.h`
contains experimental GLSL translations selected by binary length and FNV-1a
hash. Vertex arithmetic follows disassembly; fragment blending is reconstructed
and does not reproduce FX10 quantization. Unknown binaries fail compilation.
Original captured binaries are in the analysis directory's `qemu/shaders`.
The Java HMI submits its own GLSL source, which is forwarded unchanged.

References: [Grate shader decoder](https://github.com/grate-driver/grate/blob/master/src/libcgc/shader.c),
[fragment ISA](https://github.com/grate-driver/grate/blob/master/docs/fragment-shader-isa.md),
[NVIDIA EGL API interface](https://github.com/Project-Google-Tango/vendor_nvidia_jxd_src/blob/master/tegra/gpu/drv/drivers/khronos/egl/interface/partner/eglapiinterface.h).

Native cockpit bring-up now forwards the display manager's EGLImage to the host
encoder and back to the original MOST driver. See [MOST.md](../MOST.md) for
transport evidence and remaining rendering limitations. Navigation shader
translations additionally cover solid fill, vertex color, textured source-over,
linear fog and extruded road geometry. `check_navigation_shaders.py` loads the
captured native binaries through the bridge and checks rendered fog/alpha pixels
against the decoded four-operand Tegra MAD arithmetic. Override
`MHI2_SHADER_CAPTURE` and `MHI2_GLHOST` to select captures and renderer binaries.

Graphics clients retain separate host state by process. The native display
manager identifies its physical output window, so only its swaps update the
head-unit viewer file. Per-client captures remain available for diagnostics;
the cockpit window exclusively consumes the decoded MOST stream.
