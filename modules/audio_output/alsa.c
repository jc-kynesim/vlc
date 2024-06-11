/*****************************************************************************
 * alsa.c : alsa plugin for vlc
 *****************************************************************************
 * Copyright (C) 2000-2010 VLC authors and VideoLAN
 * Copyright (C) 2009-2011 Rémi Denis-Courmont
 *
 * Authors: Henri Fallon <henri@videolan.org> - Original Author
 *          Jeffrey Baker <jwbaker@acm.org> - Port to ALSA 1.0 API
 *          John Paul Lorenti <jpl31@columbia.edu> - Device selection
 *          Arnaud de Bossoreille de Ribou <bozo@via.ecp.fr> - S/PDIF and aout3
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_dialog.h>
#include <vlc_aout.h>
#include <vlc_cpu.h>

#include <alsa/asoundlib.h>
#include <alsa/version.h>

#define TRACE_ALL 0

/** Private data for an ALSA PCM playback stream */
struct aout_sys_t
{
    snd_pcm_t *pcm;
    unsigned rate; /**< Sample rate */
    vlc_fourcc_t format; /**< Sample format */
    uint8_t chans_table[AOUT_CHAN_MAX]; /**< Channels order table */
    uint8_t chans_to_reorder; /**< Number of channels to reorder */

    bool soft_mute;
    float soft_gain;
    char *device;
    unsigned int pause_bytes;

    vlc_fourcc_t * passthrough_types;
};

#include "audio_output/volume.h"

enum {
    PASSTHROUGH_UNSET = -1,
    PASSTHROUGH_NONE = 0,
    PASSTHROUGH_SPDIF,
    PASSTHROUGH_HDMI,
};

static int Open (vlc_object_t *);
static void Close (vlc_object_t *);
static int EnumDevices (vlc_object_t *, char const *, char ***, char ***);

#define AUDIO_DEV_TEXT N_("Audio output device")
#define AUDIO_DEV_LONGTEXT N_("Audio output device (using ALSA syntax).")

#define AUDIO_CHAN_TEXT N_("Audio output channels")
#define AUDIO_CHAN_LONGTEXT N_("Channels available for audio output. " \
    "If the input has more channels than the output, it will be down-mixed. " \
    "This parameter is ignored when digital pass-through is active.")
static const int channels[] = {
    AOUT_CHAN_CENTER, AOUT_CHANS_STEREO, AOUT_CHANS_4_0, AOUT_CHANS_4_1,
    AOUT_CHANS_5_0, AOUT_CHANS_5_1, AOUT_CHANS_7_1,
};
static const char *const channels_text[] = {
    N_("Mono"), N_("Stereo"), N_("Surround 4.0"), N_("Surround 4.1"),
    N_("Surround 5.0"), N_("Surround 5.1"), N_("Surround 7.1"),
};

#define PASSTHROUGH_NAME "alsa-passthrough"
#define PASSTHROUGH_TEXT N_("Audio passthrough mode")
#define PASSTHROUGH_LONGTEXT N_("Audio passthrough mode. Defaults to 0 (none)")
static const int passthrough_modes[] = {
    PASSTHROUGH_UNSET, PASSTHROUGH_NONE, PASSTHROUGH_SPDIF, PASSTHROUGH_HDMI,
};
static const char *const passthrough_modes_text[] = {
    N_("unset"), N_("none"), N_("S/PDIF"), N_("HDMI"),
};

#define PASSTHROUGH_TYPES_NAME "alsa-passthrough-types"
#define PASSTHROUGH_TYPES_TEXT "List of codecs to accept for passthrough"
#define PASSTHROUGH_TYPES_LONGTEXT "List of codecs to accept for passthrough, comma separated. Default is to try everything."\
    "If this option is given then " PASSTHROUGH_NAME " defaults to HDMI"


vlc_module_begin ()
    set_shortname( "ALSA" )
    set_description( N_("ALSA audio output") )
    set_category( CAT_AUDIO )
    set_subcategory( SUBCAT_AUDIO_AOUT )
    add_string ("alsa-audio-device", "default",
                AUDIO_DEV_TEXT, AUDIO_DEV_LONGTEXT, false)
        change_string_cb (EnumDevices)
    add_integer ("alsa-audio-channels", AOUT_CHANS_FRONT,
                 AUDIO_CHAN_TEXT, AUDIO_CHAN_LONGTEXT, false)
        change_integer_list (channels, channels_text)
    add_integer (PASSTHROUGH_NAME, PASSTHROUGH_UNSET, PASSTHROUGH_TEXT,
                 PASSTHROUGH_LONGTEXT, false)
        change_integer_list (passthrough_modes, passthrough_modes_text)
    add_string(PASSTHROUGH_TYPES_NAME, NULL, PASSTHROUGH_TYPES_TEXT,
               PASSTHROUGH_TYPES_LONGTEXT, false)
    add_sw_gain ()
    set_capability( "audio output", 150 )
    set_callbacks( Open, Close )
