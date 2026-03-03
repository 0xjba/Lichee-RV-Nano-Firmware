# LicheeRV Nano Camera

1920x1440 camera stack for LicheeRV Nano with GC4653 4MP sensor. Includes MJPEG streaming, single capture, and an AI vision assistant triggered by the USER button.

## Binaries

| Binary | Description |
|---|---|
| `CSICapture` | Single 1920x1440 JPEG capture |
| `CSIStream` | MJPEG stream (grayscale) on port 7777 |
| `CSIHiResStream` | MJPEG stream (color 1920x1440) on port 7778 |
| `VisionAssistant` | Button-triggered AI vision via OpenRouter |

---

## Requirements

- LicheeRV Nano W with GC4653 sensor
- LicheeRV-Nano-Build toolchain at:
  `~/LicheeRV-Nano-Build/host-tools/gcc/riscv64-linux-musl-x86_64`
- ret7020's middleware blobs (included in `libs_patch/`)

---

## Build

```bash
./build.sh
```

Builds patched opencv-mobile-4.13.0 from source, then builds all binaries against it.

---

## Deploy (Development / Without Firmware Rebuild)

```bash
cd src/build
scp -P 2222 CSICapture CSIStream CSIHiResStream VisionAssistant root@<board-ip>:/root/
```

Ensure `libs_patch/` is at `/root/libs_patch/` on the board and `LD_LIBRARY_PATH` is set:

```bash
export LD_LIBRARY_PATH=/root/libs_patch/lib:/root/libs_patch/middleware_v2:/root/libs_patch/middleware_v2_3rd:/root/libs_patch/tpu_sdk_libs:/root/libs_patch:/root/libs_patch/opencv:$LD_LIBRARY_PATH
```

For firmware integration (baking into image), see the [LicheeRV-Nano-Build fork](https://github.com/0xjba/LicheeRV-Nano-Build).

---

## Usage

### Single Capture

```bash
./CSICapture /root/capture.jpg 30
```

### MJPEG Streams

```bash
./CSIStream &        # grayscale — http://<board-ip>:7777
./CSIHiResStream &   # color 1920x1440 — http://<board-ip>:7778

killall CSIStream CSIHiResStream  # stop
```

### Vision Assistant

```bash
./VisionAssistant
```

**First run:** prompts for WiFi SSID and password, saves to `/etc/camera-wifi.conf`.

**Flow:**
1. Press USER button → WiFi connects
2. Camera captures 1920x1440 frame
3. Image sent to AI via OpenRouter (`anthropic/claude-opus-4.6`)
4. Response printed to terminal
5. Press button again within 30s for another capture, or wait to auto-disconnect WiFi

**Reconfigure WiFi:**
```bash
rm /etc/camera-wifi.conf
./VisionAssistant
```

---

## opencv-mobile Patches Applied

All patches are pre-applied to `opencv-mobile-4.13.0/` in this repo.

| File | Change |
|---|---|
| `capture_cvi.cpp` | `#undef __riscv_vector` — disables RVV 1.0 intrinsics (incompatible with GCC 10.2.0) |
| `jpeg_decoder_cvi.cpp` | Same as above |
| `capture_cvi.cpp` | Default resolution changed from 1080p to 1440p |
| `capture_cvi.cpp` | Added `device_model == 5` for LicheeRV Nano support |
| `stb_image.h` | `#define STBI_NO_SIMD` at top |
| `options.txt` | `WITH_OPENMP=OFF` |
| `CMakeLists.txt` | RVV HAL subdirectory commented out |

> GCC 10.2.0 supports RVV 0.7 only. opencv-mobile 4.13.0 uses RVV 1.0 intrinsics. The `#undef` patches force scalar fallback. Camera pipeline performance is unaffected as it runs entirely inside middleware `.so` files.
