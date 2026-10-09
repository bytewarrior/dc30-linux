// SPDX-License-Identifier: GPL-2.0-only
/*
 * dc30_i2c.c - bit-banged I2C bus over the ZR36057 I2CBR register
 *
 * Same two-wire scheme as the historical GPL zoran driver
 * (zoran_card.c: zoran_i2c_setsda/setscl/getsda/
 * getscl) - SDA is bit 1, SCL is bit 0 of ZR36057_I2CBR. This is the
 * GuestBus-side bit-bang used to configure the VPX3220 decoder and ADV7176
 * encoder; it is unrelated to the ZR36050's CodecBus data path.
 *
 * A shadow register value is kept because ZR36057_I2CBR only reliably
 * reflects the two output bits we just wrote - doing a read-modify-write
 * straight off hardware risks losing the other line's state between two
 * back-to-back transitions.
 */

#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/module.h>

#include "dc30.h"
#include "dc30_i2c.h"

static void dc30_i2c_setsda(void *data, int state)
{
	struct dc30_dev *dc30 = data;
	unsigned long flags;

	spin_lock_irqsave(&dc30->reg_lock, flags);
	if (state)
		dc30->i2c_bits |= ZR36057_I2CBR_SDA;
	else
		dc30->i2c_bits &= ~ZR36057_I2CBR_SDA;
	dc30_write(dc30, ZR36057_I2CBR, dc30->i2c_bits);
	spin_unlock_irqrestore(&dc30->reg_lock, flags);
}

static void dc30_i2c_setscl(void *data, int state)
{
	struct dc30_dev *dc30 = data;
	unsigned long flags;

	spin_lock_irqsave(&dc30->reg_lock, flags);
	if (state)
		dc30->i2c_bits |= ZR36057_I2CBR_SCL;
	else
		dc30->i2c_bits &= ~ZR36057_I2CBR_SCL;
	dc30_write(dc30, ZR36057_I2CBR, dc30->i2c_bits);
	spin_unlock_irqrestore(&dc30->reg_lock, flags);
}

static int dc30_i2c_getsda(void *data)
{
	struct dc30_dev *dc30 = data;

	return (dc30_read(dc30, ZR36057_I2CBR) & ZR36057_I2CBR_SDA) ? 1 : 0;
}

static int dc30_i2c_getscl(void *data)
{
	struct dc30_dev *dc30 = data;

	return (dc30_read(dc30, ZR36057_I2CBR) & ZR36057_I2CBR_SCL) ? 1 : 0;
}

int dc30_i2c_register(struct dc30_dev *dc30)
{
	dc30->i2c_algo.setsda = dc30_i2c_setsda;
	dc30->i2c_algo.setscl = dc30_i2c_setscl;
	dc30->i2c_algo.getsda = dc30_i2c_getsda;
	dc30->i2c_algo.getscl = dc30_i2c_getscl;
	dc30->i2c_algo.data = dc30;
	dc30->i2c_algo.udelay = 10;
	dc30->i2c_algo.timeout = HZ;

	dc30->i2c_adap.owner = THIS_MODULE;
	dc30->i2c_adap.algo_data = &dc30->i2c_algo;
	dc30->i2c_adap.dev.parent = &dc30->pdev->dev;
	snprintf(dc30->i2c_adap.name, sizeof(dc30->i2c_adap.name), "dc30 i2c");

	return i2c_bit_add_bus(&dc30->i2c_adap);
}

void dc30_i2c_unregister(struct dc30_dev *dc30)
{
	i2c_del_adapter(&dc30->i2c_adap);
}
