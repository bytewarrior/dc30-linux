// SPDX-License-Identifier: GPL-2.0-only
/*
 * zr36057.c - ZR36057 Video Front End (VFE): raw, unscaled continuous
 * capture.
 *
 * Parts based on zoran_device.c of the GPL zoran driver,
 * Copyright (C) 2000 Serguei Miridonov, maintained by Ronald Bultje and
 * Laurent Pinchart
 * Copyright (C) 2026 bytewarrior
 *
 * Register math ported from zr36057_set_vfe() and the vsync-interrupt
 * buffer-arming code in the GPL zoran driver's zoran_device.c, restricted
 * to the no-decimation case (HorDcm = VerDcm = 0): DC30's VPX3220 already
 * outputs a fixed square-pixel geometry per standard (768x576 PAL,
 * 640x480 NTSC - see zoran_card.c of the same driver,
 * f50sqpixel_dc10/f60sqpixel_dc10), so this driver captures that native
 * size directly instead of implementing the general scaler.
 *
 * DMA layout: VDTR/VDBR point at the same buffer, VDBR offset by one line
 * (bytesperline) and VSSFGR's DispStride set to bytesperline, so top and
 * bottom fields weave together line-by-line into one interlaced frame
 * buffer - the register recipe the GPL driver uses for its raw grabs. Here
 * it is used in continuous mode instead of SnapShot/FrameGrab: a single
 * grab can only be armed for the VSYNC *after* the one that reported the
 * previous grab done, which costs one field per frame (measured: 60ms per
 * frame, 16.7fps). See dc30_video.c for how fields are taken out of the
 * continuously written buffer.
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#include "dc30.h"
#include "zr36057.h"

struct dc30_std_geometry {
	u16 wa;		/* active samples per line */
	u16 ha;		/* active lines per frame (both fields) */
	u16 hstart;	/* horizontal sync start (pre OR-with-1 quirk) */
	u16 vstart;	/* vertical sync start */
};

/* Video FIFO / PCI burst tuning (VDCR), module parameters so they can be
 * compared on a given board without rebuilding. MinPix is the Video FIFO
 * fill level, in doublewords out of 64, at which a PCI burst is requested.
 * Triton = 0 drops REQ right after GNT, so each burst ends at the latency
 * timer - the datasheet's workaround for Code FIFO overflow in JPEG
 * decompression on Intel Triton chipsets.
 *
 * dc30.sys programs MinPix 8 with Triton = 0 (tunable through its
 * registry "Threshold"). The defaults here are the GPL driver's effective
 * values instead (MinPix 16 | Triton = 17), because that is what measured
 * clean on the real board behind a PCIe-to-PCI bridge,
 * one-minute PAL runs:
 *
 *   triton=0 minpix=8   8534 FIFO overflows, 5085 frames lost
 *   triton=0 minpix=4   7969 FIFO overflows, 4662 frames lost
 *   triton=0 minpix=16    14 FIFO overflows,   14 frames lost
 *   triton=1 minpix=17     0 FIFO overflows,    0 frames lost
 *
 * The Triton bit depends on the host chipset, not on the card, so the
 * Windows driver's 1990s choice doesn't carry over.
 */
static unsigned int dc30_minpix = 17;
module_param_named(minpix, dc30_minpix, uint, 0644);
MODULE_PARM_DESC(minpix, "Video FIFO burst request threshold in dwords, 1-60, l.s. bit follows triton (default 17)");

static bool dc30_triton = true;
module_param_named(triton, dc30_triton, bool, 0644);
MODULE_PARM_DESC(triton, "VDCR Triton bit: 1 = other PCI bridges (default), 0 = Intel Triton REQ behavior (dc30.sys)");

/* f50sqpixel_dc10 / f60sqpixel_dc10 from the DC30 card table. SECAM shares
 * PAL's 625-line geometry (only chroma decoding differs, handled entirely
 * in the VPX3220).
 *
 * PAL VStart is 17, not the table's 16: with 16, the first line of both
 * fields was still vertical blanking (measured on the board), and the
 * PAL half line 23 only showed up in row 2. With 17, row 0 is that half
 * line, i.e. the standard BT.601 576-line window. NTSC is still the
 * table value - not yet checked with an NTSC source.
 *
 * (A CCIR601 geometry test - 720 wide, HStart 75 - overflowed the FIFO on
 * every field. That was before the VID_DIR GPIO and Triton bit fixes and
 * would need re-testing before drawing conclusions from it.)
 */
