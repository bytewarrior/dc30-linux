// SPDX-License-Identifier: GPL-2.0-only
/*
 * dc30_core.c - PCI driver core for the miro DC30
 *
 * Probes the card, maps its single 4KB MMIO BAR, sets up the I2C bus, the
 * decoder subdev, video, JPEG, audio and power management, dispatches the
 * shared interrupt and provides the debugfs directory. The overall design
 * is described in docs/driver.md.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <media/v4l2-subdev.h>

#include "dc30.h"
#include "dc30_alsa.h"
#include "dc30_audio.h"
#include "dc30_i2c.h"
#include "dc30_jpeg.h"
#include "dc30_power.h"
#include "dc30_video.h"
#include "zr36057.h"

/* 7-bit I2C address of the VPX3220A input decoder on DC30 - confirmed via
 * dc30.sys RE: the board uses the
 * alternate ALSB address strap (0x8E 8-bit / 0x47 7-bit), not the chip's
 * default address.
 */
#define DC30_VPX3220_I2C_ADDR	0x47

static struct dentry *dc30_debugfs_dir;

static irqreturn_t dc30_irq_handler(int irq, void *dev_id)
{
	struct dc30_dev *dc30 = dev_id;
	u32 isr, icr, ours;

	/* The line may be shared (e.g. with a SATA controller),
	 * so only enabled sources are ours: the status bits of the others
	 * stay for whoever polls them (dc30_decoder_settle(), the field
	 * wait of the MJPEG start). GIRQ1 is DC30's vsync_int (see the card
	 * table in the GPL zoran driver's zoran_card.c; DC30 never wires
	 * JPEGRepIRQ, jpeg_int = 0).
	 */
	isr = dc30_read(dc30, ZR36057_ISR);
	if (isr == ~0U)		/* not answering, e.g. powered down */
		return IRQ_NONE;
	icr = dc30_read(dc30, ZR36057_ICR);
	ours = isr & icr & (ZR36057_ISR_GIRQ1 | ZR36057_ISR_GIRQ0 |
			    ZR36057_ISR_CODREPIRQ | ZR36057_ISR_JPEGREPIRQ);
	if (!(icr & ZR36057_ICR_INTPINEN) || !ours)
		return IRQ_NONE;
	/* Not enabled, but looked at per field (dc30_jpeg_vsync(), the
	 * field log): JPEGRepIRQ, and GIRQ0 in case the ZR36050's INT is
	 * wired there (TCVOVF/DATOVF abort; unknown on DC30).
	 */
	if (ours & ZR36057_ISR_GIRQ1)
		ours |= isr & (ZR36057_ISR_JPEGREPIRQ | ZR36057_ISR_GIRQ0);
	dc30_write(dc30, ZR36057_ISR, ours);
	WRITE_ONCE(dc30->irq_isr, ours);
	isr = ours;

	if (isr & ZR36057_ISR_GIRQ1) {
		struct dc30_audio_mark mark;

		/* Before any PostOffice access, which can wait for the audio
		 * drain: which MJPEG frames were done at the field change.
		 */
		dc30_video_vsync_early(dc30);
		/* Audio position first: the frame this field completes
		 * carries it in its metadata record.
		 */
		dc30_alsa_vsync(dc30, &mark);
		dc30_video_vsync(dc30, &mark);
	}

	return IRQ_HANDLED;
}

static int dc30_regs_show(struct seq_file *m, void *data)
{
	struct dc30_dev *dc30 = m->private;
	static const struct { u32 off; const char *name; } regs[] = {
		{ ZR36057_VFEHCR,  "VFEHCR"  },
		{ ZR36057_VFEVCR,  "VFEVCR"  },
		{ ZR36057_VFESPFR, "VFESPFR" },
		{ ZR36057_VDCR,    "VDCR"    },
		{ ZR36057_SPGPPCR, "SPGPPCR" },
		{ ZR36057_GPPGCR1, "GPPGCR1" },
		{ ZR36057_VSSFGR,  "VSSFGR"  },
		{ ZR36057_VDTR,    "VDTR"    },
		{ ZR36057_VDBR,    "VDBR"    },
		{ ZR36057_ISR,     "ISR"     },
		{ ZR36057_ICR,     "ICR"     },
		{ ZR36057_I2CBR,   "I2CBR"   },
		{ ZR36057_JMC,     "JMC"     },
		{ ZR36057_JPC,     "JPC"     },
		{ ZR36057_POR,     "POR"     },
	};
	unsigned int i;
	int err;

	err = dc30_pm_get(dc30);
	if (err)
		return err;
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		seq_printf(m, "%-8s 0x%03x = 0x%08x\n", regs[i].name,
			   regs[i].off, dc30_read(dc30, regs[i].off));
	dc30_pm_put(dc30);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dc30_regs);

