/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dc30.h - shared definitions for the miro DC30 driver
 *
 * PCI ID and ZR36057 register offsets/bit values below are transcribed from
 * the ZR36057 datasheet and cross-checked against zr36057.h of the
 * historical GPL zoran driver (0.9.4) and the register use of the original
 * Windows driver.
 */

#ifndef DC30_H
#define DC30_H

#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define DC30_PCI_VENDOR_ID_ZORAN	0x11de
#define DC30_PCI_DEVICE_ID_ZR36057	0x6057

/* ---- ZR36057 register offsets (within the single 4KB MMIO BAR) ---- */

#define ZR36057_VFEHCR		0x000	/* Video Front End, Horizontal Config */
#define ZR36057_VFEHCR_HSPOL		(1 << 30)
#define ZR36057_VFEHCR_HSTART_SHIFT	10
#define ZR36057_VFEHCR_HEND_SHIFT	0
#define ZR36057_VFEHCR_HMASK		0x3ff

#define ZR36057_VFEVCR		0x004	/* Video Front End, Vertical Config */
#define ZR36057_VFEVCR_VSPOL		(1 << 30)
#define ZR36057_VFEVCR_VSTART_SHIFT	10
#define ZR36057_VFEVCR_VEND_SHIFT	0
#define ZR36057_VFEVCR_VMASK		0x3ff

#define ZR36057_VFESPFR		0x008	/* Video Front End, Scaler/Pixel Format */
#define ZR36057_VFESPFR_EXTFL		(1 << 26)
#define ZR36057_VFESPFR_TOPFIELD	(1 << 25)
#define ZR36057_VFESPFR_VCLKPOL		(1 << 24)
#define ZR36057_VFESPFR_HFILTER_SHIFT	21
#define ZR36057_VFESPFR_HORDCM_SHIFT	14
#define ZR36057_VFESPFR_VERDCM_SHIFT	8
#define ZR36057_VFESPFR_DISPMODE_SHIFT	6
#define ZR36057_VFESPFR_YUV422		(0 << 3)
#define ZR36057_VFESPFR_RGB888		(1 << 3)
#define ZR36057_VFESPFR_RGB565		(2 << 3)
#define ZR36057_VFESPFR_RGB555		(3 << 3)
#define ZR36057_VFESPFR_ERRDIF		(1 << 2)
#define ZR36057_VFESPFR_PACK24		(1 << 1)
#define ZR36057_VFESPFR_LITTLEENDIAN	(1 << 0)

#define ZR36057_VDTR		0x00c	/* Video Display "Top" base address */
#define ZR36057_VDBR		0x010	/* Video Display "Bottom" base address */

#define ZR36057_VSSFGR		0x014	/* Video Stride, Status, Frame Grab */
#define ZR36057_VSSFGR_DISPSTRIDE_SHIFT	16
#define ZR36057_VSSFGR_VIDOVF		(1 << 8)
#define ZR36057_VSSFGR_SNAPSHOT		(1 << 1)
#define ZR36057_VSSFGR_FRAMEGRAB	(1 << 0)

#define ZR36057_VDCR		0x018	/* Video Display Configuration */
#define ZR36057_VDCR_VIDEN		(1 << 31)
#define ZR36057_VDCR_MINPIX_SHIFT	24
#define ZR36057_VDCR_TRITON		(1 << 24)
#define ZR36057_VDCR_VIDWINHT_SHIFT	12
#define ZR36057_VDCR_VIDWINWID_SHIFT	0

#define ZR36057_MMTR		0x01c	/* Masking Map "Top" */
#define ZR36057_MMBR		0x020	/* Masking Map "Bottom" */

#define ZR36057_OCR		0x024	/* Overlay Control */
#define ZR36057_OCR_OVLENABLE		(1 << 15)

#define ZR36057_SPGPPCR	0x028	/* System/PCI/GPIO Pins Control */
#define ZR36057_SPGPPCR_SOFTRESET	(1 << 24)

#define ZR36057_GPPGCR1	0x02c	/* GPIO Pins + GuestBus Control (1) */

#define ZR36057_MCSAR		0x030	/* MPEG Code Source Address */
#define ZR36057_MCTCR		0x034	/* MPEG Code Transfer Control */
#define ZR36057_MCTCR_CODTIME		(1 << 30)
#define ZR36057_MCTCR_CEMPTY		(1 << 29)
#define ZR36057_MCTCR_CFLUSH		(1 << 28)

