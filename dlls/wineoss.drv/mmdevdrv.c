/*
 * Copyright 2011 Andrew Eikum for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define NONAMELESSUNION
#define COBJMACROS
#define INITGUID
#include "config.h"

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "wine/debug.h"
#include "wine/unicode.h"
#include "wine/list.h"

#include "ole2.h"
#include "mmdeviceapi.h"
#include "devpkey.h"
#include "dshow.h"
#include "dsound.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>

#if defined(HAVE_SYS_SOUNDCARD_H)
# include <sys/soundcard.h>
#elif defined(HAVE_MACHINE_SOUNDCARD_H)
# include <machine/soundcard.h>
#elif defined(HAVE_SOUNDCARD_H)
# include <soundcard.h>
#endif

WINE_DEFAULT_DEBUG_CHANNEL(oss);

static const REFERENCE_TIME DefaultPeriod = 200000;
static const REFERENCE_TIME MinimumPeriod = 100000;

typedef struct ACImpl {
    IAudioClient IAudioClient_iface;
    IAudioRenderClient IAudioRenderClient_iface;
    IAudioCaptureClient IAudioCaptureClient_iface;
    IAudioSessionControl2 IAudioSessionControl2_iface;
    ISimpleAudioVolume ISimpleAudioVolume_iface;
    IAudioClock IAudioClock_iface;
    IAudioClock2 IAudioClock2_iface;

    LONG ref;

    IMMDevice *parent;

    WAVEFORMATEX *fmt;

    EDataFlow dataflow;
    DWORD flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;

    int fd;
    oss_audioinfo ai;

    BOOL initted, playing;
    UINT64 written_frames, held_frames, tmp_buffer_frames, inbuf_frames;
    UINT32 period_us, bufsize_frames;
    UINT32 lcl_offs_frames; /* offs into local_buffer where valid data starts */

    BYTE *local_buffer, *tmp_buffer;
    int buf_state;
    HANDLE timer;

    CRITICAL_SECTION lock;
} ACImpl;

enum BufferStates {
    NOT_LOCKED = 0,
    LOCKED_NORMAL, /* public buffer piece is from local_buffer */
    LOCKED_WRAPPED /* public buffer piece is in tmp_buffer */
};

static HANDLE g_timer_q;

static const IAudioClientVtbl AudioClient_Vtbl;
static const IAudioRenderClientVtbl AudioRenderClient_Vtbl;
static const IAudioCaptureClientVtbl AudioCaptureClient_Vtbl;
static const IAudioSessionControl2Vtbl AudioSessionControl2_Vtbl;
static const ISimpleAudioVolumeVtbl SimpleAudioVolume_Vtbl;
static const IAudioClockVtbl AudioClock_Vtbl;
static const IAudioClock2Vtbl AudioClock2_Vtbl;

static inline ACImpl *impl_from_IAudioClient(IAudioClient *iface)
{
    return CONTAINING_RECORD(iface, ACImpl, IAudioClient_iface);
}

static inline ACImpl *impl_from_IAudioRenderClient(IAudioRenderClient *iface)
{
    return CONTAINING_RECORD(iface, ACImpl, IAudioRenderClient_iface);
}

static inline ACImpl *impl_from_IAudioCaptureClient(IAudioCaptureClient *iface)
{
    return CONTAINING_RECORD(iface, ACImpl, IAudioCaptureClient_iface);
}

static inline ACImpl *impl_from_IAudioSessionControl2(IAudioSessionControl2 *iface)
{
    return CONTAINING_RECORD(iface, ACImpl, IAudioSessionControl2_iface);
}

static inline ACImpl *impl_from_ISimpleAudioVolume(ISimpleAudioVolume *iface)
{
    return CONTAINING_RECORD(iface, ACImpl, ISimpleAudioVolume_iface);
}

static inline ACImpl *impl_from_IAudioClock(IAudioClock *iface)
{
    return CONTAINING_RECORD(iface, ACImpl, IAudioClock_iface);
}

static inline ACImpl *impl_from_IAudioClock2(IAudioClock2 *iface)
{
    return CONTAINING_RECORD(iface, ACImpl, IAudioClock2_iface);
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, void *reserved)
{
    if(reason == DLL_PROCESS_ATTACH){
        g_timer_q = CreateTimerQueue();
        if(!g_timer_q)
            return FALSE;
    }

    return TRUE;
}

HRESULT WINAPI AUDDRV_GetEndpointIDs(EDataFlow flow, WCHAR ***ids, void ***keys,
        UINT *num, UINT *def_index)
{
    int i, mixer_fd;
    oss_sysinfo sysinfo;
    static int print_once = 0;

    TRACE("%d %p %p %p\n", flow, ids, num, def_index);

    mixer_fd = open("/dev/mixer", O_RDONLY, 0);
    if(mixer_fd < 0){
        ERR("OSS /dev/mixer doesn't seem to exist\n");
        return AUDCLNT_E_SERVICE_NOT_RUNNING;
    }

    if(ioctl(mixer_fd, SNDCTL_SYSINFO, &sysinfo) < 0){
        close(mixer_fd);

        if(errno == EINVAL){
            ERR("OSS version too old, need at least OSSv4\n");
            return AUDCLNT_E_SERVICE_NOT_RUNNING;
        }

        ERR("Error getting SNDCTL_SYSINFO: %d (%s)\n", errno, strerror(errno));
        return E_FAIL;
    }

    if(!print_once){
        TRACE("OSS sysinfo:\n");
        TRACE("product: %s\n", sysinfo.product);
        TRACE("version: %s\n", sysinfo.version);
        TRACE("versionnum: %x\n", sysinfo.versionnum);
        TRACE("numaudios: %d\n", sysinfo.numaudios);
        TRACE("nummixers: %d\n", sysinfo.nummixers);
        TRACE("numcards: %d\n", sysinfo.numcards);
        TRACE("numaudioengines: %d\n", sysinfo.numaudioengines);
        print_once = 1;
    }

    if(sysinfo.numaudios <= 0){
        WARN("No audio devices!\n");
        close(mixer_fd);
        return AUDCLNT_E_SERVICE_NOT_RUNNING;
    }

    *ids = HeapAlloc(GetProcessHeap(), 0, sysinfo.numaudios * sizeof(WCHAR *));
    *keys = HeapAlloc(GetProcessHeap(), 0, sysinfo.numaudios * sizeof(char *));

    *num = 0;
    *def_index = -1;
    for(i = 0; i < sysinfo.numaudios; ++i){
        oss_audioinfo ai = {0};

        ai.dev = i;
        if(ioctl(mixer_fd, SNDCTL_AUDIOINFO, &ai) < 0){
            WARN("Error getting AUDIOINFO for dev %d: %d (%s)\n", i, errno,
                    strerror(errno));
            continue;
        }

        if((flow == eCapture && (ai.caps & PCM_CAP_INPUT)) ||
                (flow == eRender && (ai.caps & PCM_CAP_OUTPUT))){
            size_t len;

            (*keys)[*num] = HeapAlloc(GetProcessHeap(), 0,
                    strlen(ai.devnode) + 1);
            if(!(*keys)[*num]){
                for(i = 0; i < *num; ++i){
                    HeapFree(GetProcessHeap(), 0, (*ids)[i]);
                    HeapFree(GetProcessHeap(), 0, (*keys)[i]);
                }
                HeapFree(GetProcessHeap(), 0, *ids);
                HeapFree(GetProcessHeap(), 0, *keys);
                close(mixer_fd);
                return E_OUTOFMEMORY;
            }
            strcpy((*keys)[*num], ai.devnode);

            len = MultiByteToWideChar(CP_UNIXCP, 0, ai.name, -1, NULL, 0);
            (*ids)[*num] = HeapAlloc(GetProcessHeap(), 0,
                    len * sizeof(WCHAR));
            if(!(*ids)[*num]){
                HeapFree(GetProcessHeap(), 0, (*keys)[*num]);
                for(i = 0; i < *num; ++i){
                    HeapFree(GetProcessHeap(), 0, (*ids)[i]);
                    HeapFree(GetProcessHeap(), 0, (*keys)[i]);
                }
                HeapFree(GetProcessHeap(), 0, *ids);
                HeapFree(GetProcessHeap(), 0, *keys);
                close(mixer_fd);
                return E_OUTOFMEMORY;
            }
            MultiByteToWideChar(CP_UNIXCP, 0, ai.name, -1,
                    (*ids)[*num], len);

            if(ai.caps & PCM_CAP_DEFAULT)
                *def_index = *num;

            (*num)++;
        }
    }

    if(*def_index == -1)
        *def_index = 0;

    close(mixer_fd);

    return S_OK;
}

