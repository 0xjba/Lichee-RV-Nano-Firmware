# LicheeRV Nano Camera

1920x1440 MJPEG streaming and capture for LicheeRV Nano with GC4653 sensor.

## Requirements

- LicheeRV-Nano-Build toolchain at `~/LicheeRV-Nano-Build/host-tools/gcc/riscv64-linux-musl-x86_64`

## Build
```bash
./build.sh
```

## Deploy
```bash
cd src/build
scp -P 2222 CSICapture CSIStream CSIHiResStream root@<board-ip>:/root/
```

## Usage
```bash
# Single capture
./CSICapture /root/capture.jpg 30

# MJPEG stream (grayscale) on port 7777
./CSIStream

# MJPEG stream (color) on port 7778
./CSIHiResStream
```

## Board setup

`libs_patch/` must be at `/root/libs_patch/` on the board with LD_LIBRARY_PATH set:
```bash
export LD_LIBRARY_PATH=/root/libs_patch/lib:/root/libs_patch/middleware_v2:/root/libs_patch/middleware_v2_3rd:/root/libs_patch/tpu_sdk_libs:/root/libs_patch:/root/libs_patch/opencv
```
