/*****************************************************************************
 * nvenc.c: NVIDIA NVENC encoder for VLC 3
 *****************************************************************************
 * Copyright (C) 2026 Chris Olin
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*
 * Loads nvEncodeAPI64.dll from the installed NVIDIA driver. The structs are
 * FFmpeg's MIT nv-codec-headers, vendored under third_party/.
 *
 * VLC 3 sends CPU pictures. Each frame is copied into an NVENC buffer and
 * encoded on the GPU. H.264 and HEVC run on Turing (RTX 20-series, including
 * the 2070) and later RTX cards. AV1 is used only when the GPU reports it,
 * which starts with Ada (RTX 40-series).
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define VLC_MODULE_LICENSE  VLC_LICENSE_GPL_2_PLUS
#define VLC_MODULE_COPYRIGHT "Copyright (C) 2026 Chris Olin"

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>

#include <stdlib.h>
#include <string.h>

#define COBJMACROS
#define INITGUID
#include <d3d11.h>
#include <dxgi.h>

#include <ffnvcodec/nvEncodeAPI.h>

#define SOUT_CFG_PREFIX "sout-nvenc-"
#define OUT_BUFFERS 8
#define DTS_CAP 64

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);

static const char *const preset_list[] = {
    "p1", "p2", "p3", "p4", "p5", "p6", "p7"
};
static const char *const tune_list[] = {
    "hq", "ll", "ull", "uhq"
};
static const char *const rc_list[] = {
    "cq", "vbr", "cbr"
};

#define PRESET_TEXT "NVENC preset"
#define PRESET_LONGTEXT "p1 is fastest and p7 is highest quality. p4 is the default."
#define TUNE_TEXT "NVENC tuning"
#define TUNE_LONGTEXT "hq is the file-convert default. ll and ull are low latency. uhq is HEVC on Turing and newer."
#define RC_TEXT "Rate control"
#define RC_LONGTEXT "cq keeps a quality target. vbr and cbr use the transcode bitrate. The default is vbr when a bitrate is set, otherwise cq."
#define CQ_TEXT "Constant-quality target"
#define CQ_LONGTEXT "0 to 51, lower is higher quality. Used when rate control is cq. 0 lets the driver choose. 23 is the default."
#define GOP_TEXT "Keyframe interval"
#define GOP_LONGTEXT "Frames between IDR pictures. 0 uses about two seconds."
#define BFRAMES_TEXT "B-frames"
#define BFRAMES_LONGTEXT "Consecutive B-frames. 0 keeps decode order equal to presentation order. The GPU maximum is applied when this is higher."

vlc_module_begin()
    set_description("NVIDIA NVENC H.264/HEVC/AV1 encoder")
    set_capability("encoder", 210)
    set_callbacks(Open, Close)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_VCODEC)
    add_shortcut("nvenc")

    add_string(SOUT_CFG_PREFIX "preset", "p4", PRESET_TEXT, PRESET_LONGTEXT, false)
        change_string_list(preset_list, preset_list)
    add_string(SOUT_CFG_PREFIX "tune", "hq", TUNE_TEXT, TUNE_LONGTEXT, false)
        change_string_list(tune_list, tune_list)
    add_string(SOUT_CFG_PREFIX "rc", "", RC_TEXT, RC_LONGTEXT, false)
        change_string_list(rc_list, rc_list)
    add_integer(SOUT_CFG_PREFIX "cq", 23, CQ_TEXT, CQ_LONGTEXT, false)
        change_integer_range(0, 51)
    add_integer(SOUT_CFG_PREFIX "gop", 0, GOP_TEXT, GOP_LONGTEXT, false)
        change_integer_range(0, 1000)
    add_integer(SOUT_CFG_PREFIX "bframes", 0, BFRAMES_TEXT, BFRAMES_LONGTEXT, false)
        change_integer_range(0, 7)
vlc_module_end()

static const char *const ppsz_sout_options[] = {
    "preset", "tune", "rc", "cq", "gop", "bframes", NULL
};

struct encoder_sys_t {
    HMODULE lib;
    NV_ENCODE_API_FUNCTION_LIST fn;
    void *encoder;
    ID3D11Device *device;

    NV_ENC_INPUT_PTR input;
    NV_ENC_OUTPUT_PTR outputs[OUT_BUFFERS];
    int n_outputs;
    int pending[OUT_BUFFERS];
    int n_pending;

    unsigned width;
    unsigned height;
    unsigned fps_num;
    unsigned fps_den;
    vlc_tick_t frame_len;
    uint32_t frame_idx;
    int bframes;

    vlc_tick_t dts_q[DTS_CAP];
    int dts_head;
    int dts_count;
    int output_count;
    vlc_tick_t delay_time;
    bool delay_time_set;
    bool has_last_dts;
    vlc_tick_t last_dts;

    bool ready;
    bool eos_sent;
};

static int nv_ok(encoder_t *enc, NVENCSTATUS st, const char *what)
{
    if (st == NV_ENC_SUCCESS)
        return 0;
    msg_Err(enc, "NVENC %s failed (%d)", what, (int)st);
    return -1;
}

static const GUID *preset_guid(const char *name)
{
    if (!name || !*name || !strcmp(name, "p4")) return &NV_ENC_PRESET_P4_GUID;
    if (!strcmp(name, "p1")) return &NV_ENC_PRESET_P1_GUID;
    if (!strcmp(name, "p2")) return &NV_ENC_PRESET_P2_GUID;
    if (!strcmp(name, "p3")) return &NV_ENC_PRESET_P3_GUID;
    if (!strcmp(name, "p5")) return &NV_ENC_PRESET_P5_GUID;
    if (!strcmp(name, "p6")) return &NV_ENC_PRESET_P6_GUID;
    if (!strcmp(name, "p7")) return &NV_ENC_PRESET_P7_GUID;
    return &NV_ENC_PRESET_P4_GUID;
}

static NV_ENC_TUNING_INFO tune_info(const char *name)
{
    if (name && !strcmp(name, "ll")) return NV_ENC_TUNING_INFO_LOW_LATENCY;
    if (name && !strcmp(name, "ull")) return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    if (name && !strcmp(name, "uhq")) return NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY;
    return NV_ENC_TUNING_INFO_HIGH_QUALITY;
}

static int codec_of(vlc_fourcc_t fcc, const GUID **guid, const GUID **profile,
                    const char **label)
{
    if (fcc == VLC_CODEC_H264) {
        *guid = &NV_ENC_CODEC_H264_GUID;
        *profile = &NV_ENC_H264_PROFILE_HIGH_GUID;
        *label = "H.264";
        return 0;
    }
    if (fcc == VLC_CODEC_HEVC || fcc == VLC_FOURCC('h','2','6','5') ||
        fcc == VLC_FOURCC('h','v','c','1') || fcc == VLC_FOURCC('h','e','v','1')) {
        *guid = &NV_ENC_CODEC_HEVC_GUID;
        *profile = &NV_ENC_HEVC_PROFILE_MAIN_GUID;
        *label = "HEVC";
        return 1;
    }
    if (fcc == VLC_CODEC_AV1) {
        *guid = &NV_ENC_CODEC_AV1_GUID;
        *profile = &NV_ENC_AV1_PROFILE_MAIN_GUID;
        *label = "AV1";
        return 2;
    }
    return -1;
}

static int open_device(encoder_t *enc, ID3D11Device **dev, char *name, size_t name_len)
{
    IDXGIFactory *factory = NULL;
    HRESULT hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
    if (FAILED(hr)) {
        msg_Err(enc, "DXGI factory failed (0x%08lx)", (unsigned long)hr);
        return -1;
    }

    IDXGIAdapter *best = NULL;
    SIZE_T best_mem = 0;
    for (UINT i = 0; ; i++) {
        IDXGIAdapter *adapter = NULL;
        if (IDXGIFactory_EnumAdapters(factory, i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc)) &&
            desc.VendorId == 0x10DE &&
            desc.DedicatedVideoMemory >= best_mem) {
            if (best)
                IDXGIAdapter_Release(best);
            best = adapter;
            best_mem = desc.DedicatedVideoMemory;
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                name, (int)name_len, NULL, NULL);
        } else {
            IDXGIAdapter_Release(adapter);
        }
    }
    IDXGIFactory_Release(factory);

    if (!best) {
        msg_Err(enc, "no NVIDIA adapter");
        return -1;
    }

    hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                           NULL, 0, D3D11_SDK_VERSION, dev, NULL, NULL);
    IDXGIAdapter_Release(best);
    if (FAILED(hr)) {
        msg_Err(enc, "D3D11 device failed (0x%08lx)", (unsigned long)hr);
        return -1;
    }
    return 0;
}

static int load_api(encoder_t *enc, encoder_sys_t *sys)
{
    sys->lib = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!sys->lib) {
        msg_Err(enc, "nvEncodeAPI64.dll is not installed");
        return -1;
    }

    typedef NVENCSTATUS (NVENCAPI *getver_fn)(uint32_t *);
    typedef NVENCSTATUS (NVENCAPI *create_fn)(NV_ENCODE_API_FUNCTION_LIST *);
    getver_fn getver = (getver_fn)GetProcAddress(sys->lib, "NvEncodeAPIGetMaxSupportedVersion");
    create_fn create = (create_fn)GetProcAddress(sys->lib, "NvEncodeAPICreateInstance");
    if (!getver || !create) {
        msg_Err(enc, "NVENC entry points are missing");
        return -1;
    }

    uint32_t driver_ver = 0;
    if (nv_ok(enc, getver(&driver_ver), "version query"))
        return -1;
    uint32_t need = ((uint32_t)NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (need > driver_ver) {
        msg_Err(enc, "NVIDIA driver NVENC API %u.%u is older than this plugin (%u.%u)",
                driver_ver >> 4, driver_ver & 0xf,
                NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        return -1;
    }

    memset(&sys->fn, 0, sizeof(sys->fn));
    sys->fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (nv_ok(enc, create(&sys->fn), "create instance"))
        return -1;
    return 0;
}

static int query_cap(encoder_t *enc, encoder_sys_t *sys, const GUID *codec,
                     NV_ENC_CAPS cap, int *value)
{
    NV_ENC_CAPS_PARAM param;
    memset(&param, 0, sizeof(param));
    param.version = NV_ENC_CAPS_PARAM_VER;
    param.capsToQuery = cap;
    return nv_ok(enc, sys->fn.nvEncGetEncodeCaps(sys->encoder, *codec, &param, value),
                 "caps");
}

static void dts_push(encoder_sys_t *sys, vlc_tick_t pts)
{
    if (sys->dts_count >= DTS_CAP)
        return;
    int slot = (sys->dts_head + sys->dts_count) % DTS_CAP;
    sys->dts_q[slot] = pts;
    sys->dts_count++;
}

static bool dts_peek(const encoder_sys_t *sys, int index, vlc_tick_t *pts)
{
    if (index < 0 || index >= sys->dts_count)
        return false;
    *pts = sys->dts_q[(sys->dts_head + index) % DTS_CAP];
    return true;
}

static bool dts_dequeue(encoder_sys_t *sys, vlc_tick_t *pts)
{
    if (!dts_peek(sys, 0, pts))
        return false;
    sys->dts_head = (sys->dts_head + 1) % DTS_CAP;
    sys->dts_count--;
    return true;
}

/* Display-order input PTS becomes decode-order DTS. The first bframes
 * outputs are placed before the first PTS so DTS stays monotonic and
 * does not pass PTS. */