HRESULT WINAPI AUDDRV_GetAudioEndpoint(char *devnode, IMMDevice *dev,
        EDataFlow dataflow, IAudioClient **out)
{
    ACImpl *This;

    TRACE("%s %p %d %p\n", devnode, dev, dataflow, out);

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ACImpl));
    if(!This)
        return E_OUTOFMEMORY;

    if(dataflow == eRender)
        This->fd = open(devnode, O_WRONLY, 0);
    else if(dataflow == eCapture)
        This->fd = open(devnode, O_RDONLY, 0);
    else{
        HeapFree(GetProcessHeap(), 0, This);
        return E_INVALIDARG;
    }
    if(This->fd < 0){
        ERR("Unable to open device %s: %d (%s)\n", devnode, errno,
                strerror(errno));
        HeapFree(GetProcessHeap(), 0, This);
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }

    This->dataflow = dataflow;

    This->ai.dev = -1;
    if(ioctl(This->fd, SNDCTL_ENGINEINFO, &This->ai) < 0){
        ERR("Unable to get audio info for device %s: %d (%s)\n", devnode,
                errno, strerror(errno));
        close(This->fd);
        HeapFree(GetProcessHeap(), 0, This);
        return E_FAIL;
    }

    TRACE("OSS audioinfo:\n");
    TRACE("devnode: %s\n", This->ai.devnode);
    TRACE("name: %s\n", This->ai.name);
    TRACE("busy: %x\n", This->ai.busy);
    TRACE("caps: %x\n", This->ai.caps);
    TRACE("iformats: %x\n", This->ai.iformats);
    TRACE("oformats: %x\n", This->ai.oformats);
    TRACE("enabled: %d\n", This->ai.enabled);
    TRACE("min_rate: %d\n", This->ai.min_rate);
    TRACE("max_rate: %d\n", This->ai.max_rate);
    TRACE("min_channels: %d\n", This->ai.min_channels);
    TRACE("max_channels: %d\n", This->ai.max_channels);

    This->IAudioClient_iface.lpVtbl = &AudioClient_Vtbl;
    This->IAudioRenderClient_iface.lpVtbl = &AudioRenderClient_Vtbl;
    This->IAudioCaptureClient_iface.lpVtbl = &AudioCaptureClient_Vtbl;
    This->IAudioSessionControl2_iface.lpVtbl = &AudioSessionControl2_Vtbl;
    This->ISimpleAudioVolume_iface.lpVtbl = &SimpleAudioVolume_Vtbl;
    This->IAudioClock_iface.lpVtbl = &AudioClock_Vtbl;
    This->IAudioClock2_iface.lpVtbl = &AudioClock2_Vtbl;

    InitializeCriticalSection(&This->lock);

    This->parent = dev;
    IMMDevice_AddRef(This->parent);

    IAudioClient_AddRef(&This->IAudioClient_iface);

    *out = &This->IAudioClient_iface;

    return S_OK;
}

static HRESULT WINAPI AudioClient_QueryInterface(IAudioClient *iface,
        REFIID riid, void **ppv)
{
    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    if(!ppv)
        return E_POINTER;
    *ppv = NULL;
    if(IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IAudioClient))
        *ppv = iface;
    if(*ppv){
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }
    WARN("Unknown interface %s\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI AudioClient_AddRef(IAudioClient *iface)
{
    ACImpl *This = impl_from_IAudioClient(iface);
    ULONG ref;
    ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) Refcount now %u\n", This, ref);
    return ref;
}

static ULONG WINAPI AudioClient_Release(IAudioClient *iface)
{
    ACImpl *This = impl_from_IAudioClient(iface);
    ULONG ref;
    ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) Refcount now %u\n", This, ref);
    if(!ref){
        IAudioClient_Stop(iface);
        IMMDevice_Release(This->parent);
        DeleteCriticalSection(&This->lock);
        close(This->fd);
        HeapFree(GetProcessHeap(), 0, This->local_buffer);
        HeapFree(GetProcessHeap(), 0, This->tmp_buffer);
        CoTaskMemFree(This->fmt);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

static void dump_fmt(const WAVEFORMATEX *fmt)
{
    TRACE("wFormatTag: 0x%x (", fmt->wFormatTag);
    switch(fmt->wFormatTag){
    case WAVE_FORMAT_PCM:
        TRACE("WAVE_FORMAT_PCM");
        break;
    case WAVE_FORMAT_IEEE_FLOAT:
        TRACE("WAVE_FORMAT_IEEE_FLOAT");
        break;
    case WAVE_FORMAT_EXTENSIBLE:
        TRACE("WAVE_FORMAT_EXTENSIBLE");
        break;
    default:
        TRACE("Unknown");
        break;
    }
    TRACE(")\n");

    TRACE("nChannels: %u\n", fmt->nChannels);
    TRACE("nSamplesPerSec: %u\n", fmt->nSamplesPerSec);
    TRACE("nAvgBytesPerSec: %u\n", fmt->nAvgBytesPerSec);
    TRACE("nBlockAlign: %u\n", fmt->nBlockAlign);
    TRACE("wBitsPerSample: %u\n", fmt->wBitsPerSample);
    TRACE("cbSize: %u\n", fmt->cbSize);

    if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE){
        WAVEFORMATEXTENSIBLE *fmtex = (void*)fmt;
        TRACE("dwChannelMask: %08x\n", fmtex->dwChannelMask);
        TRACE("Samples: %04x\n", fmtex->Samples.wReserved);
        TRACE("SubFormat: %s\n", wine_dbgstr_guid(&fmtex->SubFormat));
    }
}

static DWORD get_channel_mask(unsigned int channels)
{
    switch(channels){
    case 0:
        return 0;
    case 1:
        return SPEAKER_FRONT_CENTER;
    case 2:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 3:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
            SPEAKER_LOW_FREQUENCY;
    case 4:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT |
            SPEAKER_BACK_RIGHT;
    case 5:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT |
            SPEAKER_BACK_RIGHT | SPEAKER_LOW_FREQUENCY;
    case 6:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT |
            SPEAKER_BACK_RIGHT | SPEAKER_LOW_FREQUENCY | SPEAKER_FRONT_CENTER;
    case 7:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT |
            SPEAKER_BACK_RIGHT | SPEAKER_LOW_FREQUENCY | SPEAKER_FRONT_CENTER |
            SPEAKER_BACK_CENTER;
    }
    FIXME("Unknown speaker configuration: %u\n", channels);
    return 0;
}