static const struct dc30_std_geometry dc30_geo_pal = { 768, 576, 0, 17 };
static const struct dc30_std_geometry dc30_geo_ntsc = { 640, 480, 0, 12 };

static const struct dc30_std_geometry *dc30_geo_for_std(v4l2_std_id std)
{
	if (std & V4L2_STD_NTSC)
		return &dc30_geo_ntsc;
	return &dc30_geo_pal; /* PAL and SECAM */
}

/* General decimation support (the code below) is kept since it's the
 * correct, complete port of zr36057_set_vfe(); it was used during
 * bring-up to tell a per-line sampling artifact (a fine diagonal moire,
 * caused by GPIO3 = video bus direction left at 0, see dc30_power.c) from
 * a field-interleaving bug. DC30_VFE_TEST_HORDCM_WIDTH is 0: full native
 * width, HorDcm = 0.
 */
#define DC30_VFE_TEST_HORDCM_WIDTH	0

void dc30_vfe_geometry_for_std(v4l2_std_id std, struct dc30_vfe_geometry *geo)
{
	const struct dc30_std_geometry *g = dc30_geo_for_std(std);

	geo->width = DC30_VFE_TEST_HORDCM_WIDTH ? DC30_VFE_TEST_HORDCM_WIDTH
						 : g->wa;
	geo->height = g->ha;
}

void dc30_vfe_set_geometry(struct dc30_dev *dc30, v4l2_std_id std,
			    const struct dc30_vfe_geometry *geo)
{
	const struct dc30_std_geometry *g = dc30_geo_for_std(std);
	unsigned int hstart, hend, vstart, vend;
	unsigned int vidwinwid, vidwinht, we, he, x, y;
	unsigned int hcrop1, hcrop2, vcrop1, vcrop2, hordcm, verdcm;
	u32 reg;

	/* Horizontal: full general scaler formula (zr36057_set_vfe()), not
	 * just the HorDcm=0 special case - see the EXPERIMENT comment above.
	 */
	vidwinwid = geo->width;
	x = (vidwinwid * 64 + g->wa - 1) / g->wa;
	we = (vidwinwid * 64) / x;
	hordcm = 64 - x;
	hcrop1 = 2 * ((g->wa - we) / 4);
	hcrop2 = g->wa - we - hcrop1;
	hstart = (g->hstart | 1) + hcrop1;
	hend = (g->hstart | 1) + g->wa - 1 - hcrop2;
	reg = ((hstart & ZR36057_VFEHCR_HMASK) << ZR36057_VFEHCR_HSTART_SHIFT)
	    | ((hend & ZR36057_VFEHCR_HMASK) << ZR36057_VFEHCR_HEND_SHIFT);
	dc30_write(dc30, ZR36057_VFEHCR, reg);

	/* Vertical: DispMode always 0 here (interlaced, full frame height
	 * always exceeds one field) - no decimation tested vertically yet.
	 */
	vidwinht = geo->height / 2;
	y = (vidwinht * 64 * 2 + g->ha - 1) / g->ha;
	he = (vidwinht * 64) / y;
	verdcm = 64 - y;
	vcrop1 = (g->ha / 2 - he) / 2;
	vcrop2 = g->ha / 2 - he - vcrop1;
	vstart = g->vstart + vcrop1;
	vend = g->vstart + g->ha / 2 - vcrop2;
	reg = ((vstart & ZR36057_VFEVCR_VMASK) << ZR36057_VFEVCR_VSTART_SHIFT)
	    | ((vend & ZR36057_VFEVCR_VMASK) << ZR36057_VFEVCR_VEND_SHIFT);
	dc30_write(dc30, ZR36057_VFEVCR, reg);

	/* Interlaced two-field display mode (DispMode = 0), packed YUV422,
	 * TopField/ExtFl as the GPL driver's comments insist are required
	 * for correct field order on this chip family. ExtFl is skipped for
	 * NTSC only, matching the ported code exactly. HFilter thresholds
	 * also match zr36057_set_vfe() exactly.
	 *
	 * dc30.sys sets the top byte to TopField/ExtFl (0x06000000) the
	 * same way; bits 27-31 are undocumented and stay clear.
	 */
	reg = ZR36057_VFESPFR_TOPFIELD | ZR36057_VFESPFR_YUV422;
	if (!(std & V4L2_STD_NTSC))
		reg |= ZR36057_VFESPFR_EXTFL;
	reg |= hordcm << ZR36057_VFESPFR_HORDCM_SHIFT;
	reg |= verdcm << ZR36057_VFESPFR_VERDCM_SHIFT;
	if (hordcm >= 48)
		reg |= 3 << ZR36057_VFESPFR_HFILTER_SHIFT;
	else if (hordcm >= 32)
		reg |= 2 << ZR36057_VFESPFR_HFILTER_SHIFT;
	else if (hordcm >= 16)
		reg |= 1 << ZR36057_VFESPFR_HFILTER_SHIFT;
	dc30_write(dc30, ZR36057_VFESPFR, reg);

	/* MinPix shares its l.s. bit with the Triton bit, so only even
	 * MinPix values exist with Triton = 0 and only odd ones with 1.
	 */
	reg = (clamp(dc30_minpix, 1U, 0x3cU) << ZR36057_VDCR_MINPIX_SHIFT) &
	      ~ZR36057_VDCR_TRITON;
	if (dc30_triton)
		reg |= ZR36057_VDCR_TRITON;
	reg |= vidwinht << ZR36057_VDCR_VIDWINHT_SHIFT;
	reg |= vidwinwid << ZR36057_VDCR_VIDWINWID_SHIFT;
	reg |= dc30_read(dc30, ZR36057_VDCR) & ZR36057_VDCR_VIDEN;
	dc30_write(dc30, ZR36057_VDCR, reg);
}