static vlc_tick_t assign_dts(encoder_sys_t *sys, vlc_tick_t pts)
{
    int delay = sys->bframes;
    vlc_tick_t dts = pts;
    vlc_tick_t oldest = pts;

    if (delay <= 0 || sys->output_count >= delay) {
        if (!dts_dequeue(sys, &dts))
            dts = pts;
    } else {
        if (!sys->delay_time_set) {
            vlc_tick_t t0, t1;
            if (dts_peek(sys, 0, &t0) && dts_peek(sys, delay, &t1))
                sys->delay_time = t1 - t0;
            else
                sys->delay_time = (vlc_tick_t)delay * sys->frame_len;
            if (sys->delay_time <= 0)
                sys->delay_time = (vlc_tick_t)delay * sys->frame_len;
            sys->delay_time_set = true;
        }
        if (!dts_peek(sys, 0, &oldest))
            oldest = pts;
        dts = oldest - sys->delay_time * (delay - sys->output_count) / delay;
    }
    sys->output_count++;

    if (dts > pts)
        dts = pts;
    if (sys->has_last_dts && dts <= sys->last_dts) {
        vlc_tick_t next = sys->last_dts + 1;
        if (next <= pts)
            dts = next;
    }
    sys->last_dts = dts;
    sys->has_last_dts = true;
    return dts;
}

