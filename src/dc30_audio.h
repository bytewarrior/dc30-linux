/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dc30_audio.h - audio ASIC (GuestBus guest 4) and AD1843 codec access
 */

#ifndef DC30_AUDIO_H
#define DC30_AUDIO_H

#include <linux/types.h>

struct dc30_dev;
struct dentry;
struct seq_file;

/* Bring up the audio ASIC, check that the AD1843 answers and switch it to
 * 16-slot frames; its converters stay powered down. Sleeps ~0.5s.
 * dc30->audio_present is false only if the ASIC itself wasn't found;
 * video is unaffected either way.
 */
int dc30_audio_init(struct dc30_dev *dc30);

/* Power the AD1843 converters down again (module unload). */
void dc30_audio_exit(struct dc30_dev *dc30);

/* Restore the ZR36057 side of the audio set-up after a board power-down. */
void dc30_audio_board_resume(struct dc30_dev *dc30);

/* AD1843 converters powered up (with the mixer settings) while a
 * reference is held; also holds the board awake. The first get sleeps
 * ~0.5s. Process context.
 */
int dc30_audio_codec_get(struct dc30_dev *dc30);
void dc30_audio_codec_put(struct dc30_dev *dc30);

/* Mixer settings: AD1843 registers 2 (ADC input), 5 and 7 (analog
 * loop-through of Aux 2 / Mic). They are kept by the driver and reach the
 * codec whenever it is powered; unmuting a loop-through powers it up
 * (~0.5s). update returns 1 if the value changed, 0 if not.
 */
u16 dc30_audio_mixer_read(struct dc30_dev *dc30, unsigned int idx);
int dc30_audio_mixer_update(struct dc30_dev *dc30, unsigned int idx,
			    u16 mask, u16 val);

/* ADC overrange bits (register 1), cleared by the read; 0 while the
 * codec is powered down.
 */
int dc30_audio_overrange(struct dc30_dev *dc30, u16 *reg);

/* debugfs: codec power state. */
void dc30_audio_power_show(struct seq_file *m, struct dc30_dev *dc30);

/* Read AD1843 control register 'idx' (0-31). Process context only. */
int dc30_ad1843_read(struct dc30_dev *dc30, unsigned int idx, u16 *val);

/* Write AD1843 control register 'idx' and verify it by reading back.
 * Returns -EIO if it never reads back. Process context only.
 */
int dc30_ad1843_write(struct dc30_dev *dc30, unsigned int idx, u16 val);

/* Single write without the read-back check, for registers that don't
 * read back what was written (the sticky status bits in register 1).
 */
int dc30_ad1843_write_noverify(struct dc30_dev *dc30, unsigned int idx,
			       u16 val);

/* Read-modify-write of the bits in 'mask', verified as dc30_ad1843_write(). */
int dc30_ad1843_update(struct dc30_dev *dc30, unsigned int idx, u16 mask,
		       u16 val);

/* ---- capture ----
 *
 * The ASIC buffers captured bytes in a 32KB ring; its counter is the write
 * position in bytes (dc30.sys sets the modulus to 0x8000 at init). The
 * stream starts with one dummy byte, then 16-bit samples, high byte first,
 * L/R interleaved.
 */
#define DC30_AUDIO_RING_SIZE	0x8000
/* dc30.sys reads 128 bytes per PostOffice stream (_AudioGetStrFifo). Each
 * stream keeps interrupts off (~3us per byte), which delays the video
 * field interrupt and its timestamps; 32 bytes keep that under ~0.1ms.
 */
#define DC30_AUDIO_READ_CHUNK	32

/* Set up the AD1843 for 16-bit stereo capture at 'rate' Hz and start the
 * ASIC. Needs a dc30_audio_codec_get() reference. Sleeps. In video lock
 * mode the rate comes from the line rate and 'rate' is ignored (44.1kHz
 * nominal).
 */
int dc30_audio_capture_start(struct dc30_dev *dc30, unsigned int rate);
void dc30_audio_capture_stop(struct dc30_dev *dc30);
bool dc30_audio_videolock_enabled(void);
/* AD1843 clock generator mode word the next capture start will use. */
u16 dc30_audio_cg_mode_word(void);
/* AD1843 register 15 (which generator clocks the ADCs) at the next start. */
u16 dc30_audio_rate_src_word(void);

/* ASIC write counter, bytes modulo DC30_AUDIO_RING_SIZE. Any context. */
int dc30_audio_counter(struct dc30_dev *dc30, u16 *count);

/* Read 'len' (<= DC30_AUDIO_READ_CHUNK) bytes from the sample FIFO.
 * Interrupts are off meanwhile, ~4us per byte.
 */
int dc30_audio_fifo_read(struct dc30_dev *dc30, u8 *buf, unsigned int len);

/* debugfs: all AD1843 registers next to their reset defaults. */
void dc30_ad1843_regs_show(struct seq_file *m, struct dc30_dev *dc30);

/* debugfs "audio_test": opening it records a few seconds of audio, reading
 * it returns the raw bytes. Bring-up only.
 */
void dc30_audio_debugfs_init(struct dc30_dev *dc30, struct dentry *dir);

#endif /* DC30_AUDIO_H */