vlc_module_end ()


static vlc_fourcc_t * parse_passthrough(audio_output_t * const aout, const char * const str)
{
    const char * p = str;
    size_t n = 2;
    vlc_fourcc_t * rv = NULL;
    vlc_fourcc_t * f;

    if (str == NULL)
        return NULL;

    while (*p != '\0')
        if (*p++ == ',')
            ++n;

    rv = malloc(sizeof(vlc_fourcc_t) * n);
    if (rv == NULL)
        return NULL;
    f = rv;

    if (strcasecmp(str, "none") == 0)
        goto done;

    for (p = str; *p != 0;)
    {
        unsigned int i;
        const char *c = strchrnul(p, ',');
        vlc_fourcc_t fcc = 0;

        static const struct {
            const char * str;
            vlc_fourcc_t val;
        } codecs[] = {
            {.str = "truehd", .val = VLC_CODEC_TRUEHD },
            {.str = "mlp",    .val = VLC_CODEC_MLP },
            {.str = "dts",    .val = VLC_CODEC_DTS },
            {.str = "dtshd",  .val = VLC_CODEC_DTS },
            {.str = "ac3",    .val = VLC_CODEC_A52 },
            {.str = "ac-3",   .val = VLC_CODEC_A52 },
            {.str = "eac3",   .val = VLC_CODEC_EAC3 },
            {.str = "eac-3",  .val = VLC_CODEC_EAC3 },
            {.str = "all",    .val = VLC_CODEC_UNKNOWN },
        };

        for (i = 0; i != ARRAY_SIZE(codecs); ++i)
        {
            if (strncasecmp(p, codecs[i].str, c - p) == 0)
            {
                fcc = codecs[i].val;
                break;
            }
        }

        if (fcc != 0)
            *f++ = fcc;
        else
            msg_Warn(aout, "Unknown codec type '%.*s'", (int)(c - p), p);

        if (*c == 0)
            break;

        p = c + 1;
    }

done:
    *f = 0;
    return rv;
}

/** Helper for ALSA -> VLC debugging output */
static void Dump (vlc_object_t *obj, const char *msg,
                  int (*cb)(void *, snd_output_t *), void *p)
{
    snd_output_t *output;
    char *str;

    if (unlikely(snd_output_buffer_open (&output)))
        return;

    int val = cb (p, output);
    if (val)
    {
        msg_Warn (obj, "cannot get info: %s", snd_strerror (val));
        return;
    }

    size_t len = snd_output_buffer_string (output, &str);
    if (len > 0 && str[len - 1])
        len--; /* strip trailing newline */
    msg_Dbg (obj, "%s%.*s", msg, (int)len, str);
    snd_output_close (output);
}
#define Dump(o, m, cb, p) \
        Dump(VLC_OBJECT(o), m, (int (*)(void *, snd_output_t *))(cb), p)

static void DumpDevice (vlc_object_t *obj, snd_pcm_t *pcm)
{
    snd_pcm_info_t *info;

    Dump (obj, " ", snd_pcm_dump, pcm);
    snd_pcm_info_alloca (&info);
    if (snd_pcm_info (pcm, info) == 0)
    {
        msg_Dbg (obj, " device name   : %s", snd_pcm_info_get_name (info));
        msg_Dbg (obj, " device ID     : %s", snd_pcm_info_get_id (info));
        msg_Dbg (obj, " subdevice name: %s",
                snd_pcm_info_get_subdevice_name (info));
    }
}

static void DumpDeviceStatus (vlc_object_t *obj, snd_pcm_t *pcm)
{
    snd_pcm_status_t *status;

    snd_pcm_status_alloca (&status);
    snd_pcm_status (pcm, status);
    Dump (obj, "current status:\n", snd_pcm_status_dump, status);
}
#define DumpDeviceStatus(o, p) DumpDeviceStatus(VLC_OBJECT(o), p)

