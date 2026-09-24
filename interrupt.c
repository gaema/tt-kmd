// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "interrupt.h"

#include <linux/pci.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/eventfd.h>
#include <linux/err.h>

#include "device.h"
#include "enumerate.h"
#include "ioctl.h"

static irqreturn_t irq_handler(int irq, void *device)
{
	struct tenstorrent_device *tt_dev = device;
	unsigned long flags;

	atomic64_inc(&tt_dev->irq_count);

	spin_lock_irqsave(&tt_dev->msi_lock, flags);
	if (tt_dev->msi_eventfd)
		eventfd_signal(tt_dev->msi_eventfd);
	spin_unlock_irqrestore(&tt_dev->msi_lock, flags);

	return IRQ_HANDLED;
}

bool tenstorrent_enable_interrupts(struct tenstorrent_device *tt_dev)
{
	if (pci_alloc_irq_vectors(tt_dev->pdev, 1, 1, PCI_IRQ_ALL_TYPES) <= 0)
		goto out_pci_alloc_irq_vectors_failed;

	if (request_irq(pci_irq_vector(tt_dev->pdev, 0), irq_handler,
			IRQF_SHARED, TENSTORRENT, tt_dev) != 0)
		goto out_request_irq_failed;

	tt_dev->interrupt_enabled = true;
	return true;

out_request_irq_failed:
	pci_free_irq_vectors(tt_dev->pdev);
out_pci_alloc_irq_vectors_failed:
	return false;
}

// Swap in `ctx` as the device's eventfd (NULL clears) and return the old one,
// which the caller must eventfd_ctx_put() outside the spinlock.
static struct eventfd_ctx *swap_msi_eventfd(struct tenstorrent_device *tt_dev,
					    struct eventfd_ctx *ctx, void *owner)
{
	struct eventfd_ctx *old;
	unsigned long flags;

	spin_lock_irqsave(&tt_dev->msi_lock, flags);
	old = tt_dev->msi_eventfd;
	tt_dev->msi_eventfd = ctx;
	tt_dev->msi_owner = ctx ? owner : NULL;
	spin_unlock_irqrestore(&tt_dev->msi_lock, flags);

	return old;
}

void tenstorrent_disable_interrupts(struct tenstorrent_device *tt_dev)
{
	struct eventfd_ctx *old;

	if (tt_dev->interrupt_enabled) {
		free_irq(pci_irq_vector(tt_dev->pdev, 0), tt_dev);
		pci_free_irq_vectors(tt_dev->pdev);
		tt_dev->interrupt_enabled = false;
	}

	old = swap_msi_eventfd(tt_dev, NULL, NULL);
	if (old)
		eventfd_ctx_put(old);
}

int tenstorrent_set_msi_eventfd(struct tenstorrent_device *tt_dev, void *owner, int fd)
{
	struct eventfd_ctx *ctx = NULL;
	struct eventfd_ctx *old;
	unsigned long flags;
	bool busy;

	if (!tt_dev->interrupt_enabled)
		return -ENODEV;

	if (fd >= 0) {
		ctx = eventfd_ctx_fdget(fd);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);
	}

	spin_lock_irqsave(&tt_dev->msi_lock, flags);
	busy = tt_dev->msi_owner && tt_dev->msi_owner != owner;
	spin_unlock_irqrestore(&tt_dev->msi_lock, flags);
	if (busy) {
		if (ctx)
			eventfd_ctx_put(ctx);
		return -EBUSY;
	}

	old = swap_msi_eventfd(tt_dev, ctx, owner);
	if (old)
		eventfd_ctx_put(old);
	return 0;
}

void tenstorrent_release_msi_eventfd(struct tenstorrent_device *tt_dev, void *owner)
{
	struct eventfd_ctx *old = NULL;
	unsigned long flags;

	spin_lock_irqsave(&tt_dev->msi_lock, flags);
	if (tt_dev->msi_owner == owner) {
		old = tt_dev->msi_eventfd;
		tt_dev->msi_eventfd = NULL;
		tt_dev->msi_owner = NULL;
	}
	spin_unlock_irqrestore(&tt_dev->msi_lock, flags);

	if (old)
		eventfd_ctx_put(old);
}

void tenstorrent_msi_info(struct tenstorrent_device *tt_dev, u32 *irq_type,
			  u64 *msi_address, u32 *msi_data)
{
	struct pci_dev *pdev = tt_dev->pdev;
	u32 lo = 0, hi = 0;
	u16 ctrl = 0, data = 0;
	int pos = pdev->msi_cap;

	*msi_address = 0;
	*msi_data = 0;

	if (!tt_dev->interrupt_enabled) {
		*irq_type = 0;
	} else if (pdev->msix_enabled) {
		*irq_type = 3;
	} else if (pdev->msi_enabled && pos) {
		*irq_type = 2;
		pci_read_config_word(pdev, pos + PCI_MSI_FLAGS, &ctrl);
		pci_read_config_dword(pdev, pos + PCI_MSI_ADDRESS_LO, &lo);
		if (ctrl & PCI_MSI_FLAGS_64BIT) {
			pci_read_config_dword(pdev, pos + PCI_MSI_ADDRESS_HI, &hi);
			pci_read_config_word(pdev, pos + PCI_MSI_DATA_64, &data);
		} else {
			pci_read_config_word(pdev, pos + PCI_MSI_DATA_32, &data);
		}
		*msi_address = ((u64)hi << 32) | lo;
		*msi_data = data;
	} else {
		*irq_type = 1;
	}
}
