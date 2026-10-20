# vlc-nvenc

Windows encoder plugin for VLC 3.0.x. Module name `nvenc`. Display name `NVIDIA NVENC H.264/HEVC/AV1 encoder`. Shortcut `nvenc`.

It encodes H.264, HEVC, and, when the GPU reports it, AV1 through the NVIDIA driver's `nvEncodeAPI64.dll`. Official VLC Windows builds ship no NVENC encoder. This tree is not a VideoLAN project.

GPL-2.0-or-later. The grant is in `src/nvenc.c`. The descriptor sets `VLC_MODULE_LICENSE` to `VLC_LICENSE_GPL_2_PLUS`. There is no gettext catalog and no `set_text_domain`.

## Module

| | |
| --- | --- |
| Capability | `encoder` |
| Score | 210 |
| Category | Input / Codecs → Video codecs |
| File | `libnvenc_plugin.dll` |
| ABI | `3_0_0f` (VLC 3.0). A 3.0 plugin does not load in VLC 4. |

`module_need` tries encoders from highest score to lowest and keeps the first `Open` that returns success. 210 is above the bundled x264 encoder (200). An H.264 or HEVC transcode that does not name an encoder uses this module when `Open` succeeds. `Open` returns an error for a non-video stream, a codec it does not handle (unless the module was forced), a missing `nvEncodeAPI64.dll`, a driver older than NVENC API 12.2, no DXGI adapter with vendor `0x10DE`, or a codec the GPU does not report. VLC then tries the next encoder. Forcing the module on an unknown codec selects H.264.

Codecs and profiles:

| `vcodec` | Profile |
| --- | --- |
| `h264` | H.264 High |
| `hevc`, `h265`, `hvc1`, `hev1` | HEVC Main |
| `av01` | AV1 Main |

H.264 and HEVC run on Turing (GeForce RTX 20-series, including the RTX 2070) and later NVIDIA GPUs. AV1 encode is used only when `NvEncGetEncodeCaps` succeeds for that codec. That starts with Ada (RTX 40-series). A Turing GPU fails `Open` for AV1.

## How it works

The session is a DirectX 11 device on the NVIDIA adapter with the largest dedicated video memory. The API entry points are loaded at runtime from `nvEncodeAPI64.dll`. The structs come from the vendored FFmpeg `nv-codec-headers` header. The proprietary NVENC SDK is not in this tree.

Encoding is synchronous (`enableEncodeAsync` left off) with picture-type decision on (`enablePTD`). The encoder asks transcode for `NV12`. A three-plane 4:2:0 picture is packed as interleaved chroma from plane 1 (U) and plane 2 (V). Width and height are forced even. Each picture is copied into one NVENC input buffer. The driver copies that buffer during `NvEncEncodePicture`, so the same input buffer is reused. Output bitstream buffers are a ring of `bframes + 3`, capped at 8.

`NV_ENC_ERR_NEED_MORE_INPUT` produces no block. `NV_ENC_SUCCESS` locks every output submitted since the previous success and returns them as one block chain. A null picture sends end-of-stream once and returns the blocks still pending. The next null picture returns nothing.

Parameter sets are Annex-B in `fmt_out.p_extra`, and each IDR repeats them. H.264 and HEVC also write access unit delimiters. AV1 requests Annex-B sequence headers. Bit depth is 8. Chroma is 4:2:0. Level is left to the driver. The IDR period is the GOP length.

Transcode gives this encoder CPU pictures. With `soverlay`, transcode blends subpictures on the CPU before that. This module does not draw them.

DTS is assigned from input PTS in display order. With B-frames, the first outputs are dated before the first PTS so DTS stays monotonic and does not pass PTS. The default B-frame count is 0, so decode order matches presentation order.

A size below the GPU's minimum (the driver's minimum, or 144×48 when that query is below 16) still returns success from `Open` and does not call `NvEncInitializeEncoder`. Transcode probes an encoder and then closes it. `Encode` on that uninitialized session returns no block.

## Options

Prefix `sout-nvenc-`. List them with `vlc -I dummy -p nvenc`. Inside `venc=nvenc{...}` the names drop the prefix.

| Option | Default | |
| --- | --- | --- |
| `sout-nvenc-preset` | `p4` | `p1` through `p7`. `p1` is fastest. `p7` is highest quality. Any other name uses `p4`. |
| `sout-nvenc-tune` | `hq` | `hq`, `ll`, `ull`, `uhq`. `uhq` is applied for HEVC. Other codecs use `hq`. |
| `sout-nvenc-rc` | empty | `cq`, `vbr`, or `cbr`. Empty uses `vbr` when the transcode bitrate is set, and `cq` when it is 0. |
| `sout-nvenc-cq` | 23 | 0–51. Lower is higher quality. A value below 1 is encoded as 23. Used for `cq`. |
| `sout-nvenc-gop` | 0 | Frames between IDR pictures, 0–1000. 0 uses two seconds of frames (`frame rate × 2`), or 60 if that is below 1. |
| `sout-nvenc-bframes` | 0 | Consecutive B-frames, 0–7, then clamped to `NV_ENC_CAPS_NUM_MAX_BFRAMES`. `frameIntervalP` is this value plus 1. |