void dc30_vfe_enable(struct dc30_dev *dc30, bool on)
{
	u32 reg = dc30_read(dc30, ZR36057_VDCR);

	if (on)
		reg |= ZR36057_VDCR_VIDEN;
	else
		reg &= ~ZR36057_VDCR_VIDEN;
	dc30_write(dc30, ZR36057_VDCR, reg);
}

void dc30_vfe_start_continuous(struct dc30_dev *dc30, dma_addr_t dma_addr,
			       unsigned int bytesperline)
{
	u32 reg;

	dc30_write(dc30, ZR36057_VDTR, (u32)dma_addr);
	dc30_write(dc30, ZR36057_VDBR, (u32)dma_addr + bytesperline);

	/* SnapShot = FrameGrab = 0: continuous mode. */
	reg = bytesperline << ZR36057_VSSFGR_DISPSTRIDE_SHIFT;
	reg |= ZR36057_VSSFGR_VIDOVF;	/* write-1-to-clear overflow status */
	dc30_write(dc30, ZR36057_VSSFGR, reg);
}

bool dc30_vfe_check_overflow(struct dc30_dev *dc30)
{
	u32 reg = dc30_read(dc30, ZR36057_VSSFGR);

	if (!(reg & ZR36057_VSSFGR_VIDOVF))
		return false;
	/* Writes back the unchanged DispStride and SnapShot/FrameGrab = 0. */
	dc30_write(dc30, ZR36057_VSSFGR, reg);
	return true;
}

/* ---- PostOffice (GuestBus) access ----
 *
 * Protocol as dc30.sys's _I22_ReadGuest/_I22_WriteGuest/_CheckGuest: wait
 * for POPen (pending) to clear, issue the access, wait again, then check
 * POTime (the guest didn't respond in time). POTime is cleared the way
 * dc30.sys does it, by writing the register back with that bit set.
 *
 * While a JPEG process runs, Go_en is dropped around every access, to any
 * guest (dc30.sys _CheckSetGoEnable/_CheckResetGoEnableWait): clear it,
 * give the ZR36057 time (dc30.sys: six POR reads), access, wait for the access
 * to finish, set it again. dc30->jmc shadows JMC for this, both only
 * change under po_lock.
 */
#define DC30_PO_POLL_US		100

