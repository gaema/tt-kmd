// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// gaema h2h shim -- the ONLY tt-kmd surface `tt_h2h.ko` consumes.
//
// WHY THIS EXISTS, AND WHY IT IS A SEPARATE FILE
//
// `tt-kmd` owns the PCI device and every BAR mapping, so a second module cannot
// reach the NOC without tt-kmd handing it something.  The framework plan
// (ai/tenstorrent/blackhole/p150a/plan/h2h-dma-framework.md §4a) chose an
// exported accessor over forking the driver: an entire RDMA dataplane living
// inside a rebased vendor tree is expensive to carry, a handful of exports is
// not.  Keeping them in their OWN file means a vendor rebase touches
// blackhole.c / enumerate.c exactly as it would have anyway, and this file
// either applies cleanly or fails loudly on its own.
//
// SCOPE.  Device lookup, refcounting, and 32-bit NOC read/write.  That is all
// M1 needs: CLAIM must read `ETH_TXQ*_MAX_PKT_SIZE_BYTES` and the firmware's
// own `ETH_PARAM+0x0C` word to run the wedge preflight, and RELEASE must write
// the stock cap back.  No DMA, no descriptors, no interrupts -- those stay in
// `tt_h2h` where they can be reviewed as framework code rather than as driver
// changes.
//
// 🔴 NOT A STABLE ABI.  These symbols exist for our own out-of-tree module in
// this repo pair.  They are GPL-only exports and carry no compatibility promise
// across tt-kmd versions; `tt_h2h` checks `tenstorrent_h2h_abi_version()` at
// probe and refuses to bind on a mismatch rather than calling into a driver
// whose struct layout it does not share.

#include <linux/module.h>
#include <linux/xarray.h>

#include "device.h"
#include "enumerate.h"
#include "gaema_h2h_shim.h"	// our own prototypes -- see the header's comment
#include "module.h"

unsigned int tenstorrent_h2h_abi_version(void)
{
	return TENSTORRENT_H2H_ABI_VERSION;
}
EXPORT_SYMBOL_GPL(tenstorrent_h2h_abi_version);

// Look up a device by the ordinal that names /dev/tenstorrent/<ordinal>, and
// take a reference so a concurrent PCI remove cannot free it underneath the
// caller.  Returns NULL if no such device.  Every non-NULL return MUST be
// released with tenstorrent_device_put().
struct tenstorrent_device *tenstorrent_h2h_get_device(unsigned int ordinal)
{
	struct tenstorrent_device *tt_dev;

	// The xarray lives in enumerate.c; tenstorrent_lookup_device() is the
	// accessor added there.  Going through it rather than exporting the
	// xarray keeps the container private.
	tt_dev = tenstorrent_lookup_device(ordinal);
	if (!tt_dev)
		return NULL;

	// A device mid-remove is marked detached before its mappings go away.
	// Handing one out would let tt_h2h touch dead iomem.
	if (tt_dev->detached) {
		tenstorrent_device_put(tt_dev);
		return NULL;
	}

	return tt_dev;
}
EXPORT_SYMBOL_GPL(tenstorrent_h2h_get_device);

void tenstorrent_h2h_put_device(struct tenstorrent_device *tt_dev)
{
	if (tt_dev)
		tenstorrent_device_put(tt_dev);
}
EXPORT_SYMBOL_GPL(tenstorrent_h2h_put_device);

// 32-bit NOC read.  Returns 0 on success, -EIO if the read did not complete,
// -ENODEV if this device class has no NOC read op (Wormhole today) or the
// device has detached since lookup.
//
// ⚠️ The `detached` re-check is NOT redundant with the one in get_device():
// a PCI remove can land between lookup and use, and this is the last point
// before we touch iomem.
int tenstorrent_h2h_noc_read32(struct tenstorrent_device *tt_dev, u32 x, u32 y,
			       u64 addr, int noc, u32 *value)
{
	if (!tt_dev || !value)
		return -EINVAL;
	if (tt_dev->detached)
		return -ENODEV;
	if (!tt_dev->dev_class->noc_read32)
		return -ENODEV;

	return tt_dev->dev_class->noc_read32(tt_dev, x, y, addr, noc, value);
}
EXPORT_SYMBOL_GPL(tenstorrent_h2h_noc_read32);

// 32-bit NOC write.  Returns 0 on success.
//
// 🔴 The class's own .noc_write32 is `void` -- it swallows the completion
// status upstream.  We keep that behaviour rather than change a vendor
// signature, so a 0 here means "issued", not "observed".  tt_h2h's register
// restore therefore READS BACK and compares; do not treat this return as
// confirmation that the write landed.
int tenstorrent_h2h_noc_write32(struct tenstorrent_device *tt_dev, u32 x, u32 y,
				u64 addr, int noc, u32 value)
{
	if (!tt_dev)
		return -EINVAL;
	if (tt_dev->detached)
		return -ENODEV;
	if (!tt_dev->dev_class->noc_write32)
		return -ENODEV;

	tt_dev->dev_class->noc_write32(tt_dev, x, y, addr, value, noc);
	return 0;
}
EXPORT_SYMBOL_GPL(tenstorrent_h2h_noc_write32);