`cq` sets NVENC VBR with `targetQuality` and a zero average bitrate. `vbr` sets the average to the transcode bitrate and the maximum to 1.5 times that. `cbr` sets the average and the maximum to the bitrate. `vbr` or `cbr` with a zero bitrate uses `cq` instead. In a transcode chain, `vb` below 16000 is kilobits per second. VLC stores the encoder bitrate in bits per second.

```
#transcode{vcodec=hevc,venc=nvenc,vb=8000,acodec=none}:std{access=file,mux=mp4,dst=out.mp4}
```

```
#transcode{vcodec=h264,venc=nvenc{preset=p1,rc=cbr},vb=8000,acodec=none}:std{access=file,mux=mp4,dst=out.mp4}
```

The same options exist as `--sout-nvenc-preset`, `--sout-nvenc-tune`, `--sout-nvenc-rc`, `--sout-nvenc-cq`, `--sout-nvenc-gop`, and `--sout-nvenc-bframes`.

## Convert / Save

The profile editor's codec list is fixed: H-264, H-265, and the other built-in names. It does not list encoder modules. The dialog emits `venc=x264{...}` when the H.264 quality value is above 0 or the codec custom box is not empty. Otherwise it omits `venc`. An omitted `venc` uses `sout-transcode-venc` when that preference is set, and the highest-scoring encoder otherwise.

**Video - H.264 + MP3 (MP4)** and **Video - H.265 + MP3 (MP4)** omit `venc` and set the video bitrate to 0, so they use this module at `cq` 23 once it is installed. Profiles that store a custom string, including the Android profiles (`profile=baseline`), send `venc=x264`.

The named control is Tools → Preferences → Show settings: All → Stream output → Sout stream → Transcode → Video encoder (`sout-transcode-venc`). The entry label is `NVIDIA NVENC H.264/HEVC/AV1 encoder`. The saved value is `nvenc`. This module's own options are under Input / Codecs → Video codecs.

## Build

VLC 3.0.x plugin headers and a Windows `gcc` that can link `d3d11`, `dxgi`, `ole32`, and `uuid`. From this directory:

```
mingw32-make
```

GNU make's built-in `CC` is `cc`. The makefile selects `gcc` unless `CC` was set by the environment or the command line. The output is `build/libnvenc_plugin.dll`.

`VLC_INC` is the directory that contains `vlc_common.h`. The default is `../vlc-gpmf/third_party/vlc/vlc-3.0.23/sdk/include/vlc/plugins`. `VLC_DLL` is the installed `libvlccore.dll`. The default is `C:/Program Files/VideoLAN/VLC/libvlccore.dll`.

The compile line defines `MODULE_STRING` as the literal `nvenc` and defines `__PLUGIN__`. There is no `config.h`. The link uses `-static-libgcc`, so the DLL does not import `libgcc_s` or `winpthread`. VLC will not load a plugin that needs those. The exported entry points are `vlc_entry__3_0_0f`, `vlc_entry_copyright__3_0_0f`, and `vlc_entry_license__3_0_0f`.

The source opens a DirectX 11 NVENC session and loads `nvEncodeAPI64.dll`. The makefile's non-Windows rule does not build this file.

## Install

VLC loads `lib*_plugin.dll` from its `plugins` directory, up to five levels down, and from each directory in `VLC_PLUGIN_PATH`. The environment variable is added to the install directory. It applies to processes started from that shell. A Start-menu launch does not see it.

Close VLC before replacing a DLL it has loaded. Run `vlc-cache-gen` on a plugin directory after each copy or rebuild. A cache entry whose size or modification time does not match the DLL is stale.

Try the build without copying it into Program Files:

```
$env:VLC_PLUGIN_PATH = "C:\path\to\vlc-nvenc\build"
& "C:\Program Files\VideoLAN\VLC\vlc-cache-gen.exe" $env:VLC_PLUGIN_PATH
& "C:\Program Files\VideoLAN\VLC\vlc.exe" -I dummy -p nvenc
```

Installed copy, from an elevated shell, after VLC is closed:

```
Copy-Item build\libnvenc_plugin.dll "C:\Program Files\VideoLAN\VLC\plugins\codec\"
& "C:\Program Files\VideoLAN\VLC\vlc-cache-gen.exe" "C:\Program Files\VideoLAN\VLC\plugins"
```

`plugins\codec` is the directory used by VLC's other encoders. Restart VLC after the cache is written.

## Third party

`third_party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h` is FFmpeg `nv-codec-headers` n12.2.72.0, NVENC API 12.2. Copyright (c) 2010-2024 NVIDIA Corporation. The MIT notice is at the top of that header. It is the interface header. The encoder implementation is the installed NVIDIA driver.