/* Caller holds po_lock. */
static int dc30_po_wait(struct dc30_dev *dc30, u32 *por)
{
	unsigned int i;
	u32 reg;

	for (i = 0; i < DC30_PO_POLL_US; i++) {
		reg = dc30_read(dc30, ZR36057_POR);
		if (!(reg & ZR36057_POR_POPEN))
			break;
		udelay(1);
	}

	if (reg & (ZR36057_POR_POPEN | ZR36057_POR_POTIME)) {
		if (reg & ZR36057_POR_POTIME)
			dc30_write(dc30, ZR36057_POR, reg | ZR36057_POR_POTIME);
		return -ETIMEDOUT;
	}

	if (por)
		*por = reg;
	return 0;
}

/* dc30.sys waits with six POR reads after dropping Go_en - on a real PCI
 * bus about a microsecond, the datasheet asks for 15 PCI clocks (450 ns).
 * Behind a PCIe-to-PCI bridge each read is ~2.5 us, and Go_en stayed off
 * ~15 us per access: with the audio counter read at every field
 * interrupt (four accesses) that fell right into the moment the ZR36057
 * starts the next frame, and the codec started late or not at all (seen
 * on the board). One read (it pushes the posted JMC write through) plus
 * the datasheet's time is enough.
 */
static unsigned int dc30_goen_reads = 1;
module_param_named(goen_reads, dc30_goen_reads, uint, 0644);
MODULE_PARM_DESC(goen_reads, "POR reads after dropping Go_en, 1-6 (default 1; dc30.sys: 6)");

#define DC30_GOEN_SETTLE_NS	450

/* Caller holds po_lock. */
static void dc30_po_goen_drop(struct dc30_dev *dc30)
{
	unsigned int i, n = clamp(dc30_goen_reads, 1U, 6U);

	if (!dc30->jpeg_goen)
		return;
	dc30_write(dc30, ZR36057_JMC, dc30->jmc & ~ZR36057_JMC_GO_EN);
	for (i = 0; i < n; i++)
		dc30_read(dc30, ZR36057_POR);
	ndelay(DC30_GOEN_SETTLE_NS);
}

/* Caller holds po_lock, the access has finished (dc30_po_wait()). */
static void dc30_po_goen_restore(struct dc30_dev *dc30)
{
	if (dc30->jpeg_goen)
		dc30_write(dc30, ZR36057_JMC, dc30->jmc);
}

int dc30_guest_read(struct dc30_dev *dc30, unsigned int guest,
		    unsigned int reg, u8 *val)
{
	unsigned long flags;
	u32 por;
	int err;

	spin_lock_irqsave(&dc30->po_lock, flags);
	err = dc30_po_wait(dc30, NULL);
	if (!err) {
		dc30_po_goen_drop(dc30);
		dc30_write(dc30, ZR36057_POR,
			   (guest & 7) << ZR36057_POR_GUEST_SHIFT |
			   (reg & 7) << ZR36057_POR_REG_SHIFT);
		err = dc30_po_wait(dc30, &por);
		dc30_po_goen_restore(dc30);
	}
	spin_unlock_irqrestore(&dc30->po_lock, flags);

	*val = err ? 0 : por & 0xff;
	return err;
}

int dc30_guest_write(struct dc30_dev *dc30, unsigned int guest,
		     unsigned int reg, u8 val)
{
	unsigned long flags;
	int err;

	spin_lock_irqsave(&dc30->po_lock, flags);
	err = dc30_po_wait(dc30, NULL);
	if (!err) {
		dc30_po_goen_drop(dc30);
		dc30_write(dc30, ZR36057_POR,
			   ZR36057_POR_PODIR |
			   (guest & 7) << ZR36057_POR_GUEST_SHIFT |
			   (reg & 7) << ZR36057_POR_REG_SHIFT | val);
		err = dc30_po_wait(dc30, NULL);
		dc30_po_goen_restore(dc30);
	}
	spin_unlock_irqrestore(&dc30->po_lock, flags);

	return err;
}

/* Behind the PCIe-to-PCI bridge the first POR read after re-arming a
 * stream read nearly always still sees POPen: the read overtakes the
 * guest cycle the posted re-arm write started, and each extra read is
 * another bridge round trip (~2.3us). Measured on the board behind a
 * PCIe-to-PCI bridge, audio FIFO:
 *
 *   delay ns     0     300   350   400   450   500   600   1000
 *   reads/byte   1.99  1.99  1.80  1.15  1.00  1.00  1.00  1.00
 *   ns/byte      4069  4146  3830  2699  2800  2825  3037  3705
 *
 * With video DMA on the bus 450ns gave 1.07 reads/byte, so the optimum
 * moves with the bus load, and on another bridge or a real PCI slot it
 * is somewhere else entirely. dc30_po_tune() therefore finds the pause
 * itself (-1, default); a value >= 0 fixes it.
 */
