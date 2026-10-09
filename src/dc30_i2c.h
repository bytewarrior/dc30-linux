/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dc30_i2c.h - bit-banged I2C adapter over the ZR36057 GuestBus I2CBR
 * register (dc30_i2c.c)
 */

#ifndef DC30_I2C_H
#define DC30_I2C_H

struct dc30_dev;

int dc30_i2c_register(struct dc30_dev *dc30);
void dc30_i2c_unregister(struct dc30_dev *dc30);

#endif /* DC30_I2C_H */
