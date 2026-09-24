// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_INTERRUPT_H_INCLUDED
#define TTDRIVER_INTERRUPT_H_INCLUDED

#include <linux/types.h>

struct tenstorrent_device;

bool tenstorrent_enable_interrupts(struct tenstorrent_device *tt_dev);
void tenstorrent_disable_interrupts(struct tenstorrent_device *tt_dev);

// TENSTORRENT_IOCTL_SET_MSI_EVENTFD support. `owner` is the registering fd's
// struct chardev_private; fd < 0 unregisters.
int tenstorrent_set_msi_eventfd(struct tenstorrent_device *tt_dev, void *owner, int fd);
void tenstorrent_release_msi_eventfd(struct tenstorrent_device *tt_dev, void *owner);
void tenstorrent_msi_info(struct tenstorrent_device *tt_dev, u32 *irq_type,
			  u64 *msi_address, u32 *msi_data);

#endif