static int dc30_po_stream_delay_ns = -1;
module_param_named(po_stream_delay_ns, dc30_po_stream_delay_ns, int, 0644);
MODULE_PARM_DESC(po_stream_delay_ns, "Pause before polling each streamed PostOffice byte, ns (default -1 = automatic)");

/* Caller holds po_lock. Returns the data byte of the pending read, or -1
 * if POPen never cleared. One dword read gives both POPen and the data -
 * dc30.sys reads the status byte and then the data byte, but behind a
 * PCIe-to-PCI bridge every non-posted read costs a microsecond or more.
 * A pause too short costs time, never data: the byte is only taken once
 * POPen is clear.
 */
static int dc30_po_read_byte(struct dc30_dev *dc30, unsigned int delay_ns)
{
	unsigned int i;
	u32 por;

	/* dc30.sys polls 8 times without delay; the ASIC answers within a
	 * few PCI clocks.
	 */
	if (delay_ns)
		ndelay(delay_ns);
	for (i = 0; i < 64; i++) {
		por = dc30_read(dc30, ZR36057_POR);
		if (!(por & ZR36057_POR_POPEN)) {
			dc30->po_stream_polls += i + 1;
			return por & 0xff;
		}
	}
	dc30->po_stream_polls += i;
	dc30->po_stream_timeouts++;
	return -1;
}

/* Below this no experiment goes while a JPEG process runs: longer reads
 * keep Go_en off longer, and 450ns is proven under MJPEG load (0ns once
 * came with three codec frames lost, cause open). Also the pause at the
 * first capture and after a read timeout.
 */
#define DC30_PO_DELAY_SAFE_NS	450

/* Caller holds po_lock. Pairs each read at the candidate with the read at
 * the current pause just before it, if both were equally long (a short
 * read spreads the request overhead over fewer bytes).
 */
static void dc30_po_ab_add(struct dc30_dev *dc30, unsigned int arm, u64 ns,
			   unsigned int len)
{
	struct dc30_po_tune *t = &dc30->po_tune;
	unsigned int v = div_u64(ns, len);
	s64 diff;

	if (!arm) {
		t->ab_last = v;
		t->ab_last_len = len;
		return;
	}
	if (t->ab_last_len == len) {
		diff = (s64)v - t->ab_last;
		t->ab_n++;
		t->ab_sum += diff;
		t->ab_sumsq += diff * diff;
	}
	t->ab_last_len = 0;
}

int dc30_guest_read_stream(struct dc30_dev *dc30, unsigned int guest,
			   unsigned int reg, u8 *buf, unsigned int len)
{
	int fixed = READ_ONCE(dc30_po_stream_delay_ns);
	struct dc30_po_tune *t = &dc30->po_tune;
	unsigned int delay_ns, arm = 0;
	unsigned long flags;
	unsigned int i;
	ktime_t t0;
	u64 ns;
	int err;

	if (!len)
		return 0;

	spin_lock_irqsave(&dc30->po_lock, flags);
	delay_ns = fixed >= 0 ? fixed : dc30->po_delay_ns;
	/* An experiment: every other read at the candidate. */
	if (fixed < 0 && t->ab_delay >= 0 && !t->ab_arm &&
	    !(dc30->jpeg_goen && t->ab_delay < DC30_PO_DELAY_SAFE_NS)) {
		arm = 1;
		delay_ns = t->ab_delay;
	}
	err = dc30_po_wait(dc30, NULL);
	if (!err) {
		t0 = ktime_get();
		/* Once around the whole run: the last byte read found
		 * POPen clear, so the guest cycles are over at the end.
		 */
		dc30_po_goen_drop(dc30);
		dc30_write(dc30, ZR36057_POR,
			   (guest & 7) << ZR36057_POR_GUEST_SHIFT |
			   (reg & 7) << ZR36057_POR_REG_SHIFT);
		for (i = 0; i < len; i++) {
			int b = dc30_po_read_byte(dc30, delay_ns);

			if (b < 0) {
				err = -ETIMEDOUT;
				break;
			}
			buf[i] = b;
			dc30->po_stream_bytes++;
			/* Re-arm the read, except after the last byte. */
			if (i + 1 < len)
				iowrite8(0, dc30->mmio + ZR36057_POR);
		}
		if (err)
			dc30_po_wait(dc30, NULL);
		dc30_po_goen_restore(dc30);
		ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
		dc30->po_stream_ns += ns;
		if (!err && t->ab_delay >= 0)
			dc30_po_ab_add(dc30, arm, ns, len);
	}
	if (err)
		t->ab_last_len = 0;
	t->ab_arm = arm;
	spin_unlock_irqrestore(&dc30->po_lock, flags);

	return err;
}