static int dc30_decoder_status_show(struct seq_file *m, void *data)
{
	struct dc30_dev *dc30 = m->private;
	struct v4l2_subdev *sd;
	unsigned int ms = 0;
	bool woke;
	u32 status;
	int err;

	if (!dc30->decoder) {
		seq_puts(m, "not present\n");
		return 0;
	}

	/* The decoder only sees a signal while it is powered up. If this
	 * read powered it up, wait for the lock and report how long that
	 * took - the wake-up latency of the decoder.
	 */
	sd = i2c_get_clientdata(dc30->decoder);
	err = dc30_pm_get(dc30);
	if (err)
		return err;
	err = dc30_decoder_get(dc30, false);
	if (err)
		goto out_pm;
	woke = dc30->decoder_users == 1;
	for (;;) {
		err = v4l2_subdev_call(sd, video, g_input_status, &status);
		if (err || !woke || !(status & V4L2_IN_ST_NO_SIGNAL) ||
		    ms >= 1000)
			break;
		msleep(10);
		ms += 10;
	}
	dc30_decoder_put(dc30, false);
out_pm:
	dc30_pm_put(dc30);
	if (err) {
		seq_printf(m, "query failed: %d\n", err);
		return 0;
	}

	seq_printf(m, "status 0x%08x%s\n", status,
		   (status & V4L2_IN_ST_NO_SIGNAL) ?
		   " (no signal)" : " (signal locked)");
	if (woke)
		seq_printf(m, "decoder powered up for this read, %s after %u ms\n",
			   (status & V4L2_IN_ST_NO_SIGNAL) ? "still no lock"
							   : "locked", ms);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dc30_decoder_status);

static int dc30_ad1843_show(struct seq_file *m, void *data)
{
	struct dc30_dev *dc30 = m->private;
	int err;

	err = dc30_pm_get(dc30);
	if (err)
		return err;
	dc30_ad1843_regs_show(m, dc30);
	dc30_pm_put(dc30);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dc30_ad1843);

static int dc30_encoder_show(struct seq_file *m, void *data)
{
	struct dc30_dev *dc30 = m->private;
	int err;

	err = dc30_pm_get(dc30);
	if (err)
		return err;
	dc30_encoder_regs_show(m, dc30);
	dc30_pm_put(dc30);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dc30_encoder);

/* Doesn't wake the card: shows what is powered right now. */
static int dc30_power_state_show(struct seq_file *m, void *data)
{
	dc30_power_show(m, m->private);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dc30_power_state);

static int dc30_audio_stats_show(struct seq_file *m, void *data)
{
	dc30_alsa_stats_show(m, m->private);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dc30_audio_stats);

static int dc30_stats_show(struct seq_file *m, void *data)
{
	dc30_video_stats_show(m, m->private);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dc30_stats);

static unsigned int dc30_latency = 48;
module_param_named(latency, dc30_latency, uint, 0444);
MODULE_PARM_DESC(latency, "PCI latency timer in PCI clocks (default 48, as the GPL driver for ZR36057 rev <= 1)");

static void dc30_apply_pci_tuning(struct pci_dev *pdev)
{
	/* Latency timer 48, the datasheet's recommended value. The VDCR
	 * Triton bit and MinPix are set per stream in
	 * dc30_vfe_set_geometry() (module parameters in zr36057.c) - with
	 * Triton = 0 the latency timer is what limits each video burst.
	 */
	pci_write_config_byte(pdev, PCI_LATENCY_TIMER, min(dc30_latency, 248U));
}