#if (SND_LIB_VERSION >= 0x01001B)
static const uint16_t vlc_chans[] = {
    [SND_CHMAP_MONO] = AOUT_CHAN_CENTER,
    [SND_CHMAP_FL]   = AOUT_CHAN_LEFT,
    [SND_CHMAP_FR]   = AOUT_CHAN_RIGHT,
    [SND_CHMAP_RL]   = AOUT_CHAN_REARLEFT,
    [SND_CHMAP_RR]   = AOUT_CHAN_REARRIGHT,
    [SND_CHMAP_FC]   = AOUT_CHAN_CENTER,
    [SND_CHMAP_LFE]  = AOUT_CHAN_LFE,
    [SND_CHMAP_SL]   = AOUT_CHAN_MIDDLELEFT,
    [SND_CHMAP_SR]   = AOUT_CHAN_MIDDLERIGHT,
    [SND_CHMAP_RC]   = AOUT_CHAN_REARCENTER,
};
static_assert(AOUT_CHAN_MAX == 9, "Missing channel entries");

static int Map2Mask (vlc_object_t *obj, const snd_pcm_chmap_t *restrict map)
{
    uint16_t mask = 0;

    for (unsigned i = 0; i < map->channels; i++)
    {
        const unsigned pos = map->pos[i];
        uint_fast16_t vlc_chan = 0;

        if (pos < sizeof (vlc_chans) / sizeof (vlc_chans[0]))
            vlc_chan = vlc_chans[pos];
        if (vlc_chan == 0)
        {
            msg_Dbg (obj, " %s channel %u position %u", "unsupported", i, pos);
            return -1;
        }
        if (mask & vlc_chan)
        {
            msg_Dbg (obj, " %s channel %u position %u", "duplicate", i, pos);
            return -1;
        }
        mask |= vlc_chan;
    }
    return mask;
}

/**
 * Compares a fixed ALSA channels map with the VLC channels order.
 */
static unsigned SetupChannelsFixed(const snd_pcm_chmap_t *restrict map,
                               uint16_t *restrict maskp, uint8_t *restrict tab)
{
    uint32_t chans_out[AOUT_CHAN_MAX];
    uint16_t mask = 0;

    for (unsigned i = 0; i < map->channels; i++)
    {
        uint_fast16_t vlc_chan = vlc_chans[map->pos[i]];

        chans_out[i] = vlc_chan;
        mask |= vlc_chan;
    }

    *maskp = mask;
    return aout_CheckChannelReorder(NULL, chans_out, mask, tab);
}

/**
 * Negotiate channels mapping.
 */
static unsigned SetupChannels (vlc_object_t *obj, snd_pcm_t *pcm,
                               uint16_t *restrict mask, uint8_t *restrict tab)
{
    snd_pcm_chmap_query_t **maps = snd_pcm_query_chmaps (pcm);
    if (maps == NULL)
    {   /* Fallback to default order if unknown */
        msg_Dbg(obj, "channels map not provided");
        return 0;
    }

    /* Find most appropriate available channels map */
    unsigned best_offset, best_score = 0, to_reorder = 0;

    for (snd_pcm_chmap_query_t *const *p = maps; *p != NULL; p++)
    {
        snd_pcm_chmap_query_t *map = *p;

        switch (map->type)
        {
            case SND_CHMAP_TYPE_FIXED:
            case SND_CHMAP_TYPE_PAIRED:
            case SND_CHMAP_TYPE_VAR:
                break;
            default:
                msg_Err (obj, "unknown channels map type %u", map->type);
                continue;
        }

        int chans = Map2Mask (obj, &map->map);
        if (chans == -1)
            continue;

        unsigned score = (popcount (chans & *mask) << 8)
                       | (255 - popcount (chans));
        if (score > best_score)
        {
            best_offset = p - maps;
            best_score = score;
        }
    }

    if (best_score == 0)
    {
        msg_Err (obj, "cannot find supported channels map");
        goto out;
    }

    const snd_pcm_chmap_t *map = &maps[best_offset]->map;
    msg_Dbg (obj, "using channels map %u, type %u, %u channel(s)", best_offset,
             maps[best_offset]->type, map->channels);

    /* Setup channels map */
    to_reorder = SetupChannelsFixed(map, mask, tab);

    /* TODO: avoid reordering for PAIRED and VAR types */
    //snd_pcm_set_chmap (pcm, ...)
out:
    snd_pcm_free_chmaps (maps);
    return to_reorder;
}
#else /* (SND_LIB_VERSION < 0x01001B) */
# define SetupChannels(obj, pcm, mask, tab) (0)
#endif

