/*
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

#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include <assert.h>

#include "mpv_talloc.h"

#include "common/msg.h"
#include "options/options.h"
#include "common/common.h"
#include "common/global.h"

#include "stream/stream.h"
#include "sub/dec_sub.h"
#include "demux/demux.h"
#include "video/mp_image.h"

#include "misc/dispatch.h"
#include "core.h"

// 0: primary sub, 1: secondary sub, -1: not selected
static int get_order(struct MPContext *mpctx, struct track *track)
{
    for (int n = 0; n < num_ptracks[STREAM_SUB]; n++) {
        if (mpctx->current_track[n][STREAM_SUB] == track)
            return n;
    }
    return -1;
}

static void reset_subtitles(struct MPContext *mpctx, struct track *track)
{
    if (track->d_sub) {
        sub_reset(track->d_sub);
        sub_set_play_dir(track->d_sub, mpctx->play_dir);
    }
    term_osd_set_subs(mpctx, NULL);
}

void reset_subtitle_state(struct MPContext *mpctx)
{
    for (int n = 0; n < mpctx->num_tracks; n++)
        reset_subtitles(mpctx, mpctx->tracks[n]);
    term_osd_set_subs(mpctx, NULL);
}

void uninit_sub(struct MPContext *mpctx, struct track *track, bool destroy)
{
    if (track && track->d_sub) {
        if (destroy) {
            reset_subtitles(mpctx, track);
            sub_select(track->d_sub, false);
            sub_destroy(track->d_sub);
            track->d_sub = NULL;
        }
        int order = get_order(mpctx, track);
        osd_set_sub(mpctx->osd, order, NULL);
    }
}

void uninit_sub_all(struct MPContext *mpctx)
{
    for (int n = 0; n < mpctx->num_tracks; n++) {
        uninit_sub(mpctx, mpctx->tracks[n], true);
    }
}

static bool update_subtitle(struct MPContext *mpctx, double video_pts,
                            struct track *track, bool force_read_ahead)
{
    struct dec_sub *dec_sub = track ? track->d_sub : NULL;

    if (!dec_sub || video_pts == MP_NOPTS_VALUE)
        return true;

    if (mpctx->vo_chain) {
        struct mp_image_params params = mpctx->vo_chain->filter->input_params;
        if (params.imgfmt)
            sub_control(dec_sub, SD_CTRL_SET_VIDEO_PARAMS, &params);
    }

    if (track->demuxer->fully_read) {
        // Assume fully_read implies no interleaved audio/video streams.
        // (Reading packets will change the demuxer position.)
        if (sub_can_preload(dec_sub)) {
            demux_seek(track->demuxer, 0, 0);
            sub_preload(dec_sub);
        }
    } else if (!sub_read_packets(dec_sub, video_pts, force_read_ahead))
        return false;

    // Handle displaying subtitles on terminal; never done for secondary subs
    if (mpctx->current_track[0][STREAM_SUB] == track && !mpctx->video_out) {
        char *text = sub_get_text(dec_sub, video_pts, SD_TEXT_TYPE_PLAIN);
        term_osd_set_subs(mpctx, text);
        talloc_free(text);
    }

    // Handle displaying subtitles on VO with no video being played. This is
    // quite different, because normally subtitles are redrawn on new video
    // frames, using the video frames' timestamps.
    if (mpctx->video_out && mpctx->video_status == STATUS_EOF &&
        (mpctx->opts->subs_rend->sub_past_video_end ||
         !mpctx->current_track[0][STREAM_VIDEO] ||
         mpctx->current_track[0][STREAM_VIDEO]->attached_picture)) {
        if (osd_get_force_video_pts(mpctx->osd) != video_pts) {
            osd_set_force_video_pts(mpctx->osd, video_pts);
            osd_query_and_reset_want_redraw(mpctx->osd);
            vo_redraw(mpctx->video_out);
            // Force an arbitrary minimum FPS
            mp_set_timeout(mpctx, 0.1);
        }
    }
    return true;
}

// Returns true if all available packets have been read (which may or may not include
// the ones for the given PTS), unless force_read_ahead is set;
// in that case it returns true only when the subtitles for the given PTS are ready;
// Returns false if the player should wait for new demuxer data, and then should retry.
bool update_subtitles(struct MPContext *mpctx, double video_pts, bool force_read_ahead)
{
    bool ok = true;
    for (int n = 0; n < mpctx->num_tracks; n++) {
        if (mpctx->tracks[n]->type != STREAM_SUB)
            continue;
        ok &= update_subtitle(mpctx, video_pts, mpctx->tracks[n], force_read_ahead);
    }
    return ok;
}

static struct attachment_list *get_all_attachments(struct MPContext *mpctx)
{
    struct attachment_list *list = talloc_zero(NULL, struct attachment_list);
    struct demuxer *prev_demuxer = NULL;
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *t = mpctx->tracks[n];
        if (!t->demuxer || prev_demuxer == t->demuxer)
            continue;
        prev_demuxer = t->demuxer;
        for (int i = 0; i < t->demuxer->num_attachments; i++) {
            struct demux_attachment *att = &t->demuxer->attachments[i];
            struct demux_attachment copy = {
                .name = talloc_strdup(list, att->name),
                .type = talloc_strdup(list, att->type),
                .data = talloc_memdup(list, att->data, att->data_size),
                .data_size = att->data_size,
            };
            MP_TARRAY_APPEND(list, list->entries, list->num_entries, copy);
        }
    }
    return list;
}

static bool init_subdec(struct MPContext *mpctx, struct track *track)
{
    assert(!track->d_sub);

    if (!track->demuxer || !track->stream)
        return false;

    track->d_sub = sub_create(mpctx->global, track->stream,
                              get_all_attachments(mpctx),
                              get_order(mpctx, track));
    if (!track->d_sub)
        return false;

    struct track *vtrack = mpctx->current_track[0][STREAM_VIDEO];
    struct mp_codec_params *v_c =
        vtrack && vtrack->stream ? vtrack->stream->codec : NULL;
    double fps = v_c ? v_c->fps : 25;
    sub_control(track->d_sub, SD_CTRL_SET_VIDEO_DEF_FPS, &fps);

    return true;
}

void reinit_sub(struct MPContext *mpctx, struct track *track)
{
    if (!track || !track->stream || track->stream->type != STREAM_SUB)
        return;

    int order = get_order(mpctx, track);
    if (track->d_sub) {
        int cur_order = sub_get_order(track->d_sub);
        if (order != cur_order)
            uninit_sub(mpctx, track, true);
    }
    if (!track->d_sub) {
        if (!init_subdec(mpctx, track)) {
            error_on_track(mpctx, track);
            return;
        }
    }
    sub_select(track->d_sub, true);

    if (track->selected)
        osd_set_sub(mpctx->osd, order, track->d_sub);

    if (mpctx->playback_initialized) {
        if (mpctx->paused) {
            // If a track is reinit’ed during pause
            // this will be the only time that update_subtitles is called
            // until playback resumes, so we need to enable read ahead
            // and wait until the current subtitles have been decoded
            // before returning and drawing them. (This isn’t necessary
            // during playback because subs are updated on every new frame
            // and drawing them a couple frames too late doesn’t matter too much.)
            struct mp_dispatch_queue *demux_waiter = mp_dispatch_create(NULL);
            demux_set_stream_wakeup_cb(track->stream,
                                       (void (*)(void *)) mp_dispatch_interrupt, demux_waiter);

            for (;; mp_dispatch_queue_process(demux_waiter, INFINITY))
                if (update_subtitle(mpctx, mpctx->playback_pts, track, true))
                    break;

            demux_set_stream_wakeup_cb(track->stream, NULL, NULL);
            talloc_free(demux_waiter);
        } else
            update_subtitle(mpctx, mpctx->playback_pts, track, true);
    }
    MP_VERBOSE(track->demuxer, "Sub reinit done\n");
}

void reinit_sub_all(struct MPContext *mpctx)
{
    for (int n = 0; n < mpctx->num_tracks; n++)
        reinit_sub(mpctx, mpctx->tracks[n]);
}