/* ---- Automatic stream pause ----
 *
 * The pause is right where a byte costs the least time: pause plus POR
 * reads. Behind a PCIe bridge a read costs ~2.3us and 450ns saves nearly
 * every second read; in a real PCI slot a read is cheap and the pause
 * likely 0.
 *
 * Measured one after the other, the time per byte varies with the bus
 * load far more than a step of the pause changes it: with MJPEG on the
 * bus extra reads come in bursts (board: 1.05 reads/byte on average,
 * single 23ms windows down to 0.3%), and two rule sets judging spans in
 * sequence drove the pause to 475-500ns for nothing. So an experiment
 * measures both at once: the pause alternates between the current value
 * and a candidate DC30_PO_STEP_NS above or below from one 32-byte read
 * (~90us) to the next, and each read at the candidate is compared with
 * the one before it, under practically the same bus load. The candidate
 * wins if it is faster with |t| >= 4; after DC30_PO_PAIRS_MAX pairs
 * without that, nothing changes. A win is followed by the next step in
 * the same direction, otherwise the other direction comes next, after an
 * interval that doubles up to DC30_PO_INTERVAL_MAX_MS.
 *
 * A pause too short costs time, never data: a byte is only taken once
 * POPen is clear. Safety all the same: start at 450ns, never below it
 * while a JPEG process runs, back to at least 450ns after a timeout.
 */
#define DC30_PO_STEP_NS		25
#define DC30_PO_DELAY_MAX_NS	2000
#define DC30_PO_PAIRS_MIN	64
#define DC30_PO_PAIRS_MAX	4096	/* ~1.5s of audio */
#define DC30_PO_T2_MIN		16	/* |t| >= 4: looked at after every drain pass */
#define DC30_PO_INTERVAL_MIN_MS	1000
#define DC30_PO_INTERVAL_MAX_MS	32000

static void dc30_po_ab_set(struct dc30_dev *dc30, int cand)
{
	struct dc30_po_tune *t = &dc30->po_tune;
	unsigned long flags;

	spin_lock_irqsave(&dc30->po_lock, flags);
	t->ab_delay = cand;
	t->ab_arm = 0;
	t->ab_last_len = 0;
	t->ab_n = 0;
	t->ab_sum = 0;
	t->ab_sumsq = 0;
	spin_unlock_irqrestore(&dc30->po_lock, flags);
}

void dc30_po_tune_init(struct dc30_dev *dc30)
{
	struct dc30_po_tune *t = &dc30->po_tune;

	dc30->po_delay_ns = DC30_PO_DELAY_SAFE_NS;
	t->ab_delay = -1;
	t->interval_ms = DC30_PO_INTERVAL_MIN_MS;
	t->last_cand = -1;
}

void dc30_po_tune_start(struct dc30_dev *dc30)
{
	struct dc30_po_tune *t = &dc30->po_tune;
	unsigned long flags;

	dc30_po_ab_set(dc30, -1);
	spin_lock_irqsave(&dc30->po_lock, flags);
	t->timeouts = dc30->po_stream_timeouts;
	spin_unlock_irqrestore(&dc30->po_lock, flags);
	t->interval_ms = DC30_PO_INTERVAL_MIN_MS;
	t->next = ktime_add_ms(ktime_get(), DC30_PO_INTERVAL_MIN_MS);
}

