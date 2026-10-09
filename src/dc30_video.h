/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dc30_video.h - V4L2/vb2 video capture device, raw or MJPEG (dc30_video.c)
 */

#ifndef DC30_VIDEO_H
#define DC30_VIDEO_H

struct dc30_audio_mark;
struct dc30_dev;
struct seq_file;

/* No-op (returns 0) if dc30->decoder is NULL or never bound - a missing
 * decoder doesn't fail the PCI probe, see dc30_core.c.
 */
int dc30_video_register(struct dc30_dev *dc30);
void dc30_video_unregister(struct dc30_dev *dc30);

/* Called from dc30_core's hard-IRQ handler when ZR36057_ISR_GIRQ1
 * (vsync/field, DC30's vsync_int per the card table) fires. No-op if video
 * wasn't registered. 'mark' is the audio position at this interrupt,
 * for the metadata records.
 */
void dc30_video_vsync(struct dc30_dev *dc30,
		      const struct dc30_audio_mark *mark);

/* Same interrupt, first thing in the handler (MJPEG: code buffer state
 * at the field change).
 */
void dc30_video_vsync_early(struct dc30_dev *dc30);

/* Name of the selected input. Audio in video lock follows it too. */
const char *dc30_video_input_name(struct dc30_dev *dc30);

/* debugfs: per-stream vsync/frame/drop counters (see struct dc30_video). */
void dc30_video_stats_show(struct seq_file *m, struct dc30_dev *dc30);

#endif /* DC30_VIDEO_H */
