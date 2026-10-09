/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zr36050.h - ZR36050 JPEG image compression processor, set up for
 * motion JPEG compression the way dc30.sys does it.
 */

#ifndef DC30_ZR36050_H
#define DC30_ZR36050_H

#include <linux/types.h>

struct dc30_dev;
struct seq_file;

/* Internal memory: control registers */
#define ZR36050_GO		0x000
#define ZR36050_HARDWARE	0x002
#define ZR36050_HW_MSTR			(1 << 6)
#define ZR36050_MODE		0x003
#define ZR36050_MODE_COMP		(1 << 7)
#define ZR36050_MODE_ATP		(1 << 6)
#define ZR36050_MODE_PASS2		(1 << 5)
#define ZR36050_MODE_TLM		(1 << 4)
#define ZR36050_MODE_DCONLY		(1 << 3)
#define ZR36050_MODE_BRC		(1 << 2)
#define ZR36050_OPTIONS		0x004
#define ZR36050_MBCV		0x005
#define ZR36050_MARKERS_EN	0x006
#define ZR36050_ME_APP			(1 << 7)
#define ZR36050_ME_COM			(1 << 6)
#define ZR36050_ME_DRI			(1 << 5)
#define ZR36050_ME_DQT			(1 << 4)
#define ZR36050_ME_DHT			(1 << 3)
#define ZR36050_ME_DNL			(1 << 2)
#define ZR36050_ME_DQTI			(1 << 1)
#define ZR36050_ME_DHTI			(1 << 0)
#define ZR36050_INT_REQ_0	0x007
#define ZR36050_INT_REQ_1	0x008
#define ZR36050_TCV_NET		0x009	/* 4 bytes, MSB first */
#define ZR36050_TCV_DATA	0x00d	/* 4 bytes */
#define ZR36050_SF		0x011	/* 2 bytes, 8.8 fixed point */
#define ZR36050_AF		0x013	/* 3 bytes */
#define ZR36050_ACV		0x016	/* 4 bytes, accumulated code volume */
#define ZR36050_ACT		0x01a	/* 4 bytes, accumulated activity */
#define ZR36050_ACV_TRUN	0x01e	/* 4 bytes */
#define ZR36050_STATUS_0	0x02e
#define ZR36050_STATUS_1	0x02f
#define ZR36050_ST1_DATRDY		(1 << 7)
#define ZR36050_ST1_MRKDET		(1 << 6)
#define ZR36050_ST1_RFM			(1 << 4)
#define ZR36050_ST1_RFD			(1 << 3)
#define ZR36050_ST1_END			(1 << 2)
#define ZR36050_ST1_TCVOVF		(1 << 1)
#define ZR36050_ST1_DATOVF		(1 << 0)

/* Internal memory: marker segments (dc30.sys addresses) */
#define ZR36050_SOF_ADDR	0x040
#define ZR36050_SOS_ADDR	0x07a
#define ZR36050_DRI_ADDR	0x0c0
#define ZR36050_DQT_ADDR	0x0cc
#define ZR36050_DHT_ADDR	0x1d4
#define ZR36050_APP_ADDR	0x380
#define ZR36050_COM_ADDR	0x3c0

struct zr36050_params {
	unsigned int width;		/* frame width, pixels */
	unsigned int field_height;	/* lines per field */
	unsigned int field_bytes;	/* code volume target per field */
	bool two_fields;		/* both fields in one code buffer */
};

int zr36050_read(struct dc30_dev *dc30, unsigned int addr, u8 *val);
int zr36050_write(struct dc30_dev *dc30, unsigned int addr, u8 val);

/* Reset, load the tables and program the compression pass (dc30.sys
 * _CoderCfg). Needs the ZR36050 clock running. Sleeps.
 */
int zr36050_configure(struct dc30_dev *dc30, const struct zr36050_params *p);

/* GO for the compression pass (dc30.sys _CoderStart). */
int zr36050_go(struct dc30_dev *dc30);


/* debugfs: control registers and a check of the marker memory. */
void zr36050_show(struct seq_file *m, struct dc30_dev *dc30,
		  const struct zr36050_params *p);

#endif /* DC30_ZR36050_H */