static int get_oss_format(const WAVEFORMATEX *fmt)
{
    WAVEFORMATEXTENSIBLE *fmtex = (WAVEFORMATEXTENSIBLE*)fmt;

    if(fmt->wFormatTag == WAVE_FORMAT_PCM ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))){
        switch(fmt->wBitsPerSample){
        case 8:
            return AFMT_U8;
        case 16:
            return AFMT_S16_LE;
        case 24:
            return AFMT_S24_PACKED;
        case 32:
            return AFMT_S32_LE;
        }
        return -1;
    }

    if(fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))){
        if(fmt->wBitsPerSample != 32)
            return -1;

        return AFMT_FLOAT;
    }

    return -1;
}

static WAVEFORMATEX *clone_format(const WAVEFORMATEX *fmt)
{
    WAVEFORMATEX *ret;
    size_t size;

    if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        size = sizeof(WAVEFORMATEXTENSIBLE);
    else
        size = sizeof(WAVEFORMATEX);

    ret = CoTaskMemAlloc(size);
    if(!ret)
        return NULL;

    memcpy(ret, fmt, size);

    ret->cbSize = size - sizeof(WAVEFORMATEX);

    return ret;
}

static HRESULT setup_oss_device(ACImpl *This, const WAVEFORMATEX *fmt,
        WAVEFORMATEX **out)
{
    int tmp, oss_format;
    double tenth;
    HRESULT ret = S_OK;
    WAVEFORMATEXTENSIBLE *fmtex = (void*)fmt;
    WAVEFORMATEX *closest = NULL;

    tmp = oss_format = get_oss_format(fmt);
    if(oss_format < 0)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if(ioctl(This->fd, SNDCTL_DSP_SETFMT, &tmp) < 0){
        WARN("SETFMT failed: %d (%s)\n", errno, strerror(errno));
        return E_FAIL;
    }
    if(tmp != oss_format){
        TRACE("Format unsupported by this OSS version: %x\n", oss_format);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    closest = clone_format(fmt);

    tmp = fmt->nSamplesPerSec;
    if(ioctl(This->fd, SNDCTL_DSP_SPEED, &tmp) < 0){
        WARN("SPEED failed: %d (%s)\n", errno, strerror(errno));
        CoTaskMemFree(closest);
        return E_FAIL;
    }
    tenth = fmt->nSamplesPerSec * 0.1;
    if(tmp > fmt->nSamplesPerSec + tenth || tmp < fmt->nSamplesPerSec - tenth){
        ret = S_FALSE;
        closest->nSamplesPerSec = tmp;
    }

    tmp = fmt->nChannels;
    if(ioctl(This->fd, SNDCTL_DSP_CHANNELS, &tmp) < 0){
        WARN("CHANNELS failed: %d (%s)\n", errno, strerror(errno));
        CoTaskMemFree(closest);
        return E_FAIL;
    }
    if(tmp != fmt->nChannels){
        ret = S_FALSE;
        closest->nChannels = tmp;
    }

    if(closest->wFormatTag == WAVE_FORMAT_EXTENSIBLE){
        DWORD mask = get_channel_mask(closest->nChannels);

        ((WAVEFORMATEXTENSIBLE*)closest)->dwChannelMask = mask;

        if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                fmtex->dwChannelMask != mask)
            ret = S_FALSE;
    }

    if(ret == S_OK || !out){
        CoTaskMemFree( closest);
        if(out)
            *out = NULL;
    }else{
        closest->nBlockAlign =
            closest->nChannels * closest->wBitsPerSample / 8;
        closest->nAvgBytesPerSec =
            closest->nBlockAlign * closest->nSamplesPerSec;
        *out = closest;
    }

    TRACE("returning: %08x\n", ret);
    return ret;
}