static int TimeGet (audio_output_t *aout, vlc_tick_t *);
static void Play (audio_output_t *, block_t *);
static void Pause (audio_output_t *, bool, mtime_t);
static void PauseDummy (audio_output_t *, bool, mtime_t);
static void Flush (audio_output_t *, bool);

/** Initializes an ALSA playback stream */
static int Start (audio_output_t *aout, audio_sample_format_t *restrict fmt)
{
    aout_sys_t *sys = aout->sys;
    snd_pcm_format_t pcm_format; /* ALSA sample format */
    unsigned channels;
    int passthrough = PASSTHROUGH_NONE;
    snd_pcm_uframes_t periodSizeMax;
    snd_pcm_uframes_t periodSize;
    snd_pcm_uframes_t bufferSize;
    unsigned int req_rate = fmt->i_rate;
    vlc_fourcc_t req_format = fmt->i_format;

    msg_Dbg(aout, "Start: Format: %.4s, Chans: %d, Rate:%d", (char*)&fmt->i_format, aout_FormatNbChannels(fmt), fmt->i_rate);

    if (aout_FormatNbChannels(fmt) == 0 && AOUT_FMT_LINEAR(fmt))
        return VLC_EGENERIC;

    sys->pause_bytes = 0;
    switch (fmt->i_format)
    {
        case VLC_CODEC_FL64:
            pcm_format = SND_PCM_FORMAT_FLOAT64;
            break;
        case VLC_CODEC_FL32:
            pcm_format = SND_PCM_FORMAT_FLOAT;
            break;
        case VLC_CODEC_S32N:
            pcm_format = SND_PCM_FORMAT_S32;
            break;
        case VLC_CODEC_S16N:
            pcm_format = SND_PCM_FORMAT_S16;
            break;
        case VLC_CODEC_U8:
            pcm_format = SND_PCM_FORMAT_U8;
            break;
        default:
            if (AOUT_FMT_SPDIF(fmt) || AOUT_FMT_HDMI(fmt))
            {
                if (sys->passthrough_types != NULL)
                {
                    // VLC_CODEC_UNKNOWN used as explicit "all"
                    const vlc_fourcc_t *p;
                    for (p = sys->passthrough_types; *p != 0 || *p == VLC_CODEC_UNKNOWN; ++p)
                        if (*p == fmt->i_format)
                            break;
                    if (*p == 0)
                    {
                        msg_Dbg(aout, "Codec %.4s not in passthrough-types", (const char *)&fmt->i_format);
                        return VLC_EGENERIC;
                    }
                }

                passthrough = var_InheritInteger(aout, PASSTHROUGH_NAME);
                // Explicit passthrough will override spdif
                if (passthrough == PASSTHROUGH_UNSET)
                    passthrough =
                        var_InheritBool(aout, "spdif") ? PASSTHROUGH_SPDIF :
                        sys->passthrough_types != NULL ? PASSTHROUGH_HDMI : PASSTHROUGH_NONE;
                msg_Dbg(aout, "Passthrough %d for format %4.4s", passthrough, (const char *)&fmt->i_format);
            }

            if (passthrough != PASSTHROUGH_NONE)
            {
                req_format = VLC_CODEC_SPDIFL;
                pcm_format = SND_PCM_FORMAT_S16;
                sys->pause_bytes = 3 * 4;
                channels    = 2;

                switch (fmt->i_format) {
                    case VLC_CODEC_MLP:
                    case VLC_CODEC_TRUEHD:
                        sys->pause_bytes = 4 * 4;
                        req_rate   = fmt->i_rate * 4;
                        channels   = 8;
                        break;

                    case VLC_CODEC_DTS:
                    {
                        if (passthrough == PASSTHROUGH_SPDIF)
                            break;
                        req_rate   = 192000;
                        channels   = 8;
                        break;
                    }

                    case VLC_CODEC_EAC3:
                        sys->pause_bytes = 4 * 4;
                        req_rate   = fmt->i_rate * 4;
                        break;

                    default:
                        break;
                }
            }
            else
            if (HAVE_FPU)
            {
                req_format = VLC_CODEC_FL32;
                pcm_format = SND_PCM_FORMAT_FLOAT;
            }
            else
            {
                req_format = VLC_CODEC_S16N;
                pcm_format = SND_PCM_FORMAT_S16;
            }
            break;
    }

    const char *device = sys->device;

    /* Choose the IEC device for S/PDIF output */
    char sep = '\0';
    if (passthrough != PASSTHROUGH_NONE)
    {
        const char *opt = NULL;

        if (!strcmp (device, "default"))
            device = (passthrough == PASSTHROUGH_HDMI) ? "hdmi" : "iec958";

        if (!strncmp (device, "iec958", 6))
            opt = device + 6;
        if (!strncmp (device, "hdmi", 4))
            opt = device + 4;

        if (opt != NULL)
            switch (*opt)
            {
                case ':':  sep = ','; break;
                case '\0': sep = ':'; break;
            }
    }

    char *devbuf = NULL;
    if (sep != '\0')
    {
        unsigned aes3;

        switch (fmt->i_rate)
        {
#define FS(freq) \
            case freq: aes3 = IEC958_AES3_CON_FS_ ## freq; break;
            FS( 44100) /* def. */ FS( 48000) FS( 32000)
            FS( 22050)            FS( 24000)
            FS( 88200) FS(768000) FS( 96000)
            FS(176400)
#undef FS
            case 192000:
                aes3 = (passthrough == PASSTHROUGH_HDMI && channels == 8) ?
                    IEC958_AES3_CON_FS_768000 : IEC958_AES3_CON_FS_192000;
                break;
            default:
                aes3 = IEC958_AES3_CON_FS_NOTID;
                break;
        }

        if (asprintf (&devbuf, "%s%cAES0=0x%x,AES1=0x%x,AES2=0x%x,AES3=0x%x",
                      device, sep,
                      IEC958_AES0_CON_EMPHASIS_NONE | IEC958_AES0_NONAUDIO,
                      IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER,
                      0, aes3) == -1)
            return VLC_ENOMEM;
        device = devbuf;
    }

    /* Open the device */
    snd_pcm_t *pcm;
    /* VLC always has a resampler. No need for ALSA's. */
    const int mode = SND_PCM_NO_AUTO_RESAMPLE;

    int val = snd_pcm_open (&pcm, device, SND_PCM_STREAM_PLAYBACK, mode);
    if (val != 0)
    {
        msg_Err (aout, "cannot open ALSA device \"%s\": %s", device,
                 snd_strerror (val));
        vlc_dialog_display_error (aout, _("Audio output failed"),
            _("The audio device \"%s\" could not be used:\n%s."),
            sys->device, snd_strerror (val));
        free (devbuf);
        return VLC_EGENERIC;
    }
    sys->pcm = pcm;

    /* Print some potentially useful debug */
    msg_Dbg (aout, "using ALSA device: %s", device);
    free (devbuf);
    DumpDevice (VLC_OBJECT(aout), pcm);

    /* Get Initial hardware parameters */
    snd_pcm_hw_params_t *hw;
    unsigned param;

    snd_pcm_hw_params_alloca (&hw);
    snd_pcm_hw_params_any (pcm, hw);
    Dump (aout, "initial hardware setup:\n", snd_pcm_hw_params_dump, hw);

    val = snd_pcm_hw_params_set_rate_resample(pcm, hw, 0);
    if (val)
    {
        msg_Err (aout, "cannot disable resampling: %s", snd_strerror (val));
        goto error;
    }

    val = snd_pcm_hw_params_set_access (pcm, hw,
                                        SND_PCM_ACCESS_RW_INTERLEAVED);
    if (val)
    {
        msg_Err (aout, "cannot set access mode: %s", snd_strerror (val));
        goto error;
    }

    /* Set sample format */
    if (snd_pcm_hw_params_test_format (pcm, hw, pcm_format) == 0)
        ;
    else
    if (passthrough != PASSTHROUGH_NONE)
    {
        msg_Warn(aout, "Failed to set required passthrough format");
        goto error;
    }
    else
    if (snd_pcm_hw_params_test_format(pcm, hw, SND_PCM_FORMAT_FLOAT) == 0)
    {
        req_format = VLC_CODEC_FL32;
        pcm_format = SND_PCM_FORMAT_FLOAT;
    }
    else
    if (snd_pcm_hw_params_test_format (pcm, hw, SND_PCM_FORMAT_S32) == 0)
    {
        req_format = VLC_CODEC_S32N;
        pcm_format = SND_PCM_FORMAT_S32;
    }
    else
    if (snd_pcm_hw_params_test_format (pcm, hw, SND_PCM_FORMAT_S16) == 0)
    {
        req_format = VLC_CODEC_S16N;
        pcm_format = SND_PCM_FORMAT_S16;
    }
    else
    {
        msg_Err (aout, "no supported sample format");
        goto error;
    }

    val = snd_pcm_hw_params_set_format (pcm, hw, pcm_format);
    if (val)
    {
        msg_Err (aout, "cannot set sample format: %s", snd_strerror (val));
        goto error;
    }
    sys->format = req_format;

    /* Set channels count */
    if (passthrough == PASSTHROUGH_NONE)
    {
        uint16_t map = var_InheritInteger (aout, "alsa-audio-channels");

        sys->chans_to_reorder = SetupChannels (VLC_OBJECT(aout), pcm, &map,
                                               sys->chans_table);
        fmt->i_physical_channels = map;
        channels = popcount (map);
    }
    else
    {
        sys->chans_to_reorder = 0;
    }

    /* By default, ALSA plug will pad missing channels with zeroes, which is
     * usually fine. However, it will also discard extraneous channels, which
     * is not acceptable. Thus the user must configure the physically
     * available channels, and VLC will downmix if needed. */
    val = snd_pcm_hw_params_set_channels (pcm, hw, channels);
    if (val)
    {
        msg_Err (aout, "cannot set %u channels: %s", channels,
                 snd_strerror (val));
        goto error;
    }

    /* Set sample rate */
    sys->rate = req_rate;
    val = snd_pcm_hw_params_set_rate_near (pcm, hw, &sys->rate, NULL);
    if (val)
    {
        msg_Err (aout, "cannot set sample rate: %s", snd_strerror (val));
        goto error;
    }
    if (passthrough != PASSTHROUGH_NONE && sys->rate != req_rate)
    {
        msg_Warn(aout, "Passthrough requires rate %d, got %d", req_rate, sys->rate);
        goto error;
    }

    bufferSize    = req_rate / 10;  // 100ms - bigger than this & truehd goes unhappy?
    periodSize    = bufferSize / 4;
    periodSizeMax = bufferSize / 3;
    snd_pcm_hw_params_set_period_size_max(pcm, hw, &periodSizeMax, NULL);

    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufferSize);
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &periodSize, NULL);
#if 1 /* work-around for period-long latency outputs (e.g. PulseAudio): */
    param = AOUT_MIN_PREPARE_TIME;
    val = snd_pcm_hw_params_set_period_time_near (pcm, hw, &param, NULL);
    if (val)
    {
        msg_Err (aout, "cannot set period: %s", snd_strerror (val));
        goto error;
    }
