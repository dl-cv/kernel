// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/notifier.h>
#include <media/rk-camera-frame.h>

static ATOMIC_NOTIFIER_HEAD(rk_camera_frame_chain);

int rk_camera_frame_register(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&rk_camera_frame_chain, nb);
}
EXPORT_SYMBOL_GPL(rk_camera_frame_register);

int rk_camera_frame_unregister(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(&rk_camera_frame_chain, nb);
}
EXPORT_SYMBOL_GPL(rk_camera_frame_unregister);

void rk_camera_frame_notify(struct fwnode_handle *terminal, u32 mask)
{
	struct rk_camera_frame_event event = {
		.terminal = terminal,
		.mask = mask,
	};

	if (terminal)
		atomic_notifier_call_chain(&rk_camera_frame_chain, 0, &event);
}
EXPORT_SYMBOL_GPL(rk_camera_frame_notify);