static HRESULT WINAPI AudioClient_Initialize(IAudioClient *iface,
        AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration,
        REFERENCE_TIME period, const WAVEFORMATEX *fmt,
        const GUID *sessionguid)
{
    ACImpl *This = impl_from_IAudioClient(iface);
    int mask;
    HRESULT hr;

    TRACE("(%p)->(%x, %x, %llu, %llu, %p, %s)\n", This, mode, flags,
            duration, period, fmt, debugstr_guid(sessionguid));

    if(!fmt)
        return E_POINTER;

    dump_fmt(fmt);

    if(mode != AUDCLNT_SHAREMODE_SHARED && mode != AUDCLNT_SHAREMODE_EXCLUSIVE)
        return AUDCLNT_E_NOT_INITIALIZED;

    if(flags & ~(AUDCLNT_STREAMFLAGS_CROSSPROCESS |
                AUDCLNT_STREAMFLAGS_LOOPBACK |
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_NOPERSIST |
                AUDCLNT_STREAMFLAGS_RATEADJUST |
                AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED |
                AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE |
                AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED)){
        TRACE("Unknown flags: %08x\n", flags);
        return E_INVALIDARG;
    }

    EnterCriticalSection(&This->lock);

    if(This->initted){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_ALREADY_INITIALIZED;
    }

    hr = setup_oss_device(This, fmt, NULL);
    if(hr == S_FALSE){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    if(FAILED(hr)){
        LeaveCriticalSection(&This->lock);
        return hr;
    }

    mask = 0;
    if(ioctl(This->fd, SNDCTL_DSP_SETTRIGGER, &mask) < 0){
        LeaveCriticalSection(&This->lock);
        WARN("SETTRIGGER failed: %d (%s)\n", errno, strerror(errno));
        return E_FAIL;
    }

    mask = (100 << 8) | 100;
    if(ioctl(This->fd, SNDCTL_DSP_SETPLAYVOL, &mask) < 0)
        WARN("SETPLAYVOL failed: %d (%s)\n", errno, strerror(errno));

    This->fmt = clone_format(fmt);
    if(!This->fmt){
        LeaveCriticalSection(&This->lock);
        return E_OUTOFMEMORY;
    }

    if(period)
        This->period_us = period / 10;
    else
        This->period_us = DefaultPeriod / 10;

    This->bufsize_frames = ceil(fmt->nSamplesPerSec * (duration / 10000000.));
    This->local_buffer = HeapAlloc(GetProcessHeap(), 0,
            This->bufsize_frames * fmt->nBlockAlign);
    if(!This->local_buffer){
        CoTaskMemFree(This->fmt);
        This->fmt = NULL;
        LeaveCriticalSection(&This->lock);
        return E_OUTOFMEMORY;
    }

    This->initted = TRUE;
    This->share = mode;
    This->flags = flags;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI AudioClient_GetBufferSize(IAudioClient *iface,
        UINT32 *frames)
{
    ACImpl *This = impl_from_IAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, frames);

    if(!frames)
        return E_POINTER;

    EnterCriticalSection(&This->lock);

    if(!This->initted){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_NOT_INITIALIZED;
    }

    *frames = This->bufsize_frames;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI AudioClient_GetStreamLatency(IAudioClient *iface,
        REFERENCE_TIME *latency)
{
    ACImpl *This = impl_from_IAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, latency);

    if(!latency)
        return E_POINTER;

    EnterCriticalSection(&This->lock);

    if(!This->initted){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_NOT_INITIALIZED;
    }

    if(This->dataflow == eRender){
        int delay_bytes;
        double delay_s;

        if(ioctl(This->fd, SNDCTL_DSP_GETODELAY, &delay_bytes) < 0){
            LeaveCriticalSection(&This->lock);
            WARN("GETODELAY failed: %d (%s)\n", errno, strerror(errno));
            return E_FAIL;
        }

        delay_s = delay_bytes / (double)(This->fmt->nSamplesPerSec *
                This->fmt->nBlockAlign);

        *latency = delay_s * 10000000;
    }else
        *latency = 10000; /* OSS doesn't provide input latency */

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI AudioClient_GetCurrentPadding(IAudioClient *iface,
        UINT32 *numpad)
{
    ACImpl *This = impl_from_IAudioClient(iface);
    audio_buf_info bi;

    TRACE("(%p)->(%p)\n", This, numpad);

    if(!numpad)
        return E_POINTER;

    EnterCriticalSection(&This->lock);

    if(!This->initted){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_NOT_INITIALIZED;
    }

    if(This->dataflow == eRender){
        if(ioctl(This->fd, SNDCTL_DSP_GETOSPACE, &bi) < 0){
            LeaveCriticalSection(&This->lock);
            WARN("GETOSPACE failed: %d (%s)\n", errno, strerror(errno));
            return E_FAIL;
        }

        *numpad = (bi.fragstotal * bi.fragsize - bi.bytes) /
            This->fmt->nBlockAlign;

        /* when the OSS buffer has less than one fragment of data, including
         * no data, it often reports it as some non-zero portion of a
         * fragment. when it has more than one fragment of data, it reports
         * it as some multiple of that portion of the fragment size.
         *
         * so, we have to do some ugly workarounds to report the timing
         * as accurately as possible */
        if(*numpad < bi.fragsize / This->fmt->nBlockAlign){
            *numpad = This->inbuf_frames;
            This->inbuf_frames = 0;
        }else{
            if(*numpad < This->inbuf_frames)
                This->inbuf_frames = *numpad;
            else
                *numpad = This->inbuf_frames;
        }
    }else if(This->dataflow == eCapture){
        if(ioctl(This->fd, SNDCTL_DSP_GETISPACE, &bi) < 0){
            LeaveCriticalSection(&This->lock);
            WARN("GETISPACE failed: %d (%s)\n", errno, strerror(errno));
            return E_FAIL;
        }

        if(bi.bytes <= bi.fragsize)
            *numpad = 0;
        else
            *numpad = bi.bytes / This->fmt->nBlockAlign;
    }else{
        LeaveCriticalSection(&This->lock);
        return E_UNEXPECTED;
    }

    *numpad += This->held_frames;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI AudioClient_IsFormatSupported(IAudioClient *iface,
        AUDCLNT_SHAREMODE mode, const WAVEFORMATEX *pwfx,
        WAVEFORMATEX **outpwfx)
{
    ACImpl *This = impl_from_IAudioClient(iface);
    HRESULT ret;

    TRACE("(%p)->(%x, %p, %p)\n", This, mode, pwfx, outpwfx);

    if(!pwfx || (mode == AUDCLNT_SHAREMODE_SHARED && !outpwfx))
        return E_POINTER;

    if(mode != AUDCLNT_SHAREMODE_SHARED && mode != AUDCLNT_SHAREMODE_EXCLUSIVE)
        return E_INVALIDARG;

    if(pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            pwfx->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        return E_INVALIDARG;

    dump_fmt(pwfx);

    EnterCriticalSection(&This->lock);

    ret = setup_oss_device(This, pwfx, outpwfx);

    LeaveCriticalSection(&This->lock);

    return ret;
}

static HRESULT WINAPI AudioClient_GetMixFormat(IAudioClient *iface,
        WAVEFORMATEX **pwfx)
{
    ACImpl *This = impl_from_IAudioClient(iface);
    WAVEFORMATEXTENSIBLE *fmt;
    int formats;

    TRACE("(%p)->(%p)\n", This, pwfx);

    if(!pwfx)
        return E_POINTER;

    *pwfx = HeapAlloc(GetProcessHeap(), 0, sizeof(WAVEFORMATEXTENSIBLE));
    if(!*pwfx)
        return E_OUTOFMEMORY;

    fmt = (WAVEFORMATEXTENSIBLE*)*pwfx;

    if(This->dataflow == eRender)
        formats = This->ai.oformats;
    else if(This->dataflow == eCapture)
        formats = This->ai.iformats;
    else
        return E_UNEXPECTED;

    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    if(formats & AFMT_S16_LE){
        fmt->Format.wBitsPerSample = 16;
        fmt->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }else if(formats & AFMT_FLOAT){
        fmt->Format.wBitsPerSample = 32;
        fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }else if(formats & AFMT_U8){
        fmt->Format.wBitsPerSample = 8;
        fmt->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }else if(formats & AFMT_S32_LE){
        fmt->Format.wBitsPerSample = 32;
        fmt->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }else if(formats & AFMT_S24_PACKED){
        fmt->Format.wBitsPerSample = 24;
        fmt->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }else{
        ERR("Didn't recognize any available OSS formats: %x\n", formats);
        return E_FAIL;
    }

    fmt->Format.nChannels = This->ai.max_channels;
    fmt->Format.nSamplesPerSec = This->ai.max_rate;
    fmt->dwChannelMask = get_channel_mask(fmt->Format.nChannels);

    fmt->Format.nBlockAlign = (fmt->Format.wBitsPerSample *
            fmt->Format.nChannels) / 8;
    fmt->Format.nAvgBytesPerSec = fmt->Format.nSamplesPerSec *
        fmt->Format.nBlockAlign;

    fmt->Samples.wValidBitsPerSample = fmt->Format.wBitsPerSample;
    fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    dump_fmt(*pwfx);

    return S_OK;
}

static HRESULT WINAPI AudioClient_GetDevicePeriod(IAudioClient *iface,
        REFERENCE_TIME *defperiod, REFERENCE_TIME *minperiod)
{
    ACImpl *This = impl_from_IAudioClient(iface);

    TRACE("(%p)->(%p, %p)\n", This, defperiod, minperiod);

    if(!defperiod && !minperiod)
        return E_POINTER;

    EnterCriticalSection(&This->lock);

    if(defperiod)
        *defperiod = DefaultPeriod;
    if(minperiod)
        *minperiod = MinimumPeriod;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static void oss_write_data(ACImpl *This)
{
    ssize_t written;
    UINT32 written_frames;
    size_t to_write;
    BYTE *buf =
        This->local_buffer + (This->lcl_offs_frames * This->fmt->nBlockAlign);

    if(This->lcl_offs_frames + This->held_frames > This->bufsize_frames)
        to_write = This->bufsize_frames - This->lcl_offs_frames;
    else
        to_write = This->held_frames;

    written = write(This->fd, buf, to_write * This->fmt->nBlockAlign);
    if(written < 0){
        WARN("write failed: %d (%s)\n", errno, strerror(errno));
        return;
    }
    written_frames = written / This->fmt->nBlockAlign;

    This->lcl_offs_frames += written_frames;
    This->lcl_offs_frames %= This->bufsize_frames;
    This->held_frames -= written_frames;
    This->inbuf_frames += written_frames;

    if(written_frames < to_write){
        /* OSS buffer probably full */
        return;
    }

    if(This->held_frames){
        /* wrapped and have some data back at the start to write */
        written = write(This->fd, This->local_buffer,
                This->held_frames * This->fmt->nBlockAlign);
        if(written < 0){
            WARN("write failed: %d (%s)\n", errno, strerror(errno));
            return;
        }
        written_frames = written / This->fmt->nBlockAlign;

        This->lcl_offs_frames += written_frames;
        This->lcl_offs_frames %= This->bufsize_frames;
        This->held_frames -= written_frames;
        This->inbuf_frames += written_frames;
    }
}

static void oss_read_data(ACImpl *This)
{
    UINT64 pos, readable;
    audio_buf_info bi;
    ssize_t nread;

    if(ioctl(This->fd, SNDCTL_DSP_GETISPACE, &bi) < 0){
        WARN("GETISPACE failed: %d (%s)\n", errno, strerror(errno));
        return;
    }

    pos = (This->held_frames + This->lcl_offs_frames) % This->bufsize_frames;
    readable = (This->bufsize_frames - pos) * This->fmt->nBlockAlign;

    if(bi.bytes < readable)
        readable = bi.bytes;

    nread = read(This->fd, This->local_buffer + pos * This->fmt->nBlockAlign,
            readable);
    if(nread < 0){
        WARN("read failed: %d (%s)\n", errno, strerror(errno));
        return;
    }

    This->held_frames += nread / This->fmt->nBlockAlign;

    if(This->held_frames > This->bufsize_frames){
        WARN("Overflow of unread data\n");
        This->lcl_offs_frames += This->held_frames;
        This->lcl_offs_frames %= This->bufsize_frames;
        This->held_frames = This->bufsize_frames;
    }
}

static void CALLBACK oss_period_callback(void *user, BOOLEAN timer)
{
    ACImpl *This = user;

    EnterCriticalSection(&This->lock);

    if(This->dataflow == eRender)
        oss_write_data(This);
    else if(This->dataflow == eCapture)
        oss_read_data(This);

    if(This->event)
        SetEvent(This->event);

    LeaveCriticalSection(&This->lock);
}

static HRESULT WINAPI AudioClient_Start(IAudioClient *iface)
{
    ACImpl *This = impl_from_IAudioClient(iface);
    int mask;

    TRACE("(%p)\n", This);

    EnterCriticalSection(&This->lock);

    if(!This->initted){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_NOT_INITIALIZED;
    }

    if((This->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !This->event){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_EVENTHANDLE_NOT_SET;
    }

    if(This->playing){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_NOT_STOPPED;
    }

    if(This->dataflow == eRender)
        mask = PCM_ENABLE_OUTPUT;
    else if(This->dataflow == eCapture)
        mask = PCM_ENABLE_INPUT;
    else{
        LeaveCriticalSection(&This->lock);
        return E_UNEXPECTED;
    }

    if(ioctl(This->fd, SNDCTL_DSP_SETTRIGGER, &mask) < 0){
        LeaveCriticalSection(&This->lock);
        WARN("SETTRIGGER failed: %d (%s)\n", errno, strerror(errno));
        return E_FAIL;
    }

    if(!CreateTimerQueueTimer(&This->timer, g_timer_q,
                oss_period_callback, This, 0, This->period_us / 1000,
                WT_EXECUTEINTIMERTHREAD))
        ERR("Unable to create period timer: %u\n", GetLastError());

    This->playing = TRUE;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI AudioClient_Stop(IAudioClient *iface)
{
    ACImpl *This = impl_from_IAudioClient(iface);
    int mask;

    TRACE("(%p)\n", This);

    EnterCriticalSection(&This->lock);

    if(!This->initted){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_NOT_INITIALIZED;
    }

    if(!This->playing){
        LeaveCriticalSection(&This->lock);
        return S_FALSE;
    }

    if(This->timer && This->timer != INVALID_HANDLE_VALUE){
        DeleteTimerQueueTimer(g_timer_q, This->timer,
                INVALID_HANDLE_VALUE);
        This->timer = NULL;
    }

    if(ioctl(This->fd, SNDCTL_DSP_HALT, NULL) < 0){
        LeaveCriticalSection(&This->lock);
        WARN("HALT failed: %d (%s)\n", errno, strerror(errno));
        return E_FAIL;
    }

    mask = 0;
    if(ioctl(This->fd, SNDCTL_DSP_SETTRIGGER, &mask) < 0){
        LeaveCriticalSection(&This->lock);
        WARN("SETTRIGGER failed: %d (%s)\n", errno, strerror(errno));
        return E_FAIL;
    }

    This->playing = FALSE;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI AudioClient_Reset(IAudioClient *iface)
{
    ACImpl *This = impl_from_IAudioClient(iface);

    TRACE("(%p)\n", This);

    EnterCriticalSection(&This->lock);

    if(!This->initted){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_NOT_INITIALIZED;
    }

    if(This->playing){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_NOT_STOPPED;
    }

    This->written_frames = 0;
    This->inbuf_frames = 0;
    This->held_frames = 0;

    if(ioctl(This->fd, SNDCTL_DSP_SKIP, NULL) < 0)
        WARN("SKIP failed: %d (%s)\n", errno, strerror(errno));

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI AudioClient_SetEventHandle(IAudioClient *iface,
        HANDLE event)
{
    ACImpl *This = impl_from_IAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, event);

    if(!event)
        return E_INVALIDARG;

    EnterCriticalSection(&This->lock);

    if(!This->initted){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_NOT_INITIALIZED;
    }

    if(!(This->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK)){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    }

    This->event = event;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI AudioClient_GetService(IAudioClient *iface, REFIID riid,
        void **ppv)
{
    ACImpl *This = impl_from_IAudioClient(iface);

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), ppv);

    if(!ppv)
        return E_POINTER;
    *ppv = NULL;

    EnterCriticalSection(&This->lock);

    if(!This->initted){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_NOT_INITIALIZED;
    }

    LeaveCriticalSection(&This->lock);

    if(IsEqualIID(riid, &IID_IAudioRenderClient)){
        if(This->dataflow != eRender)
            return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        *ppv = &This->IAudioRenderClient_iface;
    }else if(IsEqualIID(riid, &IID_IAudioCaptureClient)){
        if(This->dataflow != eCapture)
            return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        *ppv = &This->IAudioCaptureClient_iface;
    }else if(IsEqualIID(riid, &IID_IAudioSessionControl)){
        *ppv = &This->IAudioSessionControl2_iface;
    }else if(IsEqualIID(riid, &IID_ISimpleAudioVolume)){
        *ppv = &This->ISimpleAudioVolume_iface;
    }else if(IsEqualIID(riid, &IID_IAudioClock)){
        *ppv = &This->IAudioClock_iface;
    }

    if(*ppv){
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    FIXME("stub %s\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static const IAudioClientVtbl AudioClient_Vtbl =
{
    AudioClient_QueryInterface,
    AudioClient_AddRef,
    AudioClient_Release,
    AudioClient_Initialize,
    AudioClient_GetBufferSize,
    AudioClient_GetStreamLatency,
    AudioClient_GetCurrentPadding,
    AudioClient_IsFormatSupported,
    AudioClient_GetMixFormat,
    AudioClient_GetDevicePeriod,
    AudioClient_Start,
    AudioClient_Stop,
    AudioClient_Reset,
    AudioClient_SetEventHandle,
    AudioClient_GetService
};

static HRESULT WINAPI AudioRenderClient_QueryInterface(
        IAudioRenderClient *iface, REFIID riid, void **ppv)
{
    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    if(!ppv)
        return E_POINTER;
    *ppv = NULL;

    if(IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_IAudioRenderClient))
        *ppv = iface;
    if(*ppv){
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    WARN("Unknown interface %s\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI AudioRenderClient_AddRef(IAudioRenderClient *iface)
{
    ACImpl *This = impl_from_IAudioRenderClient(iface);
    return AudioClient_AddRef(&This->IAudioClient_iface);
}

static ULONG WINAPI AudioRenderClient_Release(IAudioRenderClient *iface)
{
    ACImpl *This = impl_from_IAudioRenderClient(iface);
    return AudioClient_Release(&This->IAudioClient_iface);
}

static HRESULT WINAPI AudioRenderClient_GetBuffer(IAudioRenderClient *iface,
        UINT32 frames, BYTE **data)
{
    ACImpl *This = impl_from_IAudioRenderClient(iface);
    UINT32 pad, write_pos;
    HRESULT hr;

    TRACE("(%p)->(%u, %p)\n", This, frames, data);

    if(!data)
        return E_POINTER;

    EnterCriticalSection(&This->lock);

    if(This->buf_state != NOT_LOCKED){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_OUT_OF_ORDER;
    }

    if(!frames){
        This->buf_state = LOCKED_NORMAL;
        LeaveCriticalSection(&This->lock);
        return S_OK;
    }

    hr = IAudioClient_GetCurrentPadding(&This->IAudioClient_iface, &pad);
    if(FAILED(hr)){
        LeaveCriticalSection(&This->lock);
        return hr;
    }

    if(pad + frames > This->bufsize_frames){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_BUFFER_TOO_LARGE;
    }

    write_pos =
        (This->lcl_offs_frames + This->held_frames) % This->bufsize_frames;
    if(write_pos + frames > This->bufsize_frames){
        if(This->tmp_buffer_frames < frames){
            if(This->tmp_buffer)
                This->tmp_buffer = HeapReAlloc(GetProcessHeap(), 0,
                        This->tmp_buffer, frames * This->fmt->nBlockAlign);
            else
                This->tmp_buffer = HeapAlloc(GetProcessHeap(), 0,
                        frames * This->fmt->nBlockAlign);
            if(!This->tmp_buffer){
                LeaveCriticalSection(&This->lock);
                return E_OUTOFMEMORY;
            }
            This->tmp_buffer_frames = frames;
        }
        *data = This->tmp_buffer;
        This->buf_state = LOCKED_WRAPPED;
    }else{
        *data = This->local_buffer +
            This->lcl_offs_frames * This->fmt->nBlockAlign;
        This->buf_state = LOCKED_NORMAL;
    }

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static void oss_wrap_buffer(ACImpl *This, BYTE *buffer, UINT32 written_bytes)
{
    UINT32 write_offs_frames =
        (This->lcl_offs_frames + This->held_frames) % This->bufsize_frames;
    UINT32 write_offs_bytes = write_offs_frames * This->fmt->nBlockAlign;
    UINT32 chunk_frames = This->bufsize_frames - write_offs_frames;
    UINT32 chunk_bytes = chunk_frames * This->fmt->nBlockAlign;

    if(written_bytes < chunk_bytes){
        memcpy(This->local_buffer + write_offs_bytes, buffer, written_bytes);
    }else{
        memcpy(This->local_buffer + write_offs_bytes, buffer, chunk_bytes);
        memcpy(This->local_buffer, buffer + chunk_bytes,
                written_bytes - chunk_bytes);
    }
}

static HRESULT WINAPI AudioRenderClient_ReleaseBuffer(
        IAudioRenderClient *iface, UINT32 written_frames, DWORD flags)
{
    ACImpl *This = impl_from_IAudioRenderClient(iface);
    BYTE *buffer;
    UINT32 written_bytes = written_frames * This->fmt->nBlockAlign;

    TRACE("(%p)->(%u, %x)\n", This, written_frames, flags);

    EnterCriticalSection(&This->lock);

    if(This->buf_state == NOT_LOCKED || !written_frames){
        This->buf_state = NOT_LOCKED;
        LeaveCriticalSection(&This->lock);
        return written_frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
    }

    if(This->buf_state == LOCKED_NORMAL)
        buffer = This->local_buffer +
            This->lcl_offs_frames * This->fmt->nBlockAlign;
    else
        buffer = This->tmp_buffer;

    if(flags & AUDCLNT_BUFFERFLAGS_SILENT){
        WAVEFORMATEXTENSIBLE *fmtex = (WAVEFORMATEXTENSIBLE*)This->fmt;
        if((This->fmt->wFormatTag == WAVE_FORMAT_PCM ||
                (This->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))) &&
                This->fmt->wBitsPerSample == 8)
            memset(buffer, 128, written_frames * This->fmt->nBlockAlign);
        else
            memset(buffer, 0, written_frames * This->fmt->nBlockAlign);
    }

    if(This->held_frames){
        if(This->buf_state == LOCKED_WRAPPED)
            oss_wrap_buffer(This, buffer, written_bytes);

        This->held_frames += written_frames;
    }else{
        ssize_t w_bytes;
        UINT32 w_frames;

        w_bytes = write(This->fd, buffer,
                written_frames * This->fmt->nBlockAlign);
        if(w_bytes < 0){
            LeaveCriticalSection(&This->lock);
            WARN("write failed: %d (%s)\n", errno, strerror(errno));
            return E_FAIL;
        }
        w_frames = w_bytes / This->fmt->nBlockAlign;
        This->inbuf_frames += w_frames;

        if(w_frames < written_frames){
            if(This->buf_state == LOCKED_WRAPPED)
                oss_wrap_buffer(This, This->tmp_buffer + w_bytes,
                        written_frames - w_frames);

            This->held_frames = written_frames - w_frames;
        }
    }

    This->written_frames += written_frames;
    This->buf_state = NOT_LOCKED;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static const IAudioRenderClientVtbl AudioRenderClient_Vtbl = {
    AudioRenderClient_QueryInterface,
    AudioRenderClient_AddRef,
    AudioRenderClient_Release,
    AudioRenderClient_GetBuffer,
    AudioRenderClient_ReleaseBuffer
};

static HRESULT WINAPI AudioCaptureClient_QueryInterface(
        IAudioCaptureClient *iface, REFIID riid, void **ppv)
{
    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    if(!ppv)
        return E_POINTER;
    *ppv = NULL;

    if(IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_IAudioCaptureClient))
        *ppv = iface;
    if(*ppv){
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    WARN("Unknown interface %s\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI AudioCaptureClient_AddRef(IAudioCaptureClient *iface)
{
    ACImpl *This = impl_from_IAudioCaptureClient(iface);
    return IAudioClient_AddRef(&This->IAudioClient_iface);
}

static ULONG WINAPI AudioCaptureClient_Release(IAudioCaptureClient *iface)
{
    ACImpl *This = impl_from_IAudioCaptureClient(iface);
    return IAudioClient_Release(&This->IAudioClient_iface);
}

static HRESULT WINAPI AudioCaptureClient_GetBuffer(IAudioCaptureClient *iface,
        BYTE **data, UINT32 *frames, DWORD *flags, UINT64 *devpos,
        UINT64 *qpcpos)
{
    ACImpl *This = impl_from_IAudioCaptureClient(iface);
    HRESULT hr;

    TRACE("(%p)->(%p, %p, %p, %p, %p)\n", This, data, frames, flags,
            devpos, qpcpos);

    if(!data || !frames || !flags)
        return E_POINTER;

    EnterCriticalSection(&This->lock);

    if(This->buf_state != NOT_LOCKED){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_OUT_OF_ORDER;
    }

    hr = IAudioCaptureClient_GetNextPacketSize(iface, frames);
    if(FAILED(hr)){
        LeaveCriticalSection(&This->lock);
        return hr;
    }

    *flags = 0;

    if(This->lcl_offs_frames + *frames > This->bufsize_frames){
        UINT32 chunk_bytes, offs_bytes, frames_bytes;
        if(This->tmp_buffer_frames < *frames){
            if(This->tmp_buffer)
                This->tmp_buffer = HeapReAlloc(GetProcessHeap(), 0,
                        This->tmp_buffer, *frames * This->fmt->nBlockAlign);
            else
                This->tmp_buffer = HeapAlloc(GetProcessHeap(), 0,
                        *frames * This->fmt->nBlockAlign);
            if(!This->tmp_buffer){
                LeaveCriticalSection(&This->lock);
                return E_OUTOFMEMORY;
            }
            This->tmp_buffer_frames = *frames;
        }

        *data = This->tmp_buffer;
        chunk_bytes = (This->bufsize_frames - This->lcl_offs_frames) *
            This->fmt->nBlockAlign;
        offs_bytes = This->lcl_offs_frames * This->fmt->nBlockAlign;
        frames_bytes = *frames * This->fmt->nBlockAlign;
        memcpy(This->tmp_buffer, This->local_buffer + offs_bytes, chunk_bytes);
        memcpy(This->tmp_buffer, This->local_buffer,
                frames_bytes - chunk_bytes);
    }else
        *data = This->local_buffer +
            This->lcl_offs_frames * This->fmt->nBlockAlign;

    This->buf_state = LOCKED_NORMAL;

    if(devpos || qpcpos)
        IAudioClock_GetPosition(&This->IAudioClock_iface, devpos, qpcpos);

    LeaveCriticalSection(&This->lock);

    return *frames ? S_OK : AUDCLNT_S_BUFFER_EMPTY;
}

static HRESULT WINAPI AudioCaptureClient_ReleaseBuffer(
        IAudioCaptureClient *iface, UINT32 done)
{
    ACImpl *This = impl_from_IAudioCaptureClient(iface);

    TRACE("(%p)->(%u)\n", This, done);

    EnterCriticalSection(&This->lock);

    if(This->buf_state == NOT_LOCKED){
        LeaveCriticalSection(&This->lock);
        return AUDCLNT_E_OUT_OF_ORDER;
    }

    This->held_frames -= done;
    This->lcl_offs_frames += done;
    This->lcl_offs_frames %= This->bufsize_frames;

    This->buf_state = NOT_LOCKED;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI AudioCaptureClient_GetNextPacketSize(
        IAudioCaptureClient *iface, UINT32 *frames)
{
    ACImpl *This = impl_from_IAudioCaptureClient(iface);

    TRACE("(%p)->(%p)\n", This, frames);

    return AudioClient_GetCurrentPadding(&This->IAudioClient_iface, frames);
}

static const IAudioCaptureClientVtbl AudioCaptureClient_Vtbl =
{
    AudioCaptureClient_QueryInterface,
    AudioCaptureClient_AddRef,
    AudioCaptureClient_Release,
    AudioCaptureClient_GetBuffer,
    AudioCaptureClient_ReleaseBuffer,
    AudioCaptureClient_GetNextPacketSize
};

static HRESULT WINAPI AudioSessionControl_QueryInterface(
        IAudioSessionControl2 *iface, REFIID riid, void **ppv)
{
    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    if(!ppv)
        return E_POINTER;
    *ppv = NULL;

    if(IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_IAudioSessionControl) ||
            IsEqualIID(riid, &IID_IAudioSessionControl2))
        *ppv = iface;
    if(*ppv){
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    WARN("Unknown interface %s\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI AudioSessionControl_AddRef(IAudioSessionControl2 *iface)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);
    return IAudioClient_AddRef(&This->IAudioClient_iface);
}

static ULONG WINAPI AudioSessionControl_Release(IAudioSessionControl2 *iface)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);
    return IAudioClient_Release(&This->IAudioClient_iface);
}

static HRESULT WINAPI AudioSessionControl_GetState(IAudioSessionControl2 *iface,
        AudioSessionState *state)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%p) - stub\n", This, state);

    if(!state)
        return E_POINTER;

    return E_NOTIMPL;
}

static HRESULT WINAPI AudioSessionControl_GetDisplayName(
        IAudioSessionControl2 *iface, WCHAR **name)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%p) - stub\n", This, name);

    return E_NOTIMPL;
}

static HRESULT WINAPI AudioSessionControl_SetDisplayName(
        IAudioSessionControl2 *iface, const WCHAR *name, const GUID *session)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%p, %s) - stub\n", This, name, debugstr_guid(session));

    return E_NOTIMPL;
}

static HRESULT WINAPI AudioSessionControl_GetIconPath(
        IAudioSessionControl2 *iface, WCHAR **path)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%p) - stub\n", This, path);

    return E_NOTIMPL;
}

static HRESULT WINAPI AudioSessionControl_SetIconPath(
        IAudioSessionControl2 *iface, const WCHAR *path, const GUID *session)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%p, %s) - stub\n", This, path, debugstr_guid(session));

    return E_NOTIMPL;
}

static HRESULT WINAPI AudioSessionControl_GetGroupingParam(
        IAudioSessionControl2 *iface, GUID *group)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%p) - stub\n", This, group);

    return E_NOTIMPL;
}

static HRESULT WINAPI AudioSessionControl_SetGroupingParam(
        IAudioSessionControl2 *iface, GUID *group, const GUID *session)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%s, %s) - stub\n", This, debugstr_guid(group),
            debugstr_guid(session));

    return E_NOTIMPL;
}