/* The next candidate, in direction t->up if possible, or -1. */
static int dc30_po_candidate(struct dc30_dev *dc30, bool goen)
{
	struct dc30_po_tune *t = &dc30->po_tune;
	unsigned int d = dc30->po_delay_ns;
	int i;

	for (i = 0; i < 2; i++, t->up = !t->up) {
		if (t->up) {
			if (d + DC30_PO_STEP_NS <= DC30_PO_DELAY_MAX_NS)
				return d + DC30_PO_STEP_NS;
		} else if (d >= DC30_PO_STEP_NS &&
			   !(goen && d - DC30_PO_STEP_NS < DC30_PO_DELAY_SAFE_NS)) {
			return d - DC30_PO_STEP_NS;
		}
	}
	return -1;
}

void dc30_po_tune(struct dc30_dev *dc30)
{
	struct dc30_po_tune *t = &dc30->po_tune;
	int fixed = READ_ONCE(dc30_po_stream_delay_ns);
	u64 timeouts, sumsq, sum2, var, t2;
	unsigned long flags;
	s64 sum, mean;
	ktime_t now;
	bool goen;
	int cand;
	u32 n;

	spin_lock_irqsave(&dc30->po_lock, flags);
	timeouts = dc30->po_stream_timeouts;
	goen = dc30->jpeg_goen;
	cand = t->ab_delay;
	n = t->ab_n;
	sum = t->ab_sum;
	sumsq = t->ab_sumsq;
	spin_unlock_irqrestore(&dc30->po_lock, flags);
	now = ktime_get();

	if (fixed >= 0) {
		if (cand >= 0)
			dc30_po_ab_set(dc30, -1);
		t->timeouts = timeouts;
		return;
	}

	/* A read that never completed ends the capture: be safe. */
	if (timeouts != t->timeouts) {
		t->timeouts = timeouts;
		dc30_po_ab_set(dc30, -1);
		WRITE_ONCE(dc30->po_delay_ns,
			   max(dc30->po_delay_ns, DC30_PO_DELAY_SAFE_NS));
		t->interval_ms = DC30_PO_INTERVAL_MAX_MS;
		t->next = ktime_add_ms(now, t->interval_ms);
		return;
	}

	if (cand < 0) {
		if (ktime_before(now, t->next))
			return;
		cand = dc30_po_candidate(dc30, goen);
		if (cand < 0) {
			t->next = ktime_add_ms(now, t->interval_ms);
			return;
		}
		t->experiments++;
		dc30_po_ab_set(dc30, cand);
		return;
	}

	/* A JPEG process started during an experiment below the safe
	 * pause; the reads already stay at the current value.
	 */
	if (goen && cand < DC30_PO_DELAY_SAFE_NS) {
		dc30_po_ab_set(dc30, -1);
		t->next = ktime_add_ms(now, t->interval_ms);
		return;
	}

	if (n < DC30_PO_PAIRS_MIN)
		return;
	mean = div_s64(sum, n);
	sum2 = div64_u64((u64)abs(sum) * abs(sum), n);
	var = sumsq > sum2 ? div64_u64(sumsq - sum2, n - 1) : 0;
	/* t^2 = mean^2 / (var / n) */
	t2 = var ? div64_u64((u64)(mean * mean) * n, var) :
		   (mean ? DC30_PO_T2_MIN : 0);
	if (t2 < DC30_PO_T2_MIN && n < DC30_PO_PAIRS_MAX)
		return;

	dc30_po_ab_set(dc30, -1);
	t->last_cand = cand;
	t->last_diff = mean;
	t->last_n = n;
	t->last_t10 = int_sqrt64(min_t(u64, t2, 1000000) * 100);
	if (mean < 0)
		t->last_t10 = -t->last_t10;

	if (t2 >= DC30_PO_T2_MIN && mean < 0) {
		if (cand > (int)dc30->po_delay_ns)
			t->moves_up++;
		else
			t->moves_down++;
		WRITE_ONCE(dc30->po_delay_ns, cand);
		/* Keep going the same way. */
		t->interval_ms = DC30_PO_INTERVAL_MIN_MS;
		t->next = now;
		return;
	}
	t->up = !t->up;
	t->interval_ms = min(t->interval_ms * 2, DC30_PO_INTERVAL_MAX_MS);
	t->next = ktime_add_ms(now, t->interval_ms);
}