static void copy_nv12(const picture_t *pic, uint8_t *dst, uint32_t pitch,
                      unsigned width, unsigned height)
{
    unsigned y_bytes = pic->p[0].i_visible_pitch;
    if (y_bytes == 0 || y_bytes > width)
        y_bytes = width;
    for (unsigned y = 0; y < height; y++) {
        const uint8_t *src = pic->p[0].p_pixels + (size_t)y * pic->p[0].i_pitch;
        memcpy(dst + (size_t)y * pitch, src, y_bytes);
    }

    uint8_t *uv = dst + (size_t)pitch * height;
    if (pic->format.i_chroma == VLC_CODEC_NV12 || pic->i_planes < 3) {
        unsigned uv_bytes = pic->p[1].i_visible_pitch;
        if (uv_bytes == 0 || uv_bytes > width)
            uv_bytes = width;
        for (unsigned y = 0; y < height / 2; y++) {
            const uint8_t *src = pic->p[1].p_pixels + (size_t)y * pic->p[1].i_pitch;
            memcpy(uv + (size_t)y * pitch, src, uv_bytes);
        }
        return;
    }

    for (unsigned y = 0; y < height / 2; y++) {
        const uint8_t *u = pic->p[1].p_pixels + (size_t)y * pic->p[1].i_pitch;
        const uint8_t *v = pic->p[2].p_pixels + (size_t)y * pic->p[2].i_pitch;
        uint8_t *row = uv + (size_t)y * pitch;
        for (unsigned x = 0; x < width / 2; x++) {
            row[2 * x] = u[x];
            row[2 * x + 1] = v[x];
        }
    }
}