#endif
    /* Set buffer size */
    param = AOUT_MAX_ADVANCE_TIME;
    val = snd_pcm_hw_params_set_buffer_time_near (pcm, hw, &param, NULL);
    if (val)
    {
        msg_Err (aout, "cannot set buffer duration: %s", snd_strerror (val));
        goto error;
    }
#if 0
    val = snd_pcm_hw_params_get_buffer_time (hw, &param, NULL);
    if (val)
    {
        msg_Warn (aout, "cannot get buffer time: %s", snd_strerror(val));
        param = AOUT_MIN_PREPARE_TIME;
    }
    else
        param /= 2;
    val = snd_pcm_hw_params_set_period_time_near (pcm, hw, &param, NULL);
    if (val)
    {
        msg_Err (aout, "cannot set period: %s", snd_strerror (val));
        goto error;
    }
#endif

    /* Commit hardware parameters */
    val = snd_pcm_hw_params (pcm, hw);
    if (val < 0)
    {
        msg_Err (aout, "cannot commit hardware parameters: %s",
                 snd_strerror (val));
        goto error;
    }
    Dump (aout, "final HW setup:\n", snd_pcm_hw_params_dump, hw);

    /* Get Initial software parameters */
    snd_pcm_sw_params_t *sw;

    snd_pcm_sw_params_alloca (&sw);
    snd_pcm_sw_params_current (pcm, sw);
    Dump (aout, "initial software parameters:\n", snd_pcm_sw_params_dump, sw);

    /* START REVISIT */
    //snd_pcm_sw_params_set_avail_min( pcm, sw, i_period_size );
    // FIXME: useful?
    val = snd_pcm_sw_params_set_start_threshold (pcm, sw, 1);
    if( val < 0 )
    {
        msg_Err( aout, "unable to set start threshold (%s)",
                 snd_strerror( val ) );
        goto error;
    }
    /* END REVISIT */

    /* Commit software parameters. */
    val = snd_pcm_sw_params (pcm, sw);
    if (val)
    {
        msg_Err (aout, "cannot commit software parameters: %s",
                 snd_strerror (val));
        goto error;
    }
    Dump (aout, "final software parameters:\n", snd_pcm_sw_params_dump, sw);

    val = snd_pcm_prepare (pcm);
    if (val)
    {
        msg_Err (aout, "cannot prepare device: %s", snd_strerror (val));
        goto error;
    }

    /* Setup audio_output_t */
    fmt->i_frame_length = 1;
    fmt->i_bytes_per_frame = snd_pcm_frames_to_bytes(pcm, fmt->i_frame_length);
    fmt->i_channels = channels;
    fmt->i_rate = sys->rate;
    fmt->i_format = sys->format;
    fmt->channel_type = AUDIO_CHANNEL_TYPE_BITMAP;

    aout->time_get = TimeGet;
    aout->play = Play;
    if (snd_pcm_hw_params_can_pause (hw))
        aout->pause = Pause;
    else
    {
        aout->pause = PauseDummy;
        msg_Warn (aout, "device cannot be paused");
    }
    aout->flush = Flush;
    aout_SoftVolumeStart (aout);
    return 0;