static HRESULT WINAPI AudioSessionControl_RegisterAudioSessionNotification(
        IAudioSessionControl2 *iface, IAudioSessionEvents *events)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%p) - stub\n", This, events);

    return S_OK;
}

static HRESULT WINAPI AudioSessionControl_UnregisterAudioSessionNotification(
        IAudioSessionControl2 *iface, IAudioSessionEvents *events)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%p) - stub\n", This, events);

    return S_OK;
}

static HRESULT WINAPI AudioSessionControl_GetSessionIdentifier(
        IAudioSessionControl2 *iface, WCHAR **id)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%p) - stub\n", This, id);

    return E_NOTIMPL;
}

static HRESULT WINAPI AudioSessionControl_GetSessionInstanceIdentifier(
        IAudioSessionControl2 *iface, WCHAR **id)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    FIXME("(%p)->(%p) - stub\n", This, id);

    return E_NOTIMPL;
}

static HRESULT WINAPI AudioSessionControl_GetProcessId(
        IAudioSessionControl2 *iface, DWORD *pid)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    TRACE("(%p)->(%p)\n", This, pid);

    if(!pid)
        return E_POINTER;

    *pid = GetCurrentProcessId();

    return S_OK;
}

static HRESULT WINAPI AudioSessionControl_IsSystemSoundsSession(
        IAudioSessionControl2 *iface)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    TRACE("(%p)\n", This);

    return S_FALSE;
}

