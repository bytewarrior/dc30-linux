/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dc30_alsa.h - ALSA capture device for the DC30's AD1843
 */

#ifndef DC30_ALSA_H
#define DC30_ALSA_H

struct dc30_dev;
struct seq_file;

/* Register the ALSA card. Only after dc30_audio_init() succeeded. */
int dc30_alsa_register(struct dc30_dev *dc30);
void dc30_alsa_unregister(struct dc30_dev *dc30);

#include <linux/types.h>

/* The capture stream is S16_LE stereo only. */
#define DC30_ALSA_FRAME_BYTES	4

/* Audio position at a video field interrupt, for the metadata records
 * (dc30_meta.h). valid is false while no ALSA capture runs.
 */
struct dc30_audio_mark {
	bool valid;
	bool videolock;		/* sample clock locked to the video */
	u32 epoch;		/* number of the ALSA stream (start) */
	u32 rate;		/* nominal rate of that stream */
	s64 pos;		/* bytes into the ALSA stream */
};

/* Video field interrupt: sample the audio position (IRQ context). */
void dc30_alsa_vsync(struct dc30_dev *dc30, struct dc30_audio_mark *mark);

/* Keep the drain thread off the PostOffice (hold) and let it go on again.
 * Waits for a drain pass in progress. The ASIC ring overruns after
 * ~160ms without a drain. Sleeps.
 */
void dc30_alsa_hold(struct dc30_dev *dc30, bool hold);

/* debugfs: drain statistics of the current/last capture. */
void dc30_alsa_stats_show(struct seq_file *m, struct dc30_dev *dc30);

#endif /* DC30_ALSA_H */