static NV_ENC_OUTPUT_PTR take_output(encoder_sys_t *sys)
{
    for (int i = 0; i < sys->n_outputs; i++) {
        bool used = false;
        for (int j = 0; j < sys->n_pending; j++) {
            if (sys->pending[j] == i)
                used = true;
        }
        if (!used) {
            sys->pending[sys->n_pending++] = i;
            return sys->outputs[i];
        }
    }
    return NULL;
}

static block_t *lock_output(encoder_t *enc, encoder_sys_t *sys, int index)
{
    NV_ENC_LOCK_BITSTREAM lock;
    memset(&lock, 0, sizeof(lock));
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = sys->outputs[index];
    if (nv_ok(enc, sys->fn.nvEncLockBitstream(sys->encoder, &lock), "lock bitstream"))
        return NULL;
    block_t *block = NULL;
    if (lock.bitstreamSizeInBytes > 0 && lock.bitstreamBufferPtr) {
        block = block_Alloc(lock.bitstreamSizeInBytes);
        if (block) {
            memcpy(block->p_buffer, lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
            vlc_tick_t pts = (vlc_tick_t)lock.outputTimeStamp;
            block->i_pts = pts;
            block->i_dts = assign_dts(sys, pts);
            block->i_length = sys->frame_len;
            if (lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                lock.pictureType == NV_ENC_PIC_TYPE_I)
                block->i_flags |= BLOCK_FLAG_TYPE_I;
            else if (lock.pictureType == NV_ENC_PIC_TYPE_P)
                block->i_flags |= BLOCK_FLAG_TYPE_P;
            else if (lock.pictureType == NV_ENC_PIC_TYPE_B ||
                     lock.pictureType == NV_ENC_PIC_TYPE_BI)
                block->i_flags |= BLOCK_FLAG_TYPE_B;
        }
    }
    sys->fn.nvEncUnlockBitstream(sys->encoder, sys->outputs[index]);
    return block;
}

static block_t *drain_pending(encoder_t *enc, encoder_sys_t *sys)
{
    block_t *head = NULL;
    block_t **tail = &head;
    for (int i = 0; i < sys->n_pending; i++) {
        block_t *block = lock_output(enc, sys, sys->pending[i]);
        if (!block)
            continue;
        *tail = block;
        tail = &block->p_next;
    }
    sys->n_pending = 0;
    return head;
}

static block_t *submit(encoder_t *enc, encoder_sys_t *sys, picture_t *pic)
{
    NV_ENC_PIC_PARAMS params;
    memset(&params, 0, sizeof(params));
    params.version = NV_ENC_PIC_PARAMS_VER;
    params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

    if (!pic) {
        params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        sys->eos_sent = true;
    } else {
        NV_ENC_LOCK_INPUT_BUFFER inlock;
        memset(&inlock, 0, sizeof(inlock));
        inlock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        inlock.inputBuffer = sys->input;
        if (nv_ok(enc, sys->fn.nvEncLockInputBuffer(sys->encoder, &inlock), "lock input"))
            return NULL;
        uint32_t pitch = inlock.pitch ? inlock.pitch : sys->width;
        copy_nv12(pic, inlock.bufferDataPtr, pitch, sys->width, sys->height);
        sys->fn.nvEncUnlockInputBuffer(sys->encoder, sys->input);

        vlc_tick_t pts = pic->date;
        if (pts == VLC_TICK_INVALID)
            pts = (vlc_tick_t)sys->frame_idx * sys->frame_len;
        dts_push(sys, pts);

        NV_ENC_OUTPUT_PTR out = take_output(sys);
        if (!out) {
            msg_Err(enc, "NVENC output buffer pool is full");
            return NULL;
        }
        params.inputBuffer = sys->input;
        params.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
        params.inputWidth = sys->width;
        params.inputHeight = sys->height;
        params.inputPitch = pitch;
        params.outputBitstream = out;
        params.inputTimeStamp = (uint64_t)pts;
        params.inputDuration = (uint64_t)sys->frame_len;
        params.frameIdx = sys->frame_idx++;
    }

    NVENCSTATUS st = sys->fn.nvEncEncodePicture(sys->encoder, &params);
    if (st == NV_ENC_ERR_NEED_MORE_INPUT)
        return NULL;
    if (st != NV_ENC_SUCCESS) {
        msg_Err(enc, "NVENC encode failed (%d)", (int)st);
        sys->n_pending = 0;
        return NULL;
    }
    return drain_pending(enc, sys);
}

static block_t *Encode(encoder_t *enc, picture_t *pic)
{
    encoder_sys_t *sys = enc->p_sys;
    if (!sys->ready) {
        if (pic)
            msg_Err(enc, "NVENC session is not initialized");
        return NULL;
    }
    if (!pic) {
        if (sys->eos_sent)
            return NULL;
        return submit(enc, sys, NULL);
    }
    return submit(enc, sys, pic);
}

static void apply_codec_config(NV_ENC_CONFIG *cfg, int kind, uint32_t gop)
{
    if (kind == 0) {
        cfg->encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_AUTOSELECT;
        cfg->encodeCodecConfig.h264Config.idrPeriod = gop;
        cfg->encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        cfg->encodeCodecConfig.h264Config.outputAUD = 1;
        cfg->encodeCodecConfig.h264Config.chromaFormatIDC = 1;
        cfg->encodeCodecConfig.h264Config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        cfg->encodeCodecConfig.h264Config.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    } else if (kind == 1) {
        cfg->encodeCodecConfig.hevcConfig.level = NV_ENC_LEVEL_AUTOSELECT;
        cfg->encodeCodecConfig.hevcConfig.idrPeriod = gop;
        cfg->encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        cfg->encodeCodecConfig.hevcConfig.outputAUD = 1;
        cfg->encodeCodecConfig.hevcConfig.chromaFormatIDC = 1;
        cfg->encodeCodecConfig.hevcConfig.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        cfg->encodeCodecConfig.hevcConfig.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    } else {
        cfg->encodeCodecConfig.av1Config.level = NV_ENC_LEVEL_AUTOSELECT;
        cfg->encodeCodecConfig.av1Config.idrPeriod = gop;
        cfg->encodeCodecConfig.av1Config.repeatSeqHdr = 1;
        cfg->encodeCodecConfig.av1Config.chromaFormatIDC = 1;
        cfg->encodeCodecConfig.av1Config.outputAnnexBFormat = 1;
        cfg->encodeCodecConfig.av1Config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        cfg->encodeCodecConfig.av1Config.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    }
}

static int init_session(encoder_t *enc, encoder_sys_t *sys, const GUID *codec,
                        const GUID *profile, int kind, const char *label,
                        const char *gpu)
{
    char *preset_name = var_GetString(enc, SOUT_CFG_PREFIX "preset");
    char *tune_name = var_GetString(enc, SOUT_CFG_PREFIX "tune");
    char *rc_name = var_GetString(enc, SOUT_CFG_PREFIX "rc");
    int cq = var_GetInteger(enc, SOUT_CFG_PREFIX "cq");
    int gop_opt = var_GetInteger(enc, SOUT_CFG_PREFIX "gop");
    int bframes = var_GetInteger(enc, SOUT_CFG_PREFIX "bframes");
    const GUID *preset = preset_guid(preset_name);
    NV_ENC_TUNING_INFO tuning = tune_info(tune_name);

    if (tuning == NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY && kind != 1) {
        msg_Warn(enc, "uhq tuning is HEVC-only; using hq");
        tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    }

    int supported = 0;
    if (query_cap(enc, sys, codec, NV_ENC_CAPS_NUM_MAX_BFRAMES, &supported)) {
        msg_Err(enc, "%s NVENC is not available on %s", label, gpu);
        free(preset_name);
        free(tune_name);
        free(rc_name);
        return -1;
    }

    int min_w = 0, min_h = 0;
    query_cap(enc, sys, codec, NV_ENC_CAPS_WIDTH_MIN, &min_w);
    query_cap(enc, sys, codec, NV_ENC_CAPS_HEIGHT_MIN, &min_h);
    if (min_w < 16) min_w = 144;
    if (min_h < 16) min_h = 48;
    if ((int)sys->width < min_w || (int)sys->height < min_h) {
        msg_Dbg(enc, "NVENC probe size %ux%u is below %dx%d; session deferred",
                sys->width, sys->height, min_w, min_h);
        free(preset_name);
        free(tune_name);
        free(rc_name);
        return 0;
    }

    int max_b = 0;
    if (query_cap(enc, sys, codec, NV_ENC_CAPS_NUM_MAX_BFRAMES, &max_b))
        max_b = 0;
    if (bframes < 0) bframes = 0;
    if (bframes > max_b) bframes = max_b;
    sys->bframes = bframes;

    NV_ENC_PRESET_CONFIG preset_cfg;
    memset(&preset_cfg, 0, sizeof(preset_cfg));
    preset_cfg.version = NV_ENC_PRESET_CONFIG_VER;
    preset_cfg.presetCfg.version = NV_ENC_CONFIG_VER;
    if (nv_ok(enc, sys->fn.nvEncGetEncodePresetConfigEx(sys->encoder, *codec, *preset,
                                                       tuning, &preset_cfg),
              "preset")) {
        free(preset_name);
        free(tune_name);
        free(rc_name);
        return -1;
    }

    uint32_t gop = gop_opt > 0 ? (uint32_t)gop_opt : sys->fps_num / sys->fps_den * 2;
    if (gop < 1) gop = 60;

    NV_ENC_CONFIG cfg = preset_cfg.presetCfg;
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.profileGUID = *profile;
    cfg.gopLength = gop;
    cfg.frameIntervalP = bframes + 1;
    apply_codec_config(&cfg, kind, gop);

    unsigned bitrate = enc->fmt_out.i_bitrate;
    bool cq_mode = true;
    if (rc_name && !strcmp(rc_name, "cbr"))
        cq_mode = false;
    else if (rc_name && !strcmp(rc_name, "vbr"))
        cq_mode = false;
    else if (rc_name && !strcmp(rc_name, "cq"))
        cq_mode = true;
    else
        cq_mode = bitrate == 0;

    if (!cq_mode && bitrate == 0) {
        msg_Warn(enc, "NVENC bitrate is 0; using constant quality");
        cq_mode = true;
    }

    if (cq_mode) {
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        cfg.rcParams.targetQuality = (uint8_t)(cq > 0 ? cq : 23);
        cfg.rcParams.averageBitRate = 0;
        cfg.rcParams.maxBitRate = 0;
        cfg.rcParams.vbvBufferSize = 0;
        cfg.rcParams.vbvInitialDelay = 0;
    } else if (rc_name && !strcmp(rc_name, "cbr")) {
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        cfg.rcParams.targetQuality = 0;
        cfg.rcParams.averageBitRate = bitrate;
        cfg.rcParams.maxBitRate = bitrate;
        cfg.rcParams.vbvBufferSize = bitrate;
        cfg.rcParams.vbvInitialDelay = bitrate / 2;
        enc->fmt_out.i_bitrate = bitrate;
    } else {
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        cfg.rcParams.targetQuality = 0;
        cfg.rcParams.averageBitRate = bitrate;
        cfg.rcParams.maxBitRate = bitrate + bitrate / 2;
        cfg.rcParams.vbvBufferSize = cfg.rcParams.maxBitRate;
        cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize / 2;
        enc->fmt_out.i_bitrate = bitrate;
    }

    NV_ENC_INITIALIZE_PARAMS init;
    memset(&init, 0, sizeof(init));
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = *codec;
    init.presetGUID = *preset;
    init.encodeWidth = sys->width;
    init.encodeHeight = sys->height;
    init.darWidth = sys->width;
    init.darHeight = sys->height;
    init.frameRateNum = sys->fps_num;
    init.frameRateDen = sys->fps_den;
    init.enablePTD = 1;
    init.encodeConfig = &cfg;
    init.tuningInfo = tuning;
    if (nv_ok(enc, sys->fn.nvEncInitializeEncoder(sys->encoder, &init), "initialize")) {
        free(preset_name);
        free(tune_name);
        free(rc_name);
        return -1;
    }

    uint8_t header[4096];
    uint32_t header_size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD seq;
    memset(&seq, 0, sizeof(seq));
    seq.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    seq.spsppsBuffer = header;
    seq.inBufferSize = sizeof(header);
    seq.outSPSPPSPayloadSize = &header_size;
    if (nv_ok(enc, sys->fn.nvEncGetSequenceParams(sys->encoder, &seq), "sequence header")) {
        free(preset_name);
        free(tune_name);
        free(rc_name);
        return -1;
    }
    if (header_size > 0) {
        enc->fmt_out.p_extra = malloc(header_size);
        if (!enc->fmt_out.p_extra) {
            free(preset_name);
            free(tune_name);
            free(rc_name);
            return -1;
        }
        memcpy(enc->fmt_out.p_extra, header, header_size);
        enc->fmt_out.i_extra = header_size;
    }

    NV_ENC_CREATE_INPUT_BUFFER inbuf;
    memset(&inbuf, 0, sizeof(inbuf));
    inbuf.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    inbuf.width = sys->width;
    inbuf.height = sys->height;
    inbuf.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    if (nv_ok(enc, sys->fn.nvEncCreateInputBuffer(sys->encoder, &inbuf), "input buffer")) {
        free(preset_name);
        free(tune_name);
        free(rc_name);
        return -1;
    }
    sys->input = inbuf.inputBuffer;

    sys->n_outputs = bframes + 3;
    if (sys->n_outputs > OUT_BUFFERS)
        sys->n_outputs = OUT_BUFFERS;
    for (int i = 0; i < sys->n_outputs; i++) {
        NV_ENC_CREATE_BITSTREAM_BUFFER outbuf;
        memset(&outbuf, 0, sizeof(outbuf));
        outbuf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (nv_ok(enc, sys->fn.nvEncCreateBitstreamBuffer(sys->encoder, &outbuf),
                  "output buffer")) {
            free(preset_name);
            free(tune_name);
            free(rc_name);
            return -1;
        }
        sys->outputs[i] = outbuf.bitstreamBuffer;
    }

    sys->ready = true;
    msg_Info(enc, "NVENC %s %ux%u preset %s on %s",
             label, sys->width, sys->height,
             preset_name && *preset_name ? preset_name : "p4", gpu);
    free(preset_name);
    free(tune_name);
    free(rc_name);
    return 0;
}

static int Open(vlc_object_t *obj)
{
    encoder_t *enc = (encoder_t *)obj;
    if (enc->fmt_out.i_cat != VIDEO_ES)
        return VLC_EGENERIC;

    const GUID *codec = NULL;
    const GUID *profile = NULL;
    const char *label = NULL;
    int kind = codec_of(enc->fmt_out.i_codec, &codec, &profile, &label);
    if (kind < 0) {
        if (!enc->obj.force)
            return VLC_EGENERIC;
        enc->fmt_out.i_codec = VLC_CODEC_H264;
        kind = codec_of(enc->fmt_out.i_codec, &codec, &profile, &label);
    }
    if (kind == 0)
        enc->fmt_out.i_codec = VLC_CODEC_H264;
    else if (kind == 1)
        enc->fmt_out.i_codec = VLC_CODEC_HEVC;
    else
        enc->fmt_out.i_codec = VLC_CODEC_AV1;

    config_ChainParse(enc, SOUT_CFG_PREFIX, ppsz_sout_options, enc->p_cfg);

    encoder_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;
    enc->p_sys = sys;
    enc->pf_encode_video = Encode;
    enc->fmt_in.i_codec = VLC_CODEC_NV12;
    enc->fmt_in.video.i_chroma = VLC_CODEC_NV12;
    sys->has_last_dts = false;

    unsigned w = enc->fmt_in.video.i_visible_width;
    unsigned h = enc->fmt_in.video.i_visible_height;
    if (!w) w = enc->fmt_in.video.i_width;
    if (!h) h = enc->fmt_in.video.i_height;
    if (!w) w = enc->fmt_out.video.i_visible_width;
    if (!h) h = enc->fmt_out.video.i_visible_height;
    w &= ~1u;
    h &= ~1u;
    sys->width = w;
    sys->height = h;

    sys->fps_num = enc->fmt_in.video.i_frame_rate;
    sys->fps_den = enc->fmt_in.video.i_frame_rate_base;
    if (!sys->fps_num || !sys->fps_den) {
        sys->fps_num = 30000;
        sys->fps_den = 1001;
    }
    sys->frame_len = (vlc_tick_t)CLOCK_FREQ * sys->fps_den / sys->fps_num;
    if (sys->frame_len <= 0)
        sys->frame_len = CLOCK_FREQ / 30;

    char gpu[128] = "NVIDIA";
    if (load_api(enc, sys) || open_device(enc, &sys->device, gpu, sizeof(gpu))) {
        Close(obj);
        return VLC_EGENERIC;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session;
    memset(&session, 0, sizeof(session));
    session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session.device = sys->device;
    session.apiVersion = NVENCAPI_VERSION;
    if (nv_ok(enc, sys->fn.nvEncOpenEncodeSessionEx(&session, &sys->encoder), "open session")) {
        Close(obj);
        return VLC_EGENERIC;
    }

    if (init_session(enc, sys, codec, profile, kind, label, gpu)) {
        Close(obj);
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    encoder_t *enc = (encoder_t *)obj;
    encoder_sys_t *sys = enc->p_sys;
    if (!sys)
        return;

    if (sys->encoder) {
        if (sys->input)
            sys->fn.nvEncDestroyInputBuffer(sys->encoder, sys->input);
        for (int i = 0; i < sys->n_outputs; i++) {
            if (sys->outputs[i])
                sys->fn.nvEncDestroyBitstreamBuffer(sys->encoder, sys->outputs[i]);
        }
        sys->fn.nvEncDestroyEncoder(sys->encoder);
    }
    if (sys->device)
        ID3D11Device_Release(sys->device);
    if (sys->lib)
        FreeLibrary(sys->lib);
    free(sys);
    enc->p_sys = NULL;
}