static HRESULT WINAPI AudioSessionControl_SetDuckingPreference(
        IAudioSessionControl2 *iface, BOOL optout)
{
    ACImpl *This = impl_from_IAudioSessionControl2(iface);

    TRACE("(%p)->(%d)\n", This, optout);

    return S_OK;
}

static const IAudioSessionControl2Vtbl AudioSessionControl2_Vtbl =
{
    AudioSessionControl_QueryInterface,
    AudioSessionControl_AddRef,
    AudioSessionControl_Release,
    AudioSessionControl_GetState,
    AudioSessionControl_GetDisplayName,
    AudioSessionControl_SetDisplayName,
    AudioSessionControl_GetIconPath,
    AudioSessionControl_SetIconPath,
    AudioSessionControl_GetGroupingParam,
    AudioSessionControl_SetGroupingParam,
    AudioSessionControl_RegisterAudioSessionNotification,
    AudioSessionControl_UnregisterAudioSessionNotification,
    AudioSessionControl_GetSessionIdentifier,
    AudioSessionControl_GetSessionInstanceIdentifier,
    AudioSessionControl_GetProcessId,
    AudioSessionControl_IsSystemSoundsSession,
    AudioSessionControl_SetDuckingPreference
};

static HRESULT WINAPI SimpleAudioVolume_QueryInterface(
        ISimpleAudioVolume *iface, REFIID riid, void **ppv)
{
    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    if(!ppv)
        return E_POINTER;
    *ppv = NULL;

    if(IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISimpleAudioVolume))
        *ppv = iface;
    if(*ppv){
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    WARN("Unknown interface %s\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI SimpleAudioVolume_AddRef(ISimpleAudioVolume *iface)
{
    ACImpl *This = impl_from_ISimpleAudioVolume(iface);
    return IAudioClient_AddRef(&This->IAudioClient_iface);
}

static ULONG WINAPI SimpleAudioVolume_Release(ISimpleAudioVolume *iface)
{
    ACImpl *This = impl_from_ISimpleAudioVolume(iface);
    return IAudioClient_Release(&This->IAudioClient_iface);
}

static HRESULT WINAPI SimpleAudioVolume_SetMasterVolume(
        ISimpleAudioVolume *iface, float level, const GUID *context)
{
    ACImpl *This = impl_from_ISimpleAudioVolume(iface);

    FIXME("(%p)->(%f, %p) - stub\n", This, level, context);

    return E_NOTIMPL;
}

static HRESULT WINAPI SimpleAudioVolume_GetMasterVolume(
        ISimpleAudioVolume *iface, float *level)
{
    ACImpl *This = impl_from_ISimpleAudioVolume(iface);

    FIXME("(%p)->(%p) - stub\n", This, level);

    return E_NOTIMPL;
}

static HRESULT WINAPI SimpleAudioVolume_SetMute(ISimpleAudioVolume *iface,
        BOOL mute, const GUID *context)
{
    ACImpl *This = impl_from_ISimpleAudioVolume(iface);

    FIXME("(%p)->(%u, %p) - stub\n", This, mute, context);

    return E_NOTIMPL;
}

static HRESULT WINAPI SimpleAudioVolume_GetMute(ISimpleAudioVolume *iface,
        BOOL *mute)
{
    ACImpl *This = impl_from_ISimpleAudioVolume(iface);

    FIXME("(%p)->(%p) - stub\n", This, mute);

    return E_NOTIMPL;
}

static const ISimpleAudioVolumeVtbl SimpleAudioVolume_Vtbl  =
{
    SimpleAudioVolume_QueryInterface,
    SimpleAudioVolume_AddRef,
    SimpleAudioVolume_Release,
    SimpleAudioVolume_SetMasterVolume,
    SimpleAudioVolume_GetMasterVolume,
    SimpleAudioVolume_SetMute,
    SimpleAudioVolume_GetMute
};

static HRESULT WINAPI AudioClock_QueryInterface(IAudioClock *iface,
        REFIID riid, void **ppv)
{
    ACImpl *This = impl_from_IAudioClock(iface);

    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    if(!ppv)
        return E_POINTER;
    *ppv = NULL;

    if(IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IAudioClock))
        *ppv = iface;
    else if(IsEqualIID(riid, &IID_IAudioClock2))
        *ppv = &This->IAudioClock2_iface;
    if(*ppv){
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    WARN("Unknown interface %s\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI AudioClock_AddRef(IAudioClock *iface)
{
    ACImpl *This = impl_from_IAudioClock(iface);
    return IAudioClient_AddRef(&This->IAudioClient_iface);
}

static ULONG WINAPI AudioClock_Release(IAudioClock *iface)
{
    ACImpl *This = impl_from_IAudioClock(iface);
    return IAudioClient_Release(&This->IAudioClient_iface);
}

static HRESULT WINAPI AudioClock_GetFrequency(IAudioClock *iface, UINT64 *freq)
{
    ACImpl *This = impl_from_IAudioClock(iface);

    TRACE("(%p)->(%p)\n", This, freq);

    *freq = This->fmt->nSamplesPerSec;

    return S_OK;
}

static HRESULT WINAPI AudioClock_GetPosition(IAudioClock *iface, UINT64 *pos,
        UINT64 *qpctime)
{
    ACImpl *This = impl_from_IAudioClock(iface);
    UINT32 pad;
    HRESULT hr;

    TRACE("(%p)->(%p, %p)\n", This, pos, qpctime);

    if(!pos)
        return E_POINTER;

    EnterCriticalSection(&This->lock);

    hr = IAudioClient_GetCurrentPadding(&This->IAudioClient_iface, &pad);
    if(FAILED(hr)){
        LeaveCriticalSection(&This->lock);
        return hr;
    }

    if(This->dataflow == eRender)
        *pos = This->written_frames - pad;
    else if(This->dataflow == eCapture)
        *pos = This->written_frames + pad;

    LeaveCriticalSection(&This->lock);

    if(qpctime){
        LARGE_INTEGER stamp, freq;
        QueryPerformanceCounter(&stamp);
        QueryPerformanceFrequency(&freq);
        *qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    return S_OK;
}

static HRESULT WINAPI AudioClock_GetCharacteristics(IAudioClock *iface,
        DWORD *chars)
{
    ACImpl *This = impl_from_IAudioClock(iface);

    TRACE("(%p)->(%p)\n", This, chars);

    if(!chars)
        return E_POINTER;

    *chars = AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ;

    return S_OK;
}

static const IAudioClockVtbl AudioClock_Vtbl =
{
    AudioClock_QueryInterface,
    AudioClock_AddRef,
    AudioClock_Release,
    AudioClock_GetFrequency,
    AudioClock_GetPosition,
    AudioClock_GetCharacteristics
};

static HRESULT WINAPI AudioClock2_QueryInterface(IAudioClock2 *iface,
        REFIID riid, void **ppv)
{
    ACImpl *This = impl_from_IAudioClock2(iface);
    return IAudioClock_QueryInterface(&This->IAudioClock_iface, riid, ppv);
}

static ULONG WINAPI AudioClock2_AddRef(IAudioClock2 *iface)
{
    ACImpl *This = impl_from_IAudioClock2(iface);
    return IAudioClient_AddRef(&This->IAudioClient_iface);
}

static ULONG WINAPI AudioClock2_Release(IAudioClock2 *iface)
{
    ACImpl *This = impl_from_IAudioClock2(iface);
    return IAudioClient_Release(&This->IAudioClient_iface);
}

static HRESULT WINAPI AudioClock2_GetDevicePosition(IAudioClock2 *iface,
        UINT64 *pos, UINT64 *qpctime)
{
    ACImpl *This = impl_from_IAudioClock2(iface);

    FIXME("(%p)->(%p, %p)\n", This, pos, qpctime);

    return E_NOTIMPL;
}

static const IAudioClock2Vtbl AudioClock2_Vtbl =
{
    AudioClock2_QueryInterface,
    AudioClock2_AddRef,
    AudioClock2_Release,
    AudioClock2_GetDevicePosition
};