void dc30_po_tune_show(struct seq_file *m, struct dc30_dev *dc30)
{
	struct dc30_po_tune *t = &dc30->po_tune;
	int fixed = READ_ONCE(dc30_po_stream_delay_ns);
	int cand = READ_ONCE(t->ab_delay);

	if (fixed >= 0) {
		seq_printf(m, "po delay       %d ns (fixed, po_stream_delay_ns)\n",
			   fixed);
		return;
	}
	seq_printf(m, "po delay       %u ns (auto)", READ_ONCE(dc30->po_delay_ns));
	if (cand >= 0)
		seq_printf(m, ", testing %d ns (%u pairs)", cand,
			   READ_ONCE(t->ab_n));
	seq_putc(m, '\n');
	if (t->last_cand >= 0)
		seq_printf(m, "po last test   %d ns: %+d ns/byte, t %+d.%d over %u pairs\n",
			   t->last_cand, t->last_diff, t->last_t10 / 10,
			   abs(t->last_t10) % 10, t->last_n);
	seq_printf(m, "po adjustments %llu tests, %llu up, %llu down, timeouts %llu\n",
		   t->experiments, t->moves_up, t->moves_down,
		   dc30->po_stream_timeouts);
}

/* ---- JPEG code DMA ----
 *
 * Register sequences of dc30.sys _I22_SetupCapture, _I22_StartCapture and
 * _I22_Stop. The ZR36057 is sync slave here
 * (SyncMstr = 0): no VSP/HSP/FHAP/FVAP, the VPX3220 drives the syncs.
 */

#define DC30_JMC_IDLE	(ZR36057_JMC_JPG | ZR36057_JMC_MJPGCMPMODE | \
			 ZR36057_JMC_FLD_PER_BUFF | ZR36057_JMC_STLL_LITENDIAN)

static void dc30_jmc_set(struct dc30_dev *dc30, u32 jmc, bool goen)
{
	unsigned long flags;

	spin_lock_irqsave(&dc30->po_lock, flags);
	dc30->jmc = jmc;
	dc30->jpeg_goen = goen;
	dc30_write(dc30, ZR36057_JMC, jmc);
	spin_unlock_irqrestore(&dc30->po_lock, flags);
}

void dc30_jpeg_hw_setup(struct dc30_dev *dc30, dma_addr_t stat_com,
			bool two_fields)
{
	u32 jmc = DC30_JMC_IDLE;

	if (two_fields)
		jmc &= ~ZR36057_JMC_FLD_PER_BUFF;

	dc30_write(dc30, ZR36057_JPC, 0);
	dc30_write(dc30, ZR36057_MCTCR,
		   dc30_read(dc30, ZR36057_MCTCR) | ZR36057_MCTCR_CFLUSH);
	dc30_jmc_set(dc30, jmc, false);
	dc30_write(dc30, ZR36057_FPP, ZR36057_FPP_ODD_EVEN);
	dc30_write(dc30, ZR36057_JCFT, 0x14);
	dc30_write(dc30, ZR36057_JCGI, 0);
	dc30_write(dc30, ZR36057_JCBA, (u32)stat_com);
	dc30_write(dc30, ZR36057_MCTCR,
		   dc30_read(dc30, ZR36057_MCTCR) & ~ZR36057_MCTCR_CFLUSH);
}

void dc30_jpeg_hw_start(struct dc30_dev *dc30)
{
	u32 jpc = dc30_read(dc30, ZR36057_JPC);

	jpc |= ZR36057_JPC_P_RESET;
	dc30_write(dc30, ZR36057_JPC, jpc);
	jpc |= ZR36057_JPC_CODTRNSEN;
	dc30_write(dc30, ZR36057_JPC, jpc);
	jpc |= ZR36057_JPC_ACTIVE;
	dc30_write(dc30, ZR36057_JPC, jpc);
}

void dc30_jpeg_hw_go(struct dc30_dev *dc30)
{
	dc30_jmc_set(dc30, dc30->jmc | ZR36057_JMC_GO_EN, true);
}

void dc30_jpeg_hw_stop(struct dc30_dev *dc30)
{
	dc30_write(dc30, ZR36057_JPC, dc30_read(dc30, ZR36057_JPC) &
		   ~(ZR36057_JPC_P_RESET | ZR36057_JPC_CODTRNSEN |
		     ZR36057_JPC_ACTIVE));
	dc30_jmc_set(dc30, dc30->jmc & ~ZR36057_JMC_GO_EN, false);
	dc30_jmc_set(dc30, DC30_JMC_IDLE, false);
}