error:
    snd_pcm_close (pcm);
    return VLC_EGENERIC;
}

static int TimeGet (audio_output_t *aout, vlc_tick_t *restrict delay)
{
    aout_sys_t *sys = aout->sys;
    snd_pcm_sframes_t frames;

    int val = snd_pcm_delay (sys->pcm, &frames);
    if (val)
    {
        msg_Err (aout, "cannot estimate delay: %s", snd_strerror (val));
        return -1;
    }
    *delay = (uint_fast64_t)frames * CLOCK_FREQ / sys->rate;
    return 0;
}

/**
 * Queues one audio buffer to the hardware.
 */
static void Play (audio_output_t *aout, block_t *block)
{
    aout_sys_t *sys = aout->sys;

#if TRACE_ALL
    static mtime_t last_pts = 0;
    msg_Dbg(aout, "<<< %s: PTS: %"PRId64" samples: %u, bytes: %zu, delta: %"PRId64, __func__,
            block->i_pts, block->i_nb_samples, block->i_buffer,
            block->i_pts - last_pts);
    last_pts = block->i_pts;
#endif

    // S/pdif packets always start with sync so if no sync then this must
    // be a padding buffer
    if (sys->pause_bytes != 0 && block->p_buffer[0] == 0)
    {
        static const uint8_t pause_le[16] = {0x72, 0xf8, 0x1f, 0x4e, 3, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const uint8_t pause_be[16] = {0xf8, 0x72, 0x4e, 0x1f, 0, 3, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0};
        const uint8_t *const pause = sys->format == VLC_CODEC_SPDIFB ? pause_be : pause_le;
        size_t n = block->i_buffer / sys->pause_bytes;
        size_t i;

        msg_Dbg(aout, "Silence detected");
        for (i = 0; i != n; ++i)
            memcpy(block->p_buffer + i * sys->pause_bytes, pause, sys->pause_bytes);
    }

    if (sys->chans_to_reorder != 0)
        aout_ChannelReorder(block->p_buffer, block->i_buffer,
                           sys->chans_to_reorder, sys->chans_table, sys->format);

    snd_pcm_t *pcm = sys->pcm;

    /* TODO: better overflow handling */
    /* TODO: no period wake ups */

    while (block->i_nb_samples > 0)
    {
        snd_pcm_sframes_t frames;

        frames = snd_pcm_writei (pcm, block->p_buffer, block->i_nb_samples);
        if (frames >= 0)
        {
            size_t bytes = snd_pcm_frames_to_bytes (pcm, frames);
            block->i_nb_samples -= frames;
            block->p_buffer += bytes;
            block->i_buffer -= bytes;
            // pts, length
        }
        else
        {
            int val = snd_pcm_recover (pcm, frames, 1);
            if (val)
            {
                msg_Err (aout, "cannot recover playback stream: %s",
                         snd_strerror (val));
                DumpDeviceStatus (aout, pcm);
                break;
            }
            msg_Warn (aout, "cannot write samples: %s", snd_strerror (frames));
        }
    }
    block_Release (block);
}

/**
 * Pauses/resumes the audio playback.
 */
static void Pause (audio_output_t *aout, bool pause, vlc_tick_t date)
{
    snd_pcm_t *pcm = aout->sys->pcm;

    int val = snd_pcm_pause (pcm, pause);
    if (unlikely(val))
        PauseDummy (aout, pause, date);
}

static void PauseDummy (audio_output_t *aout, bool pause, vlc_tick_t date)
{
    snd_pcm_t *pcm = aout->sys->pcm;

    /* Stupid device cannot pause. Discard samples. */
    if (pause)
        snd_pcm_drop (pcm);
    else
        snd_pcm_prepare (pcm);
    (void) date;
}

/**
 * Flushes/drains the audio playback buffer.
 */
static void Flush (audio_output_t *aout, bool wait)
{
    snd_pcm_t *pcm = aout->sys->pcm;

    if (wait)
        snd_pcm_drain (pcm);
    else
        snd_pcm_drop (pcm);
    snd_pcm_prepare (pcm);
}


/**
 * Releases the audio output.
 */
static void Stop (audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;
    snd_pcm_t *pcm = sys->pcm;

    snd_pcm_drop (pcm);
    snd_pcm_close (pcm);
}

/**
 * Enumerates ALSA output devices.
 */
static int EnumDevices(vlc_object_t *obj, char const *varname,
                       char ***restrict idp, char ***restrict namep)
{
    void **hints;

    msg_Dbg (obj, "Available ALSA PCM devices:");
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return -1;

    char **ids = NULL, **names = NULL;
    unsigned n = 0;
    bool hinted_default = false;

    for (size_t i = 0; hints[i] != NULL; i++)
    {
        void *hint = hints[i];

        char *name = snd_device_name_get_hint(hint, "NAME");
        if (unlikely(name == NULL))
            continue;

        char *desc = snd_device_name_get_hint(hint, "DESC");
        if (desc == NULL)
            desc = xstrdup (name);
        for (char *lf = strchr(desc, '\n'); lf; lf = strchr(lf, '\n'))
            *lf = ' ';
        msg_Dbg (obj, "%s (%s)", (desc != NULL) ? desc : name, name);

        ids = xrealloc (ids, (n + 1) * sizeof (*ids));
        names = xrealloc (names, (n + 1) * sizeof (*names));
        ids[n] = name;
        names[n] = desc;
        n++;

        if (!strcmp(name, "default"))
            hinted_default = true;
    }

    snd_device_name_free_hint(hints);

    if (!hinted_default)
    {
        ids = xrealloc (ids, (n + 1) * sizeof (*ids));
        names = xrealloc (names, (n + 1) * sizeof (*names));
        ids[n] = xstrdup ("default");
        names[n] = xstrdup (_("Default"));
        n++;
    }

    *idp = ids;
    *namep = names;
    (void) varname;
    return n;
}

static int DeviceSelect (audio_output_t *aout, const char *id)
{
    aout_sys_t *sys = aout->sys;

    char *device = strdup (id ? id : "default");
    if (unlikely(device == NULL))
        return -1;

    free (sys->device);
    sys->device = device;
    aout_DeviceReport (aout, device);
    aout_RestartRequest (aout, AOUT_RESTART_OUTPUT);
    return 0;
}

static int Open(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;
    aout_sys_t *sys = calloc (1, sizeof (*sys));

    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    sys->device = var_InheritString (aout, "alsa-audio-device");
    if (unlikely(sys->device == NULL))
        goto error;

    aout->sys = sys;
    aout->start = Start;
    aout->stop = Stop;
    aout_SoftVolumeInit (aout);
    aout->device_select = DeviceSelect;
    aout_DeviceReport (aout, sys->device);

    /* ALSA does not support hot-plug events so list devices at startup */
    char **ids, **names;
    int count = EnumDevices (VLC_OBJECT(aout), NULL, &ids, &names);
    if (count >= 0)
    {
        for (int i = 0; i < count; i++)
        {
            aout_HotplugReport (aout, ids[i], names[i]);
            free (names[i]);
            free (ids[i]);
        }
        free (names);
        free (ids);
    }

    {
        const char *types = var_InheritString(aout, PASSTHROUGH_TYPES_NAME);
        sys->passthrough_types = parse_passthrough(aout, types);
        free((void *)types);
    }

    return VLC_SUCCESS;
error:
    free (sys);
    return VLC_ENOMEM;
}

static void Close(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;
    aout_sys_t *sys = aout->sys;

    free (sys->passthrough_types);
    free (sys->device);
    free (sys);
}
