# Toolchain

The firmware builds with **ESP-IDF v6.0.2**. Nothing exotic — but two machine-specific notes have burned time before:

- ESP-IDF v6 requires Python ≥ 3.10. If the system `python3` is older, make a newer one resolve first in `PATH` before sourcing `export.sh`.
- `cmake` and `ninja` can be installed as pip wheels inside the IDF virtualenv instead of via a system package manager — useful when Homebrew is broken or owned by another user.

Typical activation:

```sh
. ~/esp/esp-idf/export.sh
```

`ffmpeg` (for the sound pipeline) can also live as a pip wheel: `pip install static-ffmpeg`.

## Navigation

Prev: [Sounds](../runtime/sounds.md) · Next: [Build](build.md)