#define ZR36057_MCMPR		0x038	/* MPEG Code Memory Pointer */

#define ZR36057_ISR		0x03c	/* Interrupt Status Register */
#define ZR36057_ISR_GIRQ1		(1 << 30)
#define ZR36057_ISR_GIRQ0		(1 << 29)
#define ZR36057_ISR_CODREPIRQ		(1 << 28)
#define ZR36057_ISR_JPEGREPIRQ		(1 << 27)

#define ZR36057_ICR		0x040	/* Interrupt Control Register */
#define ZR36057_ICR_GIRQ1		(1 << 30)
#define ZR36057_ICR_GIRQ0		(1 << 29)
#define ZR36057_ICR_CODREPIRQ		(1 << 28)
#define ZR36057_ICR_JPEGREPIRQ		(1 << 27)
#define ZR36057_ICR_INTPINEN		(1 << 24)

#define ZR36057_I2CBR		0x044	/* I2C Bus Register */
#define ZR36057_I2CBR_SDA		(1 << 1)
#define ZR36057_I2CBR_SCL		(1 << 0)

#define ZR36057_JMC		0x100	/* JPEG Mode and Control */
#define ZR36057_JMC_JPG			(1 << 31)
#define ZR36057_JMC_MJPGCMPMODE		(3 << 29)
#define ZR36057_JMC_GO_EN		(1 << 5)
#define ZR36057_JMC_SYNCMSTR		(1 << 4)
#define ZR36057_JMC_FLD_PER_BUFF	(1 << 3)	/* one field per code buffer */
#define ZR36057_JMC_STLL_LITENDIAN	(1 << 0)

#define ZR36057_JPC		0x104	/* JPEG Process Control */
#define ZR36057_JPC_P_RESET		(1 << 7)
#define ZR36057_JPC_CODTRNSEN		(1 << 5)
#define ZR36057_JPC_ACTIVE		(1 << 0)

#define ZR36057_VSP		0x108	/* Vertical Sync Parameters */
#define ZR36057_HSP		0x10c	/* Horizontal Sync Parameters */
#define ZR36057_FHAP		0x110	/* Field Horizontal Active Portion */
#define ZR36057_FVAP		0x114	/* Field Vertical Active Portion */

#define ZR36057_FPP		0x118	/* Field Process Parameters */
#define ZR36057_FPP_ODD_EVEN		(1 << 0)

#define ZR36057_JCBA		0x11c	/* JPEG Code Base Address (code buffer table) */
#define ZR36057_JCFT		0x120	/* JPEG Code FIFO Threshold */
#define ZR36057_JCGI		0x124	/* JPEG Codec Guest ID */
#define ZR36057_GCR2		0x12c	/* GuestBus Control (2), guests 4-7 timing */

#define ZR36057_POR		0x200	/* PostOffice Register (indirect GuestBus access) */
#define ZR36057_POR_POPEN		(1 << 25)
#define ZR36057_POR_POTIME		(1 << 24)
#define ZR36057_POR_PODIR		(1 << 23)
#define ZR36057_POR_GUEST_SHIFT		20
#define ZR36057_POR_REG_SHIFT		16

#define ZR36057_STR		0x300	/* "Still" Transfer Register */

#define DC30_MMIO_SIZE		0x1000	/* single 4KB ASR window (BAR0) */

/* GuestBus IDs recovered from the original Windows driver (RE), not from the
 * public datasheet (docs/hardware.md). Guest 5 is only touched once, by a
 * dummy read at
 * audio init (dc30.sys _AudioInit); what sits there is not known.
 */
#define DC30_GUEST_ID_AUDIO	4
#define DC30_GUEST_ID_AUDIO_INIT	5
/* JPEG side: ZR36050 data on guest 0 (address
 * bits 1-0), its address latch on guest 1 (bits 9-2), ZR36016 on guest 2.
 * They are defined next to their users in zr36050.c/zr36016.c.
 */

/* Opaque outside dc30_video.c / dc30_alsa.c. */
struct dc30_video;
struct dc30_alsa;
struct dc30_jpeg;

/* State of the automatic po_stream_delay_ns, dc30_po_tune() (zr36057.c).
 * An experiment alternates the pause between the current value and a
 * candidate from one stream read to the next and collects the paired
 * differences in time per byte (ab_*, po_lock). The rest belongs to the
 * audio drain thread (under its drain_lock).
 */
