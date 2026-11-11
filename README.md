# MH2p QEMU - work in progress

**This is an experimental work in progress, not a complete or supported
MH2P head-unit emulator.** This source preview is intended for developers
and researchers. It does not include proprietary firmware, prepared disks
or the complete paired MMX/RCC HMI launcher.

## Status

The tested firmware profile is Porsche **MH2p_ER_POG35_P2865**, MMX variant
**22**. Other firmware versions and hardware variants are not validated.

- Original QNX startup and early userspace run on experimental MMX and RCC
  machine models.
- Private diagnostic controls have displayed the original HMI and exercised
  reversible Home/Media input cycles and bounded RCC networking.
- Graphics are diagnostic bridge-assisted; native GPU command rendering
  is not implemented.
- Watchdog failures, incomplete audio/backend services and variable
  performance remain. Sustained HMI health is not established.
- A complete, standalone paired HMI launcher is **not included**.

The source was clean-built and firmware-free tested locally on Linux.
MH2P macOS support has not been verified. Reaching early userspace or passing
synthetic tests does not establish a working head unit.

## Getting started

```sh
git clone https://github.com/LawPaul/MH2p_QEMU.git
cd MH2p_QEMU
```

See the [MH2P guide](tools/mh2p/README.md) for out-of-tree build instructions,
firmware preparation, diagnostic shell usage, tests and model limitations.
Supply your own legally obtained firmware and keep all extracted assets,
prepared media and captures outside the checkout.

Do not install generated diagnostic media on physical hardware. CarPlay
and other research helpers are not supported end-user features, and no
firmware-derived ModKit payloads are distributed here.
