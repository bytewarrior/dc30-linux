/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dc30_vpx3220.h - private subdev commands of dc30_vpx3220.c, used by dc30
 */

#ifndef VPX3220_H
#define VPX3220_H

#include <linux/ioctl.h>
#include <linux/videodev2.h>

/* core.ioctl: arg is a bool * - keep HREF/VREF running while the video
 * ports are off, for the AD1843's video lock (SYNC1 = VACT). Only takes
 * effect while powered.
 */
#define VPX3220_IOCTL_SYNC_OUT	_IOW('V', BASE_VIDIOC_PRIVATE + 0, bool)

#endif /* VPX3220_H */