static int dc30_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct dc30_dev *dc30;
	int err;

	dc30 = devm_kzalloc(&pdev->dev, sizeof(*dc30), GFP_KERNEL);
	if (!dc30)
		return -ENOMEM;

	dc30->pdev = pdev;
	spin_lock_init(&dc30->reg_lock);
	spin_lock_init(&dc30->po_lock);
	dc30_po_tune_init(dc30);
	snprintf(dc30->name, sizeof(dc30->name), "dc30");
	pci_set_drvdata(pdev, dc30);

	err = pci_enable_device(pdev);
	if (err)
		return err;

	err = pci_request_regions(pdev, dc30->name);
	if (err)
		goto err_disable;

	dc30->mmio = pci_iomap(pdev, 0, DC30_MMIO_SIZE);
	if (!dc30->mmio) {
		err = -ENOMEM;
		goto err_release;
	}

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		goto err_unmap;

	err = dc30_jpeg_init(dc30);
	if (err)
		goto err_unmap;

	pci_set_master(pdev);
	dc30_apply_pci_tuning(pdev);
	mutex_init(&dc30->power_lock);
	/* ZR36057 reset, GPIOs, video bus; all interrupts masked. */
	dc30_board_init(dc30);

	err = request_irq(pdev->irq, dc30_irq_handler, IRQF_SHARED,
			   dc30->name, dc30);
	if (err)
		goto err_unmap;

	/* Not fatal: video works without the audio side. */
	if (!dc30_audio_init(dc30)) {
		err = dc30_alsa_register(dc30);
		if (err)
			dev_warn(&pdev->dev, "ALSA registration failed: %d\n",
				 err);
	}

	err = dc30_i2c_register(dc30);
	if (err)
		goto err_free_irq;

	{
		struct i2c_board_info info = {
			I2C_BOARD_INFO("dc30-vpx3220", DC30_VPX3220_I2C_ADDR),
		};

		/* A missing/unresponsive decoder doesn't fail the PCI probe -
		 * the ZR36057 core is still useful on its own for bring-up
		 * (register dump, IRQ tests) even without it.
		 */
		dc30->decoder = i2c_new_client_device(&dc30->i2c_adap, &info);
		if (IS_ERR(dc30->decoder)) {
			dev_warn(&pdev->dev,
				 "vpx3220 decoder not found at 0x%02x (%ld)\n",
				 DC30_VPX3220_I2C_ADDR, PTR_ERR(dc30->decoder));
			dc30->decoder = NULL;
		}
	}

	/* A registration failure here (rare: ENOMEM, v4l2_device_register())
	 * doesn't fail the PCI probe either, for the same reason as a
	 * missing decoder above.
	 */
	err = dc30_video_register(dc30);
	if (err)
		dev_warn(&pdev->dev, "video device registration failed: %d\n",
			 err);

	if (dc30_debugfs_dir) {
		struct dentry *card_dir;

		card_dir = debugfs_create_dir(pci_name(pdev),
					       dc30_debugfs_dir);
		if (!IS_ERR_OR_NULL(card_dir)) {
			debugfs_create_file("regs", 0444, card_dir, dc30,
					     &dc30_regs_fops);
			debugfs_create_file("decoder_status", 0444, card_dir,
					     dc30, &dc30_decoder_status_fops);
			debugfs_create_file("stats", 0444, card_dir, dc30,
					    &dc30_stats_fops);
			debugfs_create_file("ad1843", 0444, card_dir, dc30,
					    &dc30_ad1843_fops);
			debugfs_create_file("audio_stats", 0444, card_dir,
					    dc30, &dc30_audio_stats_fops);
			debugfs_create_file("encoder", 0444, card_dir, dc30,
					    &dc30_encoder_fops);
			debugfs_create_file("power", 0444, card_dir, dc30,
					    &dc30_power_state_fops);
			dc30_audio_debugfs_init(dc30, card_dir);
			dc30_jpeg_debugfs_init(dc30, card_dir);
		}
	}

	dev_info(&pdev->dev, "miro DC30 (ZR36057 rev %d) irq %d, mmio 0x%08llx\n",
		 pdev->revision, pdev->irq,
		 (unsigned long long)pci_resource_start(pdev, 0));

	/* From here on the card sleeps whenever nobody uses it. */
	dc30_pm_enable(dc30);
	return 0;

err_free_irq:
	free_irq(pdev->irq, dc30);
err_unmap:
	dc30_jpeg_exit(dc30);
	pci_iounmap(pdev, dc30->mmio);
err_release:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
	return err;
}

static void dc30_remove(struct pci_dev *pdev)
{
	struct dc30_dev *dc30 = pci_get_drvdata(pdev);

	/* Awake from here on (the PCI core resumed the device). */
	dc30_pm_disable(dc30);

	/* Detach the decoder's subdev from v4l2_dev (via vpx3220's own
	 * .remove()) before tearing v4l2_dev itself down.
	 */
	if (dc30->decoder)
		i2c_unregister_device(dc30->decoder);
	dc30_video_unregister(dc30);
	dc30_alsa_unregister(dc30);
	dc30_audio_exit(dc30);
	dc30_i2c_unregister(dc30);

	dc30_write(dc30, ZR36057_ICR, 0);
	free_irq(pdev->irq, dc30);
	dc30_board_shutdown(dc30);
	dc30_jpeg_exit(dc30);
	pci_iounmap(pdev, dc30->mmio);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id dc30_pci_tbl[] = {
	{ PCI_DEVICE(DC30_PCI_VENDOR_ID_ZORAN, DC30_PCI_DEVICE_ID_ZR36057) },
	{ }
};
MODULE_DEVICE_TABLE(pci, dc30_pci_tbl);

static struct pci_driver dc30_pci_driver = {
	.name     = "dc30",
	.id_table = dc30_pci_tbl,
	.probe    = dc30_probe,
	.remove   = dc30_remove,
	.driver.pm = pm_ptr(&dc30_pm_ops),
};

static int __init dc30_init(void)
{
	dc30_debugfs_dir = debugfs_create_dir("dc30", NULL);
	return pci_register_driver(&dc30_pci_driver);
}

static void __exit dc30_exit(void)
{
	pci_unregister_driver(&dc30_pci_driver);
	debugfs_remove_recursive(dc30_debugfs_dir);
}

module_init(dc30_init);
module_exit(dc30_exit);

/* dc30 only creates the vpx3220 i2c_client at runtime - there's no symbol
 * reference tying the two modules together for modpost to pick up, so spell
 * out the load-order dependency explicitly for modprobe/dkms.
 */
MODULE_SOFTDEP("pre: dc30_vpx3220");

MODULE_AUTHOR("bytewarrior");
MODULE_DESCRIPTION("miro DC30 video capture driver (V4L2 + ALSA)");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
