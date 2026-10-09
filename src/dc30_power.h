/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dc30_power.h - board set-up and power management (dc30_power.c)
 */

#ifndef DC30_POWER_H
#define DC30_POWER_H

#include <linux/pm.h>
#include <linux/types.h>
#include <linux/videodev2.h>

struct dc30_dev;
struct seq_file;

/* Probe: bring the board up (GuestBus, GPIOs, codec clocks, ZR36050 in
 * stand-by), before I2C and audio are touched.
 */
void dc30_board_init(struct dc30_dev *dc30);

/* End of probe / start of remove: hand the device to runtime PM (it
 * suspends once nobody holds a reference) and take it back.
 */
void dc30_pm_enable(struct dc30_dev *dc30);
void dc30_pm_disable(struct dc30_dev *dc30);

/* Hold the board awake (runtime PM reference). Sleeps. */
int dc30_pm_get(struct dc30_dev *dc30);
void dc30_pm_put(struct dc30_dev *dc30);

/* Decoder (VPX3220) out of stand-by for as long as a reference is held;
 * 'sync' also keeps its sync outputs on (audio video lock). The caller
 * holds a dc30_pm_get() reference.
 */
int dc30_decoder_get(struct dc30_dev *dc30, bool sync);
void dc30_decoder_put(struct dc30_dev *dc30, bool sync);

/* The decoder's input or standard was changed (power-up counts too). */
void dc30_decoder_changed(struct dc30_dev *dc30);

/* Within a second after dc30_decoder_changed(): wait until the field
 * raster from the decoder is steady (0.8 s at most). Needs its outputs on
 * (s_stream) and the field interrupt unused meanwhile. Sleeps.
 */
void dc30_decoder_settle(struct dc30_dev *dc30, v4l2_std_id std);

/* 1 if the decoder sees a signal on its input, 0 if not, or an error. */
int dc30_decoder_signal(struct dc30_dev *dc30);

/* Remove: power the board down as dc30.sys does on unload. */
void dc30_board_shutdown(struct dc30_dev *dc30);

/* Video bus set-up for a capture stream, and back to idle after it. */
void dc30_board_capture(struct dc30_dev *dc30, bool on);

/* After dc30_board_capture(dc30, true) for a raw stream: ZR36050 to
 * stand-by, it is not needed (module parameter raw_jpeg_standby).
 */
void dc30_board_capture_raw(struct dc30_dev *dc30);

/* ZR36050 reset pulse, clock must be running (dc30_board_capture()). */
void dc30_jpeg_reset(struct dc30_dev *dc30);

/* debugfs: power state of each chip. */
void dc30_power_show(struct seq_file *m, struct dc30_dev *dc30);

/* debugfs: ADV7176 register read-back (0x00-0x12). */
void dc30_encoder_regs_show(struct seq_file *m, struct dc30_dev *dc30);

extern const struct dev_pm_ops dc30_pm_ops;

#endif /* DC30_POWER_H */
