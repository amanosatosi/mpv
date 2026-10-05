/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <libavutil/common.h>

#include "common/msg.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/options.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"
#include "video/filter/vf_sub.h"
#include "sub/osd.h"
#include "sub/dec_sub.h"

#include "video/sws_utils.h"

#include "options/m_option.h"

struct vf_sub_opts {
    int top_margin, bottom_margin;
};

struct priv {
    struct vf_sub_opts *opts;
    struct mp_image_pool *pool;
};

static void vf_sub_process(struct mp_filter *f)
{
    struct priv *priv = f->priv;

    if (!mp_pin_can_transfer_data(f->ppins[1], f->ppins[0]))
        return;

    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);

    if (mp_frame_is_signaling(frame)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    struct mp_stream_info *info = mp_filter_find_stream_info(f);
    struct osd_state *osd = info ? info->osd : NULL;

    if (!osd)
        goto error;

    osd_set_render_subs_in_filter(osd, true);

    if (frame.type != MP_FRAME_VIDEO)
        goto error;

    struct mp_image *mpi = frame.data;

    struct mp_osd_res dim = {
        .w = mpi->w,
        .h = mpi->h + priv->opts->top_margin + priv->opts->bottom_margin,
        .mt = priv->opts->top_margin,
        .mb = priv->opts->bottom_margin,
        .display_par = mpi->params.p_w / (double)mpi->params.p_h,
    };

    if (dim.w != mpi->w || dim.h != mpi->h) {
        struct mp_image *dmpi =
            mp_image_pool_get(priv->pool, mpi->imgfmt, dim.w, dim.h);
        if (!dmpi)
            goto error;
        mp_image_copy_attributes(dmpi, mpi);
        int y1 = MP_ALIGN_DOWN(priv->opts->top_margin, mpi->fmt.align_y);
        int y2 = MP_ALIGN_DOWN(y1 + mpi->h, mpi->fmt.align_y);
        struct mp_image cropped = *dmpi;
        mp_image_crop(&cropped, 0, y1, mpi->w, y1 + mpi->h);
        mp_image_copy(&cropped, mpi);
        mp_image_clear(dmpi, 0, 0, dmpi->w, y1);
        mp_image_clear(dmpi, 0, y2, dmpi->w, dim.h);
        mp_frame_unref(&frame);
        mpi = dmpi;
        frame = (struct mp_frame){MP_FRAME_VIDEO, mpi};
    }

    osd_draw_on_image_p(osd, dim, mpi->pts, OSD_DRAW_SUB_FILTER, priv->pool, mpi);

    mp_pin_in_write(f->ppins[1], frame);
    return;

error:
    MP_ERR(f, "unsupported format, missing OSD, or failed allocation\n");
    mp_frame_unref(&frame);
    mp_filter_internal_mark_failed(f);
}

static void vf_sub_destroy(struct mp_filter *f)
{
    struct mp_stream_info *info = mp_filter_find_stream_info(f);
    struct osd_state *osd = info ? info->osd : NULL;
    if (osd)
        osd_set_render_subs_in_filter(osd, false);
}

static const struct mp_filter_info vf_sub_filter = {
    .name = "sub",
    .process = vf_sub_process,
    .destroy = vf_sub_destroy,
    .priv_size = sizeof(struct priv),
};

static struct mp_filter *vf_sub_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_sub_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *priv = f->priv;
    priv->opts = talloc_steal(priv, options);
    priv->pool = mp_image_pool_new(priv);

    return f;
}

#define OPT_BASE_STRUCT struct vf_sub_opts
static const m_option_t vf_opts_fields[] = {
    {"bottom-margin", OPT_INT(bottom_margin), M_RANGE(0, 2000)},
    {"top-margin", OPT_INT(top_margin), M_RANGE(0, 2000)},
    {0}
};

const struct mp_user_filter_entry vf_sub = {
    .desc = {
        .description = "Render subtitles",
        .name = "sub",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .options = vf_opts_fields,
    },
    .create = vf_sub_create,
};


struct mangetsu_blend_priv {
    struct mp_image_pool *pool;
    struct mp_sws_context *sws;
    bool owns_sub_filter;
};