struct dc30_po_tune {
	int ab_delay;		/* candidate, -1: no experiment */
	unsigned int ab_arm;	/* 1: the last read used the candidate */
	unsigned int ab_last;	/* ns/byte of the last read at the current value */
	unsigned int ab_last_len;	/* its length, 0: none to pair */
	u32 ab_n;		/* pairs */
	s64 ab_sum;		/* candidate minus current, ns/byte */
	u64 ab_sumsq;

	u64 timeouts;		/* po_stream_timeouts seen */
	ktime_t next;		/* next experiment */
	unsigned int interval_ms;
	bool up;		/* direction of the next experiment */

	/* For audio_stats. */
	int last_cand;
	int last_diff;		/* mean difference, ns/byte */
	int last_t10;		/* its t value, times 10 */
	u32 last_n;
	u64 experiments, moves_up, moves_down;
};

struct dc30_dev {
	struct pci_dev *pdev;
	void __iomem *mmio;
	spinlock_t reg_lock;
	char name[16];

	/* Bit-banged I2C over ZR36057_I2CBR (GuestBus config only - the JPEG
	 * codec's CodecBus data path is separate).
	 */
	struct i2c_adapter i2c_adap;
	struct i2c_algo_bit_data i2c_algo;
	u32 i2c_bits;

	/* VPX3220A input decoder client, once instantiated at probe time.
	 * NULL if the chip didn't answer (e.g. during early bring-up).
	 */
	struct i2c_client *decoder;

	/* V4L2/vb2 raw capture state (dc30_video.c). NULL if video
	 * registration was skipped (e.g. no decoder found).
	 */
	struct dc30_video *video;

	/* Bit 0: the capture hardware (VFE or JPEG) is taken by a stream
	 * or a debugfs test.
	 */
	unsigned long capture_busy;

	/* JPEG compression state (dc30_jpeg.c). */
	struct dc30_jpeg *jpeg;
	/* ISR bits of the interrupt being handled, for the field handlers. */
	u32 irq_isr;

	/* PostOffice (GuestBus) accesses, see dc30_guest_read/write(). */
	spinlock_t po_lock;
	/* JMC shadow, and whether Go_en is dropped around every access
	 * while a JPEG process runs (both po_lock).
	 */
	u32 jmc;
	bool jpeg_goen;
	/* FIFO stream reads: bytes read, POR reads it took, time spent in
	 * the reads and bytes that never came (po_lock).
	 */
	u64 po_stream_bytes;
	u64 po_stream_polls;
	u64 po_stream_ns;
	u64 po_stream_timeouts;
	/* Pause before polling each streamed byte, unless the
	 * po_stream_delay_ns parameter fixes it. Set by dc30_po_tune().
	 */
	unsigned int po_delay_ns;
	struct dc30_po_tune po_tune;

	/* Audio ASIC on guest 4 and the AD1843 behind it (dc30_audio.c). */
	bool audio_present;
	bool audio_slow_writes;	/* widen guest 4 Tdur around writes */
	unsigned long audio_busy;	/* bit 0: capture running */
	struct mutex ad1843_lock;	/* serializes AD1843 register access */

	/* AD1843 power (dc30_audio.c): converters on while codec_users,
	 * i.e. while a PCM is open or the analog loop-through is on. The
	 * mixer settings live in ad1843_user[] and are written to the codec
	 * whenever it powers up - it forgets them while powered down.
	 */
	struct mutex codec_lock;
	unsigned int codec_users;
	bool codec_on;
	bool codec_loop;	/* loop-through holds a codec reference */
	u16 ad1843_user[32];
	unsigned long codec_powerups;
	unsigned int codec_powerup_ms;

	/* ALSA capture (dc30_alsa.c), NULL without the audio side. */
	struct dc30_alsa *alsa;

	/* Power management (dc30_power.c). */
	struct mutex power_lock;	/* decoder references */
	unsigned int decoder_users;
	unsigned int decoder_sync_users;
	u64 decoder_on_ns;
	u64 decoder_change_ns;		/* power-up, input or standard */
	unsigned int decoder_settle_ms;
	bool board_off;			/* ZR36057 held in soft reset */
	unsigned long suspends, resumes;
	u64 last_resume_us;
};

static inline u32 dc30_read(struct dc30_dev *dc30, u32 reg)
{
	return ioread32(dc30->mmio + reg);
}

static inline void dc30_write(struct dc30_dev *dc30, u32 reg, u32 val)
{
	iowrite32(val, dc30->mmio + reg);
}

#endif /* DC30_H */
