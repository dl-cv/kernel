/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MEDIA_RK_CAMERA_FRAME_H
#define _MEDIA_RK_CAMERA_FRAME_H

#include <linux/bits.h>
#include <linux/notifier.h>
#include <linux/types.h>

#define RK_CAMERA_FRAME_START	BIT(0)
#define RK_CAMERA_FRAME_END	BIT(1)
#define RK_CAMERA_FRAME_ERROR	BIT(2)

#define RK_CAMERA_FRAME_EVENTS	(RK_CAMERA_FRAME_START | \
				 RK_CAMERA_FRAME_END | \
				 RK_CAMERA_FRAME_ERROR)

struct fwnode_handle;

struct rk_camera_frame_event {
	struct fwnode_handle *terminal;
	u32 mask;
};

int rk_camera_frame_register(struct notifier_block *nb);
int rk_camera_frame_unregister(struct notifier_block *nb);
void rk_camera_frame_notify(struct fwnode_handle *terminal, u32 mask);

#endif