static void mangetsu_blend_release_osd(struct mp_filter *f)
{
    struct mangetsu_blend_priv *priv = f->priv;
    if (!priv->owns_sub_filter)
        return;

    struct mp_stream_info *info = mp_filter_find_stream_info(f);
    struct osd_state *osd = info ? info->osd : NULL;
    if (osd)
        osd_set_render_subs_in_filter(osd, false);
    priv->owns_sub_filter = false;
}

static void mangetsu_blend_process(struct mp_filter *f)
{
    struct mangetsu_blend_priv *priv = f->priv;

    if (!mp_pin_can_transfer_data(f->ppins[1], f->ppins[0]))
        return;

    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
    if (mp_frame_is_signaling(frame)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    if (frame.type != MP_FRAME_VIDEO)
        goto error;

    struct mp_stream_info *info = mp_filter_find_stream_info(f);
    struct osd_state *osd = info ? info->osd : NULL;
    if (!osd)
        goto passthrough;

    bool needed = osd_has_bgra_sub_compositor(osd);
    if (!needed) {
        mangetsu_blend_release_osd(f);
        goto passthrough;
    }

    // A user-specified vf=sub owns subtitle-in-video rendering already.
    if (!priv->owns_sub_filter && osd_get_render_subs_in_filter(osd))
        goto passthrough;

    if (!priv->owns_sub_filter) {
        osd_set_render_subs_in_filter(osd, true);
        priv->owns_sub_filter = true;
        MP_VERBOSE(f, "Mangetsu \\blend detected; enabling destination-aware "
                      "BGRA subtitle composition.\n");
    }

    struct mp_image *mpi = frame.data;

    if (mpi->hwctx) {
        struct mp_image *downloaded = mp_image_hw_download(mpi, priv->pool);
        if (!downloaded) {
            MP_ERR(f, "Mangetsu \\blend requires downloading the video frame.\n");
            goto error;
        }
        mp_frame_unref(&frame);
        mpi = downloaded;
        frame = (struct mp_frame){MP_FRAME_VIDEO, mpi};
    }

    if (mpi->imgfmt != IMGFMT_BGRA) {
        struct mp_image *bgra =
            mp_image_pool_get(priv->pool, IMGFMT_BGRA, mpi->w, mpi->h);
        if (!bgra)
            goto error;

        mp_image_copy_attributes(bgra, mpi);
        mp_image_setfmt(bgra, IMGFMT_BGRA);
        mp_image_params_guess_csp(&bgra->params);

        if (mp_sws_scale(priv->sws, bgra, mpi) < 0) {
            mp_image_unrefp(&bgra);
            MP_ERR(f, "Failed converting video to BGRA for Mangetsu \\blend.\n");
            goto error;
        }

        mp_frame_unref(&frame);
        mpi = bgra;
        frame = (struct mp_frame){MP_FRAME_VIDEO, mpi};
    }

    struct mp_osd_res dim = {
        .w = mpi->w,
        .h = mpi->h,
        .display_par = mpi->params.p_w / (double) mpi->params.p_h,
    };

    if (!osd_draw_subs_on_bgra_p(osd, dim, mpi->pts, priv->pool, mpi)) {
        MP_ERR(f, "Mangetsu \\blend subtitle composition failed.\n");
        goto error;
    }

passthrough:
    mp_pin_in_write(f->ppins[1], frame);
    return;

error:
    mangetsu_blend_release_osd(f);
    mp_frame_unref(&frame);
    mp_filter_internal_mark_failed(f);
}

static void mangetsu_blend_destroy(struct mp_filter *f)
{
    mangetsu_blend_release_osd(f);
}

static const struct mp_filter_info mangetsu_blend_filter = {
    .name = "mangetsu-blend",
    .process = mangetsu_blend_process,
    .destroy = mangetsu_blend_destroy,
    .priv_size = sizeof(struct mangetsu_blend_priv),
};

struct mp_filter *mp_mangetsu_blend_create(struct mp_filter *parent)
{
    struct mp_filter *f = mp_filter_create(parent, &mangetsu_blend_filter);
    if (!f)
        return NULL;

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct mangetsu_blend_priv *priv = f->priv;
    priv->pool = mp_image_pool_new(priv);
    priv->sws = mp_sws_alloc(priv);
    if (!priv->pool || !priv->sws) {
        talloc_free(f);
        return NULL;
    }
    priv->sws->log = f->log;
    mp_sws_enable_cmdline_opts(priv->sws, f->global);

    return f;
}
