# LicheeRV Nano Camera

1920x1440 MJPEG streaming and capture for LicheeRV Nano with GC4653 sensor.

## Requirements

- LicheeRV-Nano-Build toolchain at `~/LicheeRV-Nano-Build/host-tools/gcc/riscv64-linux-musl-x86_64`

## Build
```bash
./build.sh
```

## Firmware Integration

After building, integrate into LicheeRV-Nano-Build:
```bash
OVERLAY=~/LicheeRV-Nano-Build/buildroot/board/cvitek/SG200X/overlay
mkdir -p $OVERLAY/root
cp src/build/{CSICapture,CSIStream,CSIHiResStream} $OVERLAY/root/
cp -r libs_patch $OVERLAY/root/
```

Add LD_LIBRARY_PATH to `$OVERLAY/etc/profile.d/middleware.sh`:
```
export LD_LIBRARY_PATH=/root/libs_patch/lib:/root/libs_patch/middleware_v2:/root/libs_patch/middleware_v2_3rd:/root/libs_patch/tpu_sdk_libs:/root/libs_patch:/root/libs_patch/opencv:$LD_LIBRARY_PATH
```

Then rebuild firmware:
```bash
cd ~/LicheeRV-Nano-Build
source build/cvisetup.sh
defconfig sg2002_licheervnano_sd
build_all
```

## Deploy (without firmware rebuild)
```bash
cd src/build
scp -P 2222 CSICapture CSIStream CSIHiResStream root@<board-ip>:/root/
```

## Usage
```bash
# Single capture
./CSICapture /root/capture.jpg 30

# MJPEG stream grayscale on port 7777
./CSIStream

# MJPEG stream color 1920x1440 on port 7778
./CSIHiResStream
```
