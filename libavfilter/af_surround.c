/*
 * Copyright (c) 2017 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/tx.h"
#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "internal.h"
#include "formats.h"
#include "window_func.h"

typedef struct AudioSurroundContext {
    const AVClass *class;

    char *out_channel_layout_str;
    char *in_channel_layout_str;

    float level_in;
    float level_out;
    float fc_in;
    float fc_out;
    float fl_in;
    float fl_out;
    float fr_in;
    float fr_out;
    float sl_in;
    float sl_out;
    float sr_in;
    float sr_out;
    float bl_in;
    float bl_out;
    float br_in;
    float br_out;
    float bc_in;
    float bc_out;
    float lfe_in;
    float lfe_out;
    int   lfe_mode;
    float angle;
    float focus;
    int   win_size;
    int   win_func;
    float overlap;

    float all_x;
    float all_y;

    float fc_x;
    float fl_x;
    float fr_x;
    float bl_x;
    float br_x;
    float sl_x;
    float sr_x;
    float bc_x;

    float fc_y;
    float fl_y;
    float fr_y;
    float bl_y;
    float br_y;
    float sl_y;
    float sr_y;
    float bc_y;

    float *input_levels;
    float *output_levels;
    int output_lfe;
    int have_lfe;
    int lowcutf;
    int highcutf;

    float lowcut;
    float highcut;

    AVChannelLayout out_ch_layout;
    AVChannelLayout in_ch_layout;
    int nb_in_channels;
    int nb_out_channels;

    AVFrame *input_in;
    AVFrame *input;
    AVFrame *output;
    AVFrame *output_out;
    AVFrame *overlap_buffer;
    AVFrame *window;

    float *x_pos;
    float *y_pos;
    float *l_phase;
    float *r_phase;
    float *c_phase;
    float *lfe_mag;
    float *mag_total;

    int rdft_size;
    int hop_size;
    AVTXContext **rdft, **irdft;
    av_tx_fn tx_fn, itx_fn;
    float *window_func_lut;

    void (*filter)(AVFilterContext *ctx);
    int (*upmix)(AVFilterContext *ctx, int ch);
    void (*upmix_2_1)(AVFilterContext *ctx,
                      float l_phase,
                      float r_phase,
                      float c_phase,
                      float mag_total,
                      float lfe_im,
                      float lfe_re,
                      float x, float y,
                      int n);
    void (*upmix_3_0)(AVFilterContext *ctx,
                      float l_phase,
                      float r_phase,
                      float c_mag,
                      float c_phase,
                      float mag_total,
                      float x, float y,
                      int n);
    void (*upmix_5_0)(AVFilterContext *ctx,
                      float c_re, float c_im,
                      float mag_totall, float mag_totalr,
                      float fl_phase, float fr_phase,
                      float bl_phase, float br_phase,
                      float sl_phase, float sr_phase,
                      float xl, float yl,
                      float xr, float yr,
                      int n);
    void (*upmix_5_1)(AVFilterContext *ctx,
                      float c_re, float c_im,
                      float lfe_re, float lfe_im,
                      float mag_totall, float mag_totalr,
                      float fl_phase, float fr_phase,
                      float bl_phase, float br_phase,
                      float sl_phase, float sr_phase,
                      float xl, float yl,
                      float xr, float yr,
                      int n);
} AudioSurroundContext;

static int query_formats(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    int ret;

    ret = ff_add_format(&formats, AV_SAMPLE_FMT_FLTP);
    if (ret)
        return ret;
    ret = ff_set_common_formats(ctx, formats);
    if (ret)
        return ret;

    layouts = NULL;
    ret = ff_add_channel_layout(&layouts, &s->out_ch_layout);
    if (ret)
        return ret;

    ret = ff_channel_layouts_ref(layouts, &ctx->outputs[0]->incfg.channel_layouts);
    if (ret)
        return ret;

    layouts = NULL;
    ret = ff_add_channel_layout(&layouts, &s->in_ch_layout);
    if (ret)
        return ret;

    ret = ff_channel_layouts_ref(layouts, &ctx->inputs[0]->outcfg.channel_layouts);
    if (ret)
        return ret;

    return ff_set_common_all_samplerates(ctx);
}

static void set_input_levels(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;
    int ch;

    ch = av_channel_layout_index_from_channel(&s->in_ch_layout, AV_CHAN_FRONT_CENTER);
    if (ch >= 0)
        s->input_levels[ch] = s->fc_in;
    ch = av_channel_layout_index_from_channel(&s->in_ch_layout, AV_CHAN_FRONT_LEFT);
    if (ch >= 0)
        s->input_levels[ch] = s->fl_in;
    ch = av_channel_layout_index_from_channel(&s->in_ch_layout, AV_CHAN_FRONT_RIGHT);
    if (ch >= 0)
        s->input_levels[ch] = s->fr_in;
    ch = av_channel_layout_index_from_channel(&s->in_ch_layout, AV_CHAN_SIDE_LEFT);
    if (ch >= 0)
        s->input_levels[ch] = s->sl_in;
    ch = av_channel_layout_index_from_channel(&s->in_ch_layout, AV_CHAN_SIDE_RIGHT);
    if (ch >= 0)
        s->input_levels[ch] = s->sr_in;
    ch = av_channel_layout_index_from_channel(&s->in_ch_layout, AV_CHAN_BACK_LEFT);
    if (ch >= 0)
        s->input_levels[ch] = s->bl_in;
    ch = av_channel_layout_index_from_channel(&s->in_ch_layout, AV_CHAN_BACK_RIGHT);
    if (ch >= 0)
        s->input_levels[ch] = s->br_in;
    ch = av_channel_layout_index_from_channel(&s->in_ch_layout, AV_CHAN_BACK_CENTER);
    if (ch >= 0)
        s->input_levels[ch] = s->bc_in;
    ch = av_channel_layout_index_from_channel(&s->in_ch_layout, AV_CHAN_LOW_FREQUENCY);
    if (ch >= 0)
        s->input_levels[ch] = s->lfe_in;

    for (ch = 0;  ch < s->nb_in_channels && s->level_in >= 0.f; ch++)
        s->input_levels[ch] = s->level_in;
    s->level_in = -1.f;
}

static void set_output_levels(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;
    int ch;

    ch = av_channel_layout_index_from_channel(&s->out_ch_layout, AV_CHAN_FRONT_CENTER);
    if (ch >= 0)
        s->output_levels[ch] = s->fc_out;
    ch = av_channel_layout_index_from_channel(&s->out_ch_layout, AV_CHAN_FRONT_LEFT);
    if (ch >= 0)
        s->output_levels[ch] = s->fl_out;
    ch = av_channel_layout_index_from_channel(&s->out_ch_layout, AV_CHAN_FRONT_RIGHT);
    if (ch >= 0)
        s->output_levels[ch] = s->fr_out;
    ch = av_channel_layout_index_from_channel(&s->out_ch_layout, AV_CHAN_SIDE_LEFT);
    if (ch >= 0)
        s->output_levels[ch] = s->sl_out;
    ch = av_channel_layout_index_from_channel(&s->out_ch_layout, AV_CHAN_SIDE_RIGHT);
    if (ch >= 0)
        s->output_levels[ch] = s->sr_out;
    ch = av_channel_layout_index_from_channel(&s->out_ch_layout, AV_CHAN_BACK_LEFT);
    if (ch >= 0)
        s->output_levels[ch] = s->bl_out;
    ch = av_channel_layout_index_from_channel(&s->out_ch_layout, AV_CHAN_BACK_RIGHT);
    if (ch >= 0)
        s->output_levels[ch] = s->br_out;
    ch = av_channel_layout_index_from_channel(&s->out_ch_layout, AV_CHAN_BACK_CENTER);
    if (ch >= 0)
        s->output_levels[ch] = s->bc_out;
    ch = av_channel_layout_index_from_channel(&s->out_ch_layout, AV_CHAN_LOW_FREQUENCY);
    if (ch >= 0)
        s->output_levels[ch] = s->lfe_out;

    for (ch = 0;  ch < s->nb_out_channels && s->level_out >= 0.f; ch++)
        s->output_levels[ch] = s->level_out;
    s->level_out = -1.f;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    AudioSurroundContext *s = ctx->priv;
    int ret;

    s->rdft = av_calloc(inlink->ch_layout.nb_channels, sizeof(*s->rdft));
    if (!s->rdft)
        return AVERROR(ENOMEM);
    s->nb_in_channels = inlink->ch_layout.nb_channels;

    for (int ch = 0; ch < inlink->ch_layout.nb_channels; ch++) {
        float scale = 1.f;

        ret = av_tx_init(&s->rdft[ch], &s->tx_fn, AV_TX_FLOAT_RDFT,
                         0, s->win_size, &scale, 0);
        if (ret < 0)
            return ret;
    }
    s->input_levels = av_malloc_array(s->nb_in_channels, sizeof(*s->input_levels));
    if (!s->input_levels)
        return AVERROR(ENOMEM);

    set_input_levels(ctx);

    s->window = ff_get_audio_buffer(inlink, s->win_size * 2);
    if (!s->window)
        return AVERROR(ENOMEM);

    s->input_in = ff_get_audio_buffer(inlink, s->win_size * 2);
    if (!s->input_in)
        return AVERROR(ENOMEM);

    s->input = ff_get_audio_buffer(inlink, s->win_size + 2);
    if (!s->input)
        return AVERROR(ENOMEM);

    s->lowcut = 1.f * s->lowcutf / (inlink->sample_rate * 0.5) * (s->win_size / 2);
    s->highcut = 1.f * s->highcutf / (inlink->sample_rate * 0.5) * (s->win_size / 2);

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioSurroundContext *s = ctx->priv;
    int ret;

    s->irdft = av_calloc(outlink->ch_layout.nb_channels, sizeof(*s->irdft));
    if (!s->irdft)
        return AVERROR(ENOMEM);
    s->nb_out_channels = outlink->ch_layout.nb_channels;

    for (int ch = 0; ch < outlink->ch_layout.nb_channels; ch++) {
        float iscale = 1.f;

        ret = av_tx_init(&s->irdft[ch], &s->itx_fn, AV_TX_FLOAT_RDFT,
                         1, s->win_size, &iscale, 0);
        if (ret < 0)
            return ret;
    }

    s->output_levels = av_malloc_array(s->nb_out_channels, sizeof(*s->output_levels));
    if (!s->output_levels)
        return AVERROR(ENOMEM);

    set_output_levels(ctx);

    s->output_out = ff_get_audio_buffer(outlink, s->win_size + 2);
    s->output = ff_get_audio_buffer(outlink, s->win_size + 2);
    s->overlap_buffer = ff_get_audio_buffer(outlink, s->win_size * 2);
    if (!s->overlap_buffer || !s->output || !s->output_out)
        return AVERROR(ENOMEM);

    s->rdft_size = s->win_size / 2 + 1;

    s->x_pos = av_calloc(s->rdft_size, sizeof(*s->x_pos));
    s->y_pos = av_calloc(s->rdft_size, sizeof(*s->y_pos));
    s->l_phase = av_calloc(s->rdft_size, sizeof(*s->l_phase));
    s->r_phase = av_calloc(s->rdft_size, sizeof(*s->r_phase));
    s->c_phase = av_calloc(s->rdft_size, sizeof(*s->c_phase));
    s->mag_total = av_calloc(s->rdft_size, sizeof(*s->mag_total));
    s->lfe_mag = av_calloc(s->rdft_size, sizeof(*s->lfe_mag));
    if (!s->x_pos || !s->y_pos || !s->l_phase || !s->r_phase ||
        !s->c_phase || !s->mag_total || !s->lfe_mag)
        return AVERROR(ENOMEM);

    return 0;
}

static float sqrf(float x)
{
    return x * x;
}

static float r_distance(float a)
{
    return fminf(sqrtf(1.f + sqrf(tanf(a))), sqrtf(1.f + sqrf(1.f / tanf(a))));
}

#define MIN_MAG_SUM 0.00000001f

static void angle_transform(float *x, float *y, float angle)
{
    float reference, r, a;

    if (angle == 90.f)
        return;

    reference = angle * M_PI / 180.f;
    r = hypotf(*x, *y);
    a = atan2f(*x, *y);

    r /= r_distance(a);

    if (fabsf(a) <= M_PI_4)
        a *= reference / M_PI_2;
    else
        a = M_PI + (-2.f * M_PI + reference) * (M_PI - fabsf(a)) * FFDIFFSIGN(a, 0.f) / (3.f * M_PI_2);

    r *= r_distance(a);

    *x = av_clipf(sinf(a) * r, -1.f, 1.f);
    *y = av_clipf(cosf(a) * r, -1.f, 1.f);
}

static void focus_transform(float *x, float *y, float focus)
{
    float a, r, ra;

    if (focus == 0.f)
        return;

    a = atan2f(*x, *y);
    ra = r_distance(a);
    r = av_clipf(hypotf(*x, *y) / ra, 0.f, 1.f);
    r = focus > 0.f ? 1.f - powf(1.f - r, 1.f + focus * 20.f) : powf(r, 1.f - focus * 20.f);
    r *= ra;
    *x = av_clipf(sinf(a) * r, -1.f, 1.f);
    *y = av_clipf(cosf(a) * r, -1.f, 1.f);
}

static void stereo_position(float a, float p, float *x, float *y)
{
    av_assert2(a >= -1.f && a <= 1.f);
    av_assert2(p >= 0.f && p <= M_PI);
    *x = av_clipf(a+a*fmaxf(0.f, p*p-M_PI_2), -1.f, 1.f);
    *y = av_clipf(cosf(a*M_PI_2+M_PI)*cosf(M_PI_2-p/M_PI)*M_LN10+1.f, -1.f, 1.f);
}

static inline void get_lfe(int output_lfe, int n, float lowcut, float highcut,
                           float *lfe_mag, float *mag_total, int lfe_mode)
{
    if (output_lfe && n < highcut) {
        *lfe_mag    = n < lowcut ? 1.f : .5f*(1.f+cosf(M_PI*(lowcut-n)/(lowcut-highcut)));
        *lfe_mag   *= *mag_total;
        if (lfe_mode)
            *mag_total -= *lfe_mag;
    } else {
        *lfe_mag = 0.f;
    }
}

#define TRANSFORM                         \
        dst[2 * n    ] = mag * cosf(ph);  \
        dst[2 * n + 1] = mag * sinf(ph);

static int stereo_upmix(AVFilterContext *ctx, int ch)
{
    AudioSurroundContext *s = ctx->priv;
    float *dst = (float *)s->output->extended_data[ch];
    const float *mag_total = s->mag_total;
    const int rdft_size = s->rdft_size;
    const float *c_phase = s->c_phase;
    const float *l_phase = s->l_phase;
    const float *r_phase = s->r_phase;
    const float *lfe_mag = s->lfe_mag;
    const float *x = s->x_pos;
    const float *y = s->y_pos;
    const int chan = av_channel_layout_channel_from_index(&s->out_ch_layout, ch);
    float f_x, f_y;

    switch (chan) {
    case AV_CHAN_FRONT_CENTER:
        f_x = s->fc_x;
        f_y = s->fc_y;
        for (int n = 0; n < rdft_size; n++) {
            float mag = powf(1.f - fabsf(x[n]), f_x) * powf((y[n] + 1.f) * .5f, f_y) * mag_total[n];
            float ph = c_phase[n];

            TRANSFORM
        }
        break;
    case AV_CHAN_FRONT_LEFT:
        f_x = s->fl_x;
        f_y = s->fl_y;
        for (int n = 0; n < rdft_size; n++) {
            float mag = powf(.5f * ( x[n] + 1.f), f_x) * powf((y[n] + 1.f) * .5f, f_y) * mag_total[n];
            float ph = l_phase[n];

            TRANSFORM
        }
        break;
    case AV_CHAN_FRONT_RIGHT:
        f_x = s->fr_x;
        f_y = s->fr_y;
        for (int n = 0; n < rdft_size; n++) {
            float mag = powf(.5f * (-x[n] + 1.f), f_x) * powf((y[n] + 1.f) * .5f, f_y) * mag_total[n];
            float ph = r_phase[n];

            TRANSFORM
        }
        break;
    case AV_CHAN_LOW_FREQUENCY:
        for (int n = 0; n < rdft_size; n++) {
            float mag = lfe_mag[n];
            float ph = c_phase[n];

            TRANSFORM
        }
        break;
    case AV_CHAN_BACK_CENTER:
        f_x = s->bc_x;
        f_y = s->bc_y;
        for (int n = 0; n < rdft_size; n++) {
            float mag = powf(1.f - fabsf(x[n]), f_x) * powf((1.f - y[n]) * .5f, f_y) * mag_total[n];
            float ph = c_phase[n];

            TRANSFORM
        }
        break;
    case AV_CHAN_BACK_LEFT:
        f_x = s->bl_x;
        f_y = s->bl_y;
        for (int n = 0; n < rdft_size; n++) {
            float mag = powf(.5f * ( x[n] + 1.f), f_x) * powf(1.f - ((y[n] + 1.f) * .5f), f_y) * mag_total[n];
            float ph = l_phase[n];

            TRANSFORM
        }
        break;
    case AV_CHAN_BACK_RIGHT:
        f_x = s->br_x;
        f_y = s->br_y;
        for (int n = 0; n < rdft_size; n++) {
            float mag = powf(.5f * (-x[n] + 1.f), f_x) * powf(1.f - ((y[n] + 1.f) * .5f), f_y) * mag_total[n];
            float ph = r_phase[n];

            TRANSFORM
        }
        break;
    case AV_CHAN_SIDE_LEFT:
        f_x = s->sl_x;
        f_y = s->sl_y;
        for (int n = 0; n < rdft_size; n++) {
            float mag = powf(.5f * ( x[n] + 1.f), f_x) * powf(1.f - fabsf(y[n]), f_y) * mag_total[n];
            float ph = l_phase[n];

            TRANSFORM
        }
        break;
    case AV_CHAN_SIDE_RIGHT:
        f_x = s->sr_x;
        f_y = s->sr_y;
        for (int n = 0; n < rdft_size; n++) {
            float mag = powf(.5f * (-x[n] + 1.f), f_x) * powf(1.f - fabsf(y[n]), f_y) * mag_total[n];
            float ph = r_phase[n];

            TRANSFORM
        }
        break;
    default:
        break;
    }

    return 0;
}

static void upmix_3_1_surround(AVFilterContext *ctx,
                               float l_phase,
                               float r_phase,
                               float c_phase,
                               float c_mag,
                               float mag_total,
                               float x, float y,
                               int n)
{
    AudioSurroundContext *s = ctx->priv;
    float lfe_mag, l_mag, r_mag, *dstc, *dstl, *dstr, *dstlfe;

    dstl = (float *)s->output->extended_data[0];
    dstr = (float *)s->output->extended_data[1];
    dstc = (float *)s->output->extended_data[2];
    dstlfe = (float *)s->output->extended_data[3];

    get_lfe(s->output_lfe, n, s->lowcut, s->highcut, &lfe_mag, &c_mag, s->lfe_mode);

    l_mag = powf(.5f * ( x + 1.f), s->fl_x) * powf((y + 1.f) * .5f, s->fl_y) * mag_total;
    r_mag = powf(.5f * (-x + 1.f), s->fr_x) * powf((y + 1.f) * .5f, s->fr_y) * mag_total;

    dstl[2 * n    ] = l_mag * cosf(l_phase);
    dstl[2 * n + 1] = l_mag * sinf(l_phase);

    dstr[2 * n    ] = r_mag * cosf(r_phase);
    dstr[2 * n + 1] = r_mag * sinf(r_phase);

    dstc[2 * n    ] = c_mag * cosf(c_phase);
    dstc[2 * n + 1] = c_mag * sinf(c_phase);

    dstlfe[2 * n    ] = lfe_mag * cosf(c_phase);
    dstlfe[2 * n + 1] = lfe_mag * sinf(c_phase);
}

static void upmix_5_1_back_surround(AVFilterContext *ctx,
                                    float l_phase,
                                    float r_phase,
                                    float c_phase,
                                    float c_mag,
                                    float mag_total,
                                    float x, float y,
                                    int n)
{
    AudioSurroundContext *s = ctx->priv;
    float lfe_mag, l_mag, r_mag, *dstc, *dstl, *dstr, *dstlfe;
    float ls_mag, rs_mag, *dstls, *dstrs;

    dstl = (float *)s->output->extended_data[0];
    dstr = (float *)s->output->extended_data[1];
    dstc = (float *)s->output->extended_data[2];
    dstlfe = (float *)s->output->extended_data[3];
    dstls = (float *)s->output->extended_data[4];
    dstrs = (float *)s->output->extended_data[5];

    get_lfe(s->output_lfe, n, s->lowcut, s->highcut, &lfe_mag, &c_mag, s->lfe_mode);

    l_mag = powf(.5f * ( x + 1.f),  s->fl_x) * powf((y + 1.f) * .5f, s->fl_y) * mag_total;
    r_mag = powf(.5f * (-x + 1.f),  s->fr_x) * powf((y + 1.f) * .5f, s->fr_y) * mag_total;
    ls_mag = powf(.5f * ( x + 1.f), s->bl_x) * powf(1.f - ((y + 1.f) * .5f), s->bl_y) * mag_total;
    rs_mag = powf(.5f * (-x + 1.f), s->br_x) * powf(1.f - ((y + 1.f) * .5f), s->br_y) * mag_total;

    dstl[2 * n    ] = l_mag * cosf(l_phase);
    dstl[2 * n + 1] = l_mag * sinf(l_phase);

    dstr[2 * n    ] = r_mag * cosf(r_phase);
    dstr[2 * n + 1] = r_mag * sinf(r_phase);

    dstc[2 * n    ] = c_mag * cosf(c_phase);
    dstc[2 * n + 1] = c_mag * sinf(c_phase);

    dstlfe[2 * n    ] = lfe_mag * cosf(c_phase);
    dstlfe[2 * n + 1] = lfe_mag * sinf(c_phase);

    dstls[2 * n    ] = ls_mag * cosf(l_phase);
    dstls[2 * n + 1] = ls_mag * sinf(l_phase);

    dstrs[2 * n    ] = rs_mag * cosf(r_phase);
    dstrs[2 * n + 1] = rs_mag * sinf(r_phase);
}

static void upmix_5_1_back_2_1(AVFilterContext *ctx,
                               float l_phase,
                               float r_phase,
                               float c_phase,
                               float mag_total,
                               float lfe_re,
                               float lfe_im,
                               float x, float y,
                               int n)
{
    AudioSurroundContext *s = ctx->priv;
    float c_mag, l_mag, r_mag, *dstc, *dstl, *dstr, *dstlfe;
    float ls_mag, rs_mag, *dstls, *dstrs;

    dstl = (float *)s->output->extended_data[0];
    dstr = (float *)s->output->extended_data[1];
    dstc = (float *)s->output->extended_data[2];
    dstlfe = (float *)s->output->extended_data[3];
    dstls = (float *)s->output->extended_data[4];
    dstrs = (float *)s->output->extended_data[5];

    c_mag  = powf(1.f - fabsf(x),   s->fc_x) * powf((y + 1.f) * .5f, s->fc_y) * mag_total;
    l_mag  = powf(.5f * ( x + 1.f), s->fl_x) * powf((y + 1.f) * .5f, s->fl_y) * mag_total;
    r_mag  = powf(.5f * (-x + 1.f), s->fr_x) * powf((y + 1.f) * .5f, s->fr_y) * mag_total;
    ls_mag = powf(.5f * ( x + 1.f), s->bl_x) * powf(1.f - ((y + 1.f) * .5f), s->bl_y) * mag_total;
    rs_mag = powf(.5f * (-x + 1.f), s->br_x) * powf(1.f - ((y + 1.f) * .5f), s->br_y) * mag_total;

    dstl[2 * n    ] = l_mag * cosf(l_phase);
    dstl[2 * n + 1] = l_mag * sinf(l_phase);

    dstr[2 * n    ] = r_mag * cosf(r_phase);
    dstr[2 * n + 1] = r_mag * sinf(r_phase);

    dstc[2 * n    ] = c_mag * cosf(c_phase);
    dstc[2 * n + 1] = c_mag * sinf(c_phase);

    dstlfe[2 * n    ] = lfe_re;
    dstlfe[2 * n + 1] = lfe_im;

    dstls[2 * n    ] = ls_mag * cosf(l_phase);
    dstls[2 * n + 1] = ls_mag * sinf(l_phase);

    dstrs[2 * n    ] = rs_mag * cosf(r_phase);
    dstrs[2 * n + 1] = rs_mag * sinf(r_phase);
}

static void upmix_7_1_5_0_side(AVFilterContext *ctx,
                               float c_re, float c_im,
                               float mag_totall, float mag_totalr,
                               float fl_phase, float fr_phase,
                               float bl_phase, float br_phase,
                               float sl_phase, float sr_phase,
                               float xl, float yl,
                               float xr, float yr,
                               int n)
{
    float fl_mag, fr_mag, ls_mag, rs_mag, lb_mag, rb_mag;
    float *dstc, *dstl, *dstr, *dstls, *dstrs, *dstlb, *dstrb, *dstlfe;
    float lfe_mag, c_phase, mag_total = (mag_totall + mag_totalr) * 0.5;
    AudioSurroundContext *s = ctx->priv;

    dstl  = (float *)s->output->extended_data[0];
    dstr  = (float *)s->output->extended_data[1];
    dstc  = (float *)s->output->extended_data[2];
    dstlfe = (float *)s->output->extended_data[3];
    dstlb = (float *)s->output->extended_data[4];
    dstrb = (float *)s->output->extended_data[5];
    dstls = (float *)s->output->extended_data[6];
    dstrs = (float *)s->output->extended_data[7];

    c_phase = atan2f(c_im, c_re);

    get_lfe(s->output_lfe, n, s->lowcut, s->highcut, &lfe_mag, &mag_total, s->lfe_mode);

    fl_mag = powf(.5f * (xl + 1.f), s->fl_x) * powf((yl + 1.f) * .5f, s->fl_y) * mag_totall;
    fr_mag = powf(.5f * (xr + 1.f), s->fr_x) * powf((yr + 1.f) * .5f, s->fr_y) * mag_totalr;
    lb_mag = powf(.5f * (-xl + 1.f), s->bl_x) * powf((yl + 1.f) * .5f, s->bl_y) * mag_totall;
    rb_mag = powf(.5f * (-xr + 1.f), s->br_x) * powf((yr + 1.f) * .5f, s->br_y) * mag_totalr;
    ls_mag = powf(1.f - fabsf(xl), s->sl_x) * powf((yl + 1.f) * .5f, s->sl_y) * mag_totall;
    rs_mag = powf(1.f - fabsf(xr), s->sr_x) * powf((yr + 1.f) * .5f, s->sr_y) * mag_totalr;

    dstl[2 * n    ] = fl_mag * cosf(fl_phase);
    dstl[2 * n + 1] = fl_mag * sinf(fl_phase);

    dstr[2 * n    ] = fr_mag * cosf(fr_phase);
    dstr[2 * n + 1] = fr_mag * sinf(fr_phase);

    dstc[2 * n    ] = c_re;
    dstc[2 * n + 1] = c_im;

    dstlfe[2 * n    ] = lfe_mag * cosf(c_phase);
    dstlfe[2 * n + 1] = lfe_mag * sinf(c_phase);

    dstlb[2 * n    ] = lb_mag * cosf(bl_phase);
    dstlb[2 * n + 1] = lb_mag * sinf(bl_phase);

    dstrb[2 * n    ] = rb_mag * cosf(br_phase);
    dstrb[2 * n + 1] = rb_mag * sinf(br_phase);

    dstls[2 * n    ] = ls_mag * cosf(sl_phase);
    dstls[2 * n + 1] = ls_mag * sinf(sl_phase);

    dstrs[2 * n    ] = rs_mag * cosf(sr_phase);
    dstrs[2 * n + 1] = rs_mag * sinf(sr_phase);
}

static void upmix_7_1_5_1(AVFilterContext *ctx,
                          float c_re, float c_im,
                          float lfe_re, float lfe_im,
                          float mag_totall, float mag_totalr,
                          float fl_phase, float fr_phase,
                          float bl_phase, float br_phase,
                          float sl_phase, float sr_phase,
                          float xl, float yl,
                          float xr, float yr,
                          int n)
{
    float fl_mag, fr_mag, ls_mag, rs_mag, lb_mag, rb_mag;
    float *dstc, *dstl, *dstr, *dstls, *dstrs, *dstlb, *dstrb, *dstlfe;
    AudioSurroundContext *s = ctx->priv;

    dstl  = (float *)s->output->extended_data[0];
    dstr  = (float *)s->output->extended_data[1];
    dstc  = (float *)s->output->extended_data[2];
    dstlfe = (float *)s->output->extended_data[3];
    dstlb = (float *)s->output->extended_data[4];
    dstrb = (float *)s->output->extended_data[5];
    dstls = (float *)s->output->extended_data[6];
    dstrs = (float *)s->output->extended_data[7];

    fl_mag = powf(.5f * (xl + 1.f), s->fl_x) * powf((yl + 1.f) * .5f, s->fl_y) * mag_totall;
    fr_mag = powf(.5f * (xr + 1.f), s->fr_x) * powf((yr + 1.f) * .5f, s->fr_y) * mag_totalr;
    lb_mag = powf(.5f * (-xl + 1.f), s->bl_x) * powf((yl + 1.f) * .5f, s->bl_y) * mag_totall;
    rb_mag = powf(.5f * (-xr + 1.f), s->br_x) * powf((yr + 1.f) * .5f, s->br_y) * mag_totalr;
    ls_mag = powf(1.f - fabsf(xl), s->sl_x) * powf((yl + 1.f) * .5f, s->sl_y) * mag_totall;
    rs_mag = powf(1.f - fabsf(xr), s->sr_x) * powf((yr + 1.f) * .5f, s->sr_y) * mag_totalr;

    dstl[2 * n    ] = fl_mag * cosf(fl_phase);
    dstl[2 * n + 1] = fl_mag * sinf(fl_phase);

    dstr[2 * n    ] = fr_mag * cosf(fr_phase);
    dstr[2 * n + 1] = fr_mag * sinf(fr_phase);

    dstc[2 * n    ] = c_re;
    dstc[2 * n + 1] = c_im;

    dstlfe[2 * n    ] = lfe_re;
    dstlfe[2 * n + 1] = lfe_im;

    dstlb[2 * n    ] = lb_mag * cosf(bl_phase);
    dstlb[2 * n + 1] = lb_mag * sinf(bl_phase);

    dstrb[2 * n    ] = rb_mag * cosf(br_phase);
    dstrb[2 * n + 1] = rb_mag * sinf(br_phase);

    dstls[2 * n    ] = ls_mag * cosf(sl_phase);
    dstls[2 * n + 1] = ls_mag * sinf(sl_phase);

    dstrs[2 * n    ] = rs_mag * cosf(sr_phase);
    dstrs[2 * n + 1] = rs_mag * sinf(sr_phase);
}

static void filter_stereo(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;
    const float *srcl = (const float *)s->input->extended_data[0];
    const float *srcr = (const float *)s->input->extended_data[1];
    const int output_lfe = s->output_lfe && s->have_lfe;
    const int lfe_mode = s->lfe_mode;
    const float highcut = s->highcut;
    const float lowcut = s->lowcut;
    const float angle = s->angle;
    const float focus = s->focus;
    float *magtotal = s->mag_total;
    float *lfemag = s->lfe_mag;
    float *lphase = s->l_phase;
    float *rphase = s->r_phase;
    float *cphase = s->c_phase;
    float *xpos = s->x_pos;
    float *ypos = s->y_pos;

    for (int n = 0; n < s->rdft_size; n++) {
        float l_re = srcl[2 * n], r_re = srcr[2 * n];
        float l_im = srcl[2 * n + 1], r_im = srcr[2 * n + 1];
        float c_phase = atan2f(l_im + r_im, l_re + r_re);
        float l_mag = hypotf(l_re, l_im);
        float r_mag = hypotf(r_re, r_im);
        float mag_total = hypotf(l_mag, r_mag);
        float l_phase = atan2f(l_im, l_re);
        float r_phase = atan2f(r_im, r_re);
        float phase_dif = fabsf(l_phase - r_phase);
        float mag_sum = l_mag + r_mag;
        float mag_dif, x, y;

        mag_sum = mag_sum < MIN_MAG_SUM ? 1.f : mag_sum;
        mag_dif = (l_mag - r_mag) / mag_sum;
        if (phase_dif > M_PI)
            phase_dif = 2.f * M_PI - phase_dif;

        stereo_position(mag_dif, phase_dif, &x, &y);
        angle_transform(&x, &y, angle);
        focus_transform(&x, &y, focus);
        get_lfe(output_lfe, n, lowcut, highcut, &lfemag[n], &mag_total, lfe_mode);

        xpos[n]   = x;
        ypos[n]   = y;
        lphase[n] = l_phase;
        rphase[n] = r_phase;
        cphase[n] = c_phase;
        magtotal[n] = mag_total;
    }
}

static void filter_surround(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;
    float *srcl, *srcr, *srcc;
    int n;

    srcl = (float *)s->input->extended_data[0];
    srcr = (float *)s->input->extended_data[1];
    srcc = (float *)s->input->extended_data[2];

    for (n = 0; n < s->rdft_size; n++) {
        float l_re = srcl[2 * n], r_re = srcr[2 * n];
        float l_im = srcl[2 * n + 1], r_im = srcr[2 * n + 1];
        float c_re = srcc[2 * n], c_im = srcc[2 * n + 1];
        float c_mag = hypotf(c_re, c_im);
        float c_phase = atan2f(c_im, c_re);
        float l_mag = hypotf(l_re, l_im);
        float r_mag = hypotf(r_re, r_im);
        float mag_total = hypotf(l_mag, r_mag);
        float l_phase = atan2f(l_im, l_re);
        float r_phase = atan2f(r_im, r_re);
        float phase_dif = fabsf(l_phase - r_phase);
        float mag_sum = l_mag + r_mag;
        float mag_dif, x, y;

        mag_sum = mag_sum < MIN_MAG_SUM ? 1.f : mag_sum;
        mag_dif = (l_mag - r_mag) / mag_sum;
        if (phase_dif > M_PI)
            phase_dif = 2.f * M_PI - phase_dif;

        stereo_position(mag_dif, phase_dif, &x, &y);
        angle_transform(&x, &y, s->angle);

        s->upmix_3_0(ctx, l_phase, r_phase, c_phase, c_mag, mag_total, x, y, n);
    }
}

static void filter_2_1(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;
    float *srcl, *srcr, *srclfe;
    int n;

    srcl = (float *)s->input->extended_data[0];
    srcr = (float *)s->input->extended_data[1];
    srclfe = (float *)s->input->extended_data[2];

    for (n = 0; n < s->rdft_size; n++) {
        float l_re = srcl[2 * n], r_re = srcr[2 * n];
        float l_im = srcl[2 * n + 1], r_im = srcr[2 * n + 1];
        float lfe_re = srclfe[2 * n], lfe_im = srclfe[2 * n + 1];
        float c_phase = atan2f(l_im + r_im, l_re + r_re);
        float l_mag = hypotf(l_re, l_im);
        float r_mag = hypotf(r_re, r_im);
        float mag_total = hypotf(l_mag, r_mag);
        float l_phase = atan2f(l_im, l_re);
        float r_phase = atan2f(r_im, r_re);
        float phase_dif = fabsf(l_phase - r_phase);
        float mag_sum = l_mag + r_mag;
        float mag_dif, x, y;

        mag_sum = mag_sum < MIN_MAG_SUM ? 1.f : mag_sum;
        mag_dif = (l_mag - r_mag) / mag_sum;
        if (phase_dif > M_PI)
            phase_dif = 2.f * M_PI - phase_dif;

        stereo_position(mag_dif, phase_dif, &x, &y);
        angle_transform(&x, &y, s->angle);

        s->upmix_2_1(ctx, l_phase, r_phase, c_phase, mag_total, lfe_re, lfe_im, x, y, n);
    }
}

static void filter_5_0_side(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;
    float *srcl, *srcr, *srcc, *srcsl, *srcsr;
    int n;

    srcl = (float *)s->input->extended_data[0];
    srcr = (float *)s->input->extended_data[1];
    srcc = (float *)s->input->extended_data[2];
    srcsl = (float *)s->input->extended_data[3];
    srcsr = (float *)s->input->extended_data[4];

    for (n = 0; n < s->rdft_size; n++) {
        float fl_re = srcl[2 * n], fr_re = srcr[2 * n];
        float fl_im = srcl[2 * n + 1], fr_im = srcr[2 * n + 1];
        float c_re = srcc[2 * n], c_im = srcc[2 * n + 1];
        float sl_re = srcsl[2 * n], sl_im = srcsl[2 * n + 1];
        float sr_re = srcsr[2 * n], sr_im = srcsr[2 * n + 1];
        float fl_mag = hypotf(fl_re, fl_im);
        float fr_mag = hypotf(fr_re, fr_im);
        float fl_phase = atan2f(fl_im, fl_re);
        float fr_phase = atan2f(fr_im, fr_re);
        float sl_mag = hypotf(sl_re, sl_im);
        float sr_mag = hypotf(sr_re, sr_im);
        float sl_phase = atan2f(sl_im, sl_re);
        float sr_phase = atan2f(sr_im, sr_re);
        float phase_difl = fabsf(fl_phase - sl_phase);
        float phase_difr = fabsf(fr_phase - sr_phase);
        float magl_sum = fl_mag + sl_mag;
        float magr_sum = fr_mag + sr_mag;
        float mag_difl = magl_sum < MIN_MAG_SUM ? FFDIFFSIGN(fl_mag, sl_mag) : (fl_mag - sl_mag) / magl_sum;
        float mag_difr = magr_sum < MIN_MAG_SUM ? FFDIFFSIGN(fr_mag, sr_mag) : (fr_mag - sr_mag) / magr_sum;
        float mag_totall = hypotf(fl_mag, sl_mag);
        float mag_totalr = hypotf(fr_mag, sr_mag);
        float bl_phase = atan2f(fl_im + sl_im, fl_re + sl_re);
        float br_phase = atan2f(fr_im + sr_im, fr_re + sr_re);
        float xl, yl;
        float xr, yr;

        if (phase_difl > M_PI)
            phase_difl = 2.f * M_PI - phase_difl;

        if (phase_difr > M_PI)
            phase_difr = 2.f * M_PI - phase_difr;

        stereo_position(mag_difl, phase_difl, &xl, &yl);
        stereo_position(mag_difr, phase_difr, &xr, &yr);

        s->upmix_5_0(ctx, c_re, c_im,
                     mag_totall, mag_totalr,
                     fl_phase, fr_phase,
                     bl_phase, br_phase,
                     sl_phase, sr_phase,
                     xl, yl, xr, yr, n);
    }
}

static void filter_5_1_side(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;
    float *srcl, *srcr, *srcc, *srclfe, *srcsl, *srcsr;
    int n;

    srcl = (float *)s->input->extended_data[0];
    srcr = (float *)s->input->extended_data[1];
    srcc = (float *)s->input->extended_data[2];
    srclfe = (float *)s->input->extended_data[3];
    srcsl = (float *)s->input->extended_data[4];
    srcsr = (float *)s->input->extended_data[5];

    for (n = 0; n < s->rdft_size; n++) {
        float fl_re = srcl[2 * n], fr_re = srcr[2 * n];
        float fl_im = srcl[2 * n + 1], fr_im = srcr[2 * n + 1];
        float c_re = srcc[2 * n], c_im = srcc[2 * n + 1];
        float lfe_re = srclfe[2 * n], lfe_im = srclfe[2 * n + 1];
        float sl_re = srcsl[2 * n], sl_im = srcsl[2 * n + 1];
        float sr_re = srcsr[2 * n], sr_im = srcsr[2 * n + 1];
        float fl_mag = hypotf(fl_re, fl_im);
        float fr_mag = hypotf(fr_re, fr_im);
        float fl_phase = atan2f(fl_im, fl_re);
        float fr_phase = atan2f(fr_im, fr_re);
        float sl_mag = hypotf(sl_re, sl_im);
        float sr_mag = hypotf(sr_re, sr_im);
        float sl_phase = atan2f(sl_im, sl_re);
        float sr_phase = atan2f(sr_im, sr_re);
        float phase_difl = fabsf(fl_phase - sl_phase);
        float phase_difr = fabsf(fr_phase - sr_phase);
        float magl_sum = fl_mag + sl_mag;
        float magr_sum = fr_mag + sr_mag;
        float mag_difl = magl_sum < MIN_MAG_SUM ? FFDIFFSIGN(fl_mag, sl_mag) : (fl_mag - sl_mag) / magl_sum;
        float mag_difr = magr_sum < MIN_MAG_SUM ? FFDIFFSIGN(fr_mag, sr_mag) : (fr_mag - sr_mag) / magr_sum;
        float mag_totall = hypotf(fl_mag, sl_mag);
        float mag_totalr = hypotf(fr_mag, sr_mag);
        float bl_phase = atan2f(fl_im + sl_im, fl_re + sl_re);
        float br_phase = atan2f(fr_im + sr_im, fr_re + sr_re);
        float xl, yl;
        float xr, yr;

        if (phase_difl > M_PI)
            phase_difl = 2.f * M_PI - phase_difl;

        if (phase_difr > M_PI)
            phase_difr = 2.f * M_PI - phase_difr;

        stereo_position(mag_difl, phase_difl, &xl, &yl);
        stereo_position(mag_difr, phase_difr, &xr, &yr);

        s->upmix_5_1(ctx, c_re, c_im, lfe_re, lfe_im,
                     mag_totall, mag_totalr,
                     fl_phase, fr_phase,
                     bl_phase, br_phase,
                     sl_phase, sr_phase,
                     xl, yl, xr, yr, n);
    }
}

static void filter_5_1_back(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;
    float *srcl, *srcr, *srcc, *srclfe, *srcbl, *srcbr;
    int n;

    srcl = (float *)s->input->extended_data[0];
    srcr = (float *)s->input->extended_data[1];
    srcc = (float *)s->input->extended_data[2];
    srclfe = (float *)s->input->extended_data[3];
    srcbl = (float *)s->input->extended_data[4];
    srcbr = (float *)s->input->extended_data[5];

    for (n = 0; n < s->rdft_size; n++) {
        float fl_re = srcl[2 * n], fr_re = srcr[2 * n];
        float fl_im = srcl[2 * n + 1], fr_im = srcr[2 * n + 1];
        float c_re = srcc[2 * n], c_im = srcc[2 * n + 1];
        float lfe_re = srclfe[2 * n], lfe_im = srclfe[2 * n + 1];
        float bl_re = srcbl[2 * n], bl_im = srcbl[2 * n + 1];
        float br_re = srcbr[2 * n], br_im = srcbr[2 * n + 1];
        float fl_mag = hypotf(fl_re, fl_im);
        float fr_mag = hypotf(fr_re, fr_im);
        float fl_phase = atan2f(fl_im, fl_re);
        float fr_phase = atan2f(fr_im, fr_re);
        float bl_mag = hypotf(bl_re, bl_im);
        float br_mag = hypotf(br_re, br_im);
        float bl_phase = atan2f(bl_im, bl_re);
        float br_phase = atan2f(br_im, br_re);
        float phase_difl = fabsf(fl_phase - bl_phase);
        float phase_difr = fabsf(fr_phase - br_phase);
        float magl_sum = fl_mag + bl_mag;
        float magr_sum = fr_mag + br_mag;
        float mag_difl = magl_sum < MIN_MAG_SUM ? FFDIFFSIGN(fl_mag, bl_mag) : (fl_mag - bl_mag) / magl_sum;
        float mag_difr = magr_sum < MIN_MAG_SUM ? FFDIFFSIGN(fr_mag, br_mag) : (fr_mag - br_mag) / magr_sum;
        float mag_totall = hypotf(fl_mag, bl_mag);
        float mag_totalr = hypotf(fr_mag, br_mag);
        float sl_phase = atan2f(fl_im + bl_im, fl_re + bl_re);
        float sr_phase = atan2f(fr_im + br_im, fr_re + br_re);
        float xl, yl;
        float xr, yr;

        if (phase_difl > M_PI)
            phase_difl = 2.f * M_PI - phase_difl;

        if (phase_difr > M_PI)
            phase_difr = 2.f * M_PI - phase_difr;

        stereo_position(mag_difl, phase_difl, &xl, &yl);
        stereo_position(mag_difr, phase_difr, &xr, &yr);

        s->upmix_5_1(ctx, c_re, c_im, lfe_re, lfe_im,
                     mag_totall, mag_totalr,
                     fl_phase, fr_phase,
                     bl_phase, br_phase,
                     sl_phase, sr_phase,
                     xl, yl, xr, yr, n);
    }
}

static void allchannels_spread(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;

    if (s->all_x >= 0.f)
        s->fc_x = s->fl_x = s->fr_x = s->bc_x = s->sl_x = s->sr_x = s->bl_x = s->br_x = s->all_x;
    s->all_x = -1.f;
    if (s->all_y >= 0.f)
        s->fc_y = s->fl_y = s->fr_y = s->bc_y = s->sl_y = s->sr_y = s->bl_y = s->br_y = s->all_y;
    s->all_y = -1.f;
}

static av_cold int init(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;
    float overlap;
    int64_t in_channel_layout, out_channel_layout;
    int i, ret;

    if ((ret = av_channel_layout_from_string(&s->out_ch_layout, s->out_channel_layout_str)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error parsing output channel layout '%s'.\n",
               s->out_channel_layout_str);
        return ret;
    }

    if ((ret = av_channel_layout_from_string(&s->in_ch_layout, s->in_channel_layout_str)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error parsing input channel layout '%s'.\n",
               s->in_channel_layout_str);
        return AVERROR(EINVAL);
    }

    if (s->lowcutf >= s->highcutf) {
        av_log(ctx, AV_LOG_ERROR, "Low cut-off '%d' should be less than high cut-off '%d'.\n",
               s->lowcutf, s->highcutf);
        return AVERROR(EINVAL);
    }

    in_channel_layout  = s->in_ch_layout.order == AV_CHANNEL_ORDER_NATIVE ?
                         s->in_ch_layout.u.mask : 0;
    out_channel_layout = s->out_ch_layout.order == AV_CHANNEL_ORDER_NATIVE ?
                         s->out_ch_layout.u.mask : 0;

    s->have_lfe = av_channel_layout_index_from_channel(&s->out_ch_layout,
                                                       AV_CHAN_LOW_FREQUENCY) >= 0;

    switch (in_channel_layout) {
    case AV_CH_LAYOUT_STEREO:
        s->filter = filter_stereo;
        s->upmix = stereo_upmix;
        switch (out_channel_layout) {
        case AV_CH_LAYOUT_MONO:
        case AV_CH_LAYOUT_STEREO:
        case AV_CH_LAYOUT_SURROUND:
        case AV_CH_LAYOUT_3POINT1:
        case AV_CH_LAYOUT_4POINT0:
        case AV_CH_LAYOUT_4POINT1:
        case AV_CH_LAYOUT_5POINT0:
        case AV_CH_LAYOUT_5POINT1:
        case AV_CH_LAYOUT_5POINT0_BACK:
        case AV_CH_LAYOUT_5POINT1_BACK:
        case AV_CH_LAYOUT_6POINT0:
        case AV_CH_LAYOUT_6POINT1:
        case AV_CH_LAYOUT_7POINT0:
        case AV_CH_LAYOUT_7POINT1:
        case AV_CH_LAYOUT_OCTAGONAL:
            break;
        default:
            goto fail;
        }
        break;
    case AV_CH_LAYOUT_2POINT1:
        s->filter = filter_2_1;
        switch (out_channel_layout) {
        case AV_CH_LAYOUT_5POINT1_BACK:
            s->upmix_2_1 = upmix_5_1_back_2_1;
            break;
        default:
            goto fail;
        }
        break;
    case AV_CH_LAYOUT_SURROUND:
        s->filter = filter_surround;
        switch (out_channel_layout) {
        case AV_CH_LAYOUT_3POINT1:
            s->upmix_3_0 = upmix_3_1_surround;
            break;
        case AV_CH_LAYOUT_5POINT1_BACK:
            s->upmix_3_0 = upmix_5_1_back_surround;
            break;
        default:
            goto fail;
        }
        break;
    case AV_CH_LAYOUT_5POINT0:
        s->filter = filter_5_0_side;
        switch (out_channel_layout) {
        case AV_CH_LAYOUT_7POINT1:
            s->upmix_5_0 = upmix_7_1_5_0_side;
            break;
        default:
            goto fail;
        }
        break;
    case AV_CH_LAYOUT_5POINT1:
        s->filter = filter_5_1_side;
        switch (out_channel_layout) {
        case AV_CH_LAYOUT_7POINT1:
            s->upmix_5_1 = upmix_7_1_5_1;
            break;
        default:
            goto fail;
        }
        break;
    case AV_CH_LAYOUT_5POINT1_BACK:
        s->filter = filter_5_1_back;
        switch (out_channel_layout) {
        case AV_CH_LAYOUT_7POINT1:
            s->upmix_5_1 = upmix_7_1_5_1;
            break;
        default:
            goto fail;
        }
        break;
    default:
fail:
        av_log(ctx, AV_LOG_ERROR, "Unsupported upmix: '%s' -> '%s'.\n",
               s->in_channel_layout_str, s->out_channel_layout_str);
        return AVERROR(EINVAL);
    }

    s->window_func_lut = av_calloc(s->win_size, sizeof(*s->window_func_lut));
    if (!s->window_func_lut)
        return AVERROR(ENOMEM);

    generate_window_func(s->window_func_lut, s->win_size, s->win_func, &overlap);
    if (s->overlap == 1)
        s->overlap = overlap;

    for (i = 0; i < s->win_size; i++)
        s->window_func_lut[i] = sqrtf(s->window_func_lut[i] / s->win_size);
    s->hop_size = FFMAX(1, s->win_size * (1. - s->overlap));

    allchannels_spread(ctx);

    return 0;
}

static int fft_channel(AVFilterContext *ctx, AVFrame *in, int ch)
{
    AudioSurroundContext *s = ctx->priv;
    float *src = (float *)s->input_in->extended_data[ch];
    float *win = (float *)s->window->extended_data[ch];
    const int offset = s->win_size - s->hop_size;
    const float level_in = s->input_levels[ch];

    memmove(src, &src[s->hop_size], offset * sizeof(float));
    memcpy(&src[offset], in->extended_data[ch], in->nb_samples * sizeof(float));
    memset(&src[offset + in->nb_samples], 0, (s->hop_size - in->nb_samples) * sizeof(float));

    for (int n = 0; n < s->win_size; n++)
        win[n] = src[n] * s->window_func_lut[n] * level_in;

    s->tx_fn(s->rdft[ch], (float *)s->input->extended_data[ch], win, sizeof(float));

    return 0;
}

static int fft_channels(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    AVFrame *in = arg;
    const int start = (in->ch_layout.nb_channels * jobnr) / nb_jobs;
    const int end = (in->ch_layout.nb_channels * (jobnr+1)) / nb_jobs;

    for (int ch = start; ch < end; ch++)
        fft_channel(ctx, in, ch);

    return 0;
}

static int ifft_channel(AVFilterContext *ctx, AVFrame *out, int ch)
{
    AudioSurroundContext *s = ctx->priv;
    const float level_out = s->output_levels[ch];
    float *dst, *ptr;

    dst = (float *)s->output_out->extended_data[ch];
    ptr = (float *)s->overlap_buffer->extended_data[ch];
    s->itx_fn(s->irdft[ch], dst, (float *)s->output->extended_data[ch], sizeof(float));

    memmove(s->overlap_buffer->extended_data[ch],
            s->overlap_buffer->extended_data[ch] + s->hop_size * sizeof(float),
            s->win_size * sizeof(float));
    memset(s->overlap_buffer->extended_data[ch] + s->win_size * sizeof(float),
           0, s->hop_size * sizeof(float));

    for (int n = 0; n < s->win_size; n++)
        ptr[n] += dst[n] * s->window_func_lut[n] * level_out;

    ptr = (float *)s->overlap_buffer->extended_data[ch];
    dst = (float *)out->extended_data[ch];
    memcpy(dst, ptr, s->hop_size * sizeof(float));

    return 0;
}

static int ifft_channels(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    AudioSurroundContext *s = ctx->priv;
    AVFrame *out = arg;
    const int start = (out->ch_layout.nb_channels * jobnr) / nb_jobs;
    const int end = (out->ch_layout.nb_channels * (jobnr+1)) / nb_jobs;

    for (int ch = start; ch < end; ch++) {
        if (s->upmix)
            s->upmix(ctx, ch);
        ifft_channel(ctx, out, ch);
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AudioSurroundContext *s = ctx->priv;
    AVFrame *out;

    ff_filter_execute(ctx, fft_channels, in, NULL,
                      FFMIN(inlink->ch_layout.nb_channels,
                            ff_filter_get_nb_threads(ctx)));

    s->filter(ctx);

    out = ff_get_audio_buffer(outlink, s->hop_size);
    if (!out)
        return AVERROR(ENOMEM);

    ff_filter_execute(ctx, ifft_channels, out, NULL,
                      FFMIN(outlink->ch_layout.nb_channels,
                            ff_filter_get_nb_threads(ctx)));

    out->pts = in->pts;
    out->nb_samples = in->nb_samples;

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AudioSurroundContext *s = ctx->priv;
    AVFrame *in = NULL;
    int ret = 0, status;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    ret = ff_inlink_consume_samples(inlink, s->hop_size, s->hop_size, &in);
    if (ret < 0)
        return ret;

    if (ret > 0)
        ret = filter_frame(inlink, in);
    if (ret < 0)
        return ret;

    if (ff_inlink_queued_samples(inlink) >= s->hop_size) {
        ff_filter_set_ready(ctx, 10);
        return 0;
    }

    if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        ff_outlink_set_status(outlink, status, pts);
        return 0;
    }

    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AudioSurroundContext *s = ctx->priv;

    av_frame_free(&s->window);
    av_frame_free(&s->input_in);
    av_frame_free(&s->input);
    av_frame_free(&s->output);
    av_frame_free(&s->output_out);
    av_frame_free(&s->overlap_buffer);

    for (int ch = 0; ch < s->nb_in_channels; ch++)
        av_tx_uninit(&s->rdft[ch]);
    for (int ch = 0; ch < s->nb_out_channels; ch++)
        av_tx_uninit(&s->irdft[ch]);
    av_freep(&s->input_levels);
    av_freep(&s->output_levels);
    av_freep(&s->rdft);
    av_freep(&s->irdft);
    av_freep(&s->window_func_lut);

    av_freep(&s->x_pos);
    av_freep(&s->y_pos);
    av_freep(&s->l_phase);
    av_freep(&s->r_phase);
    av_freep(&s->c_phase);
    av_freep(&s->mag_total);
    av_freep(&s->lfe_mag);
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    AudioSurroundContext *s = ctx->priv;
    int ret;

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    s->hop_size = FFMAX(1, s->win_size * (1. - s->overlap));

    allchannels_spread(ctx);
    set_input_levels(ctx);
    set_output_levels(ctx);

    return 0;
}

#define OFFSET(x) offsetof(AudioSurroundContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define TFLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption surround_options[] = {
    { "chl_out",   "set output channel layout", OFFSET(out_channel_layout_str), AV_OPT_TYPE_STRING, {.str="5.1"}, 0,   0, FLAGS },
    { "chl_in",    "set input channel layout",  OFFSET(in_channel_layout_str),  AV_OPT_TYPE_STRING, {.str="stereo"},0, 0, FLAGS },
    { "level_in",  "set input level",           OFFSET(level_in),               AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "level_out", "set output level",          OFFSET(level_out),              AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "lfe",       "output LFE",                OFFSET(output_lfe),             AV_OPT_TYPE_BOOL,   {.i64=1},     0,   1, TFLAGS },
    { "lfe_low",   "LFE low cut off",           OFFSET(lowcutf),                AV_OPT_TYPE_INT,    {.i64=128},   0, 256, FLAGS },
    { "lfe_high",  "LFE high cut off",          OFFSET(highcutf),               AV_OPT_TYPE_INT,    {.i64=256},   0, 512, FLAGS },
    { "lfe_mode",  "set LFE channel mode",      OFFSET(lfe_mode),               AV_OPT_TYPE_INT,    {.i64=0},     0,   1, TFLAGS, "lfe_mode" },
    {  "add",      "just add LFE channel",                  0,                  AV_OPT_TYPE_CONST,  {.i64=0},     0,   1, TFLAGS, "lfe_mode" },
    {  "sub",      "substract LFE channel with others",     0,                  AV_OPT_TYPE_CONST,  {.i64=1},     0,   1, TFLAGS, "lfe_mode" },
    { "angle",     "set soundfield transform angle",        OFFSET(angle),      AV_OPT_TYPE_FLOAT,  {.dbl=90},    0, 360, TFLAGS },
    { "focus",     "set soundfield transform focus",        OFFSET(focus),      AV_OPT_TYPE_FLOAT,  {.dbl=0},    -1,   1, TFLAGS },
    { "fc_in",     "set front center channel input level",  OFFSET(fc_in),      AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "fc_out",    "set front center channel output level", OFFSET(fc_out),     AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "fl_in",     "set front left channel input level",    OFFSET(fl_in),      AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "fl_out",    "set front left channel output level",   OFFSET(fl_out),     AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "fr_in",     "set front right channel input level",   OFFSET(fr_in),      AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "fr_out",    "set front right channel output level",  OFFSET(fr_out),     AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "sl_in",     "set side left channel input level",     OFFSET(sl_in),      AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "sl_out",    "set side left channel output level",    OFFSET(sl_out),     AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "sr_in",     "set side right channel input level",    OFFSET(sr_in),      AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "sr_out",    "set side right channel output level",   OFFSET(sr_out),     AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "bl_in",     "set back left channel input level",     OFFSET(bl_in),      AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "bl_out",    "set back left channel output level",    OFFSET(bl_out),     AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "br_in",     "set back right channel input level",    OFFSET(br_in),      AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "br_out",    "set back right channel output level",   OFFSET(br_out),     AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "bc_in",     "set back center channel input level",   OFFSET(bc_in),      AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "bc_out",    "set back center channel output level",  OFFSET(bc_out),     AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "lfe_in",    "set lfe channel input level",  OFFSET(lfe_in),              AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "lfe_out",   "set lfe channel output level", OFFSET(lfe_out),             AV_OPT_TYPE_FLOAT,  {.dbl=1},     0,  10, TFLAGS },
    { "allx",      "set all channel's x spread",         OFFSET(all_x),         AV_OPT_TYPE_FLOAT,  {.dbl=-1},   -1,  15, TFLAGS },
    { "ally",      "set all channel's y spread",         OFFSET(all_y),         AV_OPT_TYPE_FLOAT,  {.dbl=-1},   -1,  15, TFLAGS },
    { "fcx",       "set front center channel x spread",  OFFSET(fc_x),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "flx",       "set front left channel x spread",    OFFSET(fl_x),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "frx",       "set front right channel x spread",   OFFSET(fr_x),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "blx",       "set back left channel x spread",     OFFSET(bl_x),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "brx",       "set back right channel x spread",    OFFSET(br_x),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "slx",       "set side left channel x spread",     OFFSET(sl_x),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "srx",       "set side right channel x spread",    OFFSET(sr_x),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "bcx",       "set back center channel x spread",   OFFSET(bc_x),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "fcy",       "set front center channel y spread",  OFFSET(fc_y),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "fly",       "set front left channel y spread",    OFFSET(fl_y),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "fry",       "set front right channel y spread",   OFFSET(fr_y),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "bly",       "set back left channel y spread",     OFFSET(bl_y),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "bry",       "set back right channel y spread",    OFFSET(br_y),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "sly",       "set side left channel y spread",     OFFSET(sl_y),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "sry",       "set side right channel y spread",    OFFSET(sr_y),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "bcy",       "set back center channel y spread",   OFFSET(bc_y),          AV_OPT_TYPE_FLOAT,  {.dbl=0.5}, .06,  15, TFLAGS },
    { "win_size", "set window size", OFFSET(win_size), AV_OPT_TYPE_INT, {.i64 = 4096}, 1024, 65536, FLAGS },
    WIN_FUNC_OPTION("win_func", OFFSET(win_func), FLAGS, WFUNC_HANNING),
    { "overlap", "set window overlap", OFFSET(overlap), AV_OPT_TYPE_FLOAT, {.dbl=0.5}, 0, 1, TFLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(surround);

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = config_output,
    },
};

const AVFilter ff_af_surround = {
    .name           = "surround",
    .description    = NULL_IF_CONFIG_SMALL("Apply audio surround upmix filter."),
    .priv_size      = sizeof(AudioSurroundContext),
    .priv_class     = &surround_class,
    .init           = init,
    .uninit         = uninit,
    .activate       = activate,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC(query_formats),
    .flags          = AVFILTER_FLAG_SLICE_THREADS,
    .process_command = process_command,
};
