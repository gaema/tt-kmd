/* SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gaema h2h shim -- the tt-kmd surface `tt_h2h.ko` consumes.
 *
 * ONE header, TWO consumers, deliberately: gaema_h2h_shim.c includes it so the
 * compiler checks the definitions against the declarations (without it every
 * export is a -Wmissing-prototypes warning and a signature typo would only
 * surface as a link failure in the OTHER module), and tt_h2h includes the very
 * same file so both sides cannot drift.  Do not fork a private copy into
 * tt_h2h -- a copied header that silently disagrees is exactly the failure the
 * ABI version below exists to catch, and catching it at compile time is
 * cheaper than at insmod.
 */

#ifndef TTDRIVER_GAEMA_H2H_SHIM_H_INCLUDED
#define TTDRIVER_GAEMA_H2H_SHIM_H_INCLUDED

#include <linux/types.h>

struct tenstorrent_device;

/* Bumped on ANY incompatible change below.  tt_h2h compares this compile-time
 * constant against the running driver's tenstorrent_h2h_abi_version() and
 * refuses to bind on a mismatch.
 *
 * v2 (M2): + tenstorrent_h2h_dma_dev().  Adding a symbol is compatible in the
 * old-tt_h2h-on-new-tt-kmd direction and NOT in the other, which is the
 * direction that bites: a v2 tt_h2h against a resident v1 tenstorrent.ko fails
 * at insmod on an unknown symbol.  Bumping makes that a named ABI refusal
 * instead.  Rebuild AND reload tenstorrent.ko when this moves. */
#define TENSTORRENT_H2H_ABI_VERSION 2u

unsigned int tenstorrent_h2h_abi_version(void);

/* Refcounted lookup by the ordinal naming /dev/tenstorrent/<ordinal>.
 * NULL if absent or detached; otherwise release with put_device(). */
struct tenstorrent_device *tenstorrent_h2h_get_device(unsigned int ordinal);
void tenstorrent_h2h_put_device(struct tenstorrent_device *tt_dev);

/* The device DMA mappings must be created against -- i.e. &tt_dev->pdev->dev.
 * NULL if the device has detached.
 *
 * 🔴 WHY THIS IS HERE AND NOT IN tt_h2h.  An MR's IOVA is not a property of the
 * pages; it is a property of the DMA path the CARD will use to reach them, so
 * it can only be produced by dma_map against the card's own struct device.
 * tt_h2h has no other way to name that device, and the alternative -- treating
 * page_to_phys() as an IOVA -- is silently WRONG the moment an IOMMU is
 * translating, which is exactly the class of bug that is invisible in testing
 * on a passthrough host and corrupts memory on a translating one.  Two lines
 * here beat a masked bug there.
 *
 * The reference belongs to tt_dev: the caller must hold its
 * tenstorrent_h2h_get_device() reference for as long as it uses this pointer,
 * and must not put_device() it separately. */
struct device *tenstorrent_h2h_dma_dev(struct tenstorrent_device *tt_dev);

/* 32-bit NOC accessors.  0 on success, -ENODEV if the class has no op or the
 * device detached, -EINVAL on a bad argument, -EIO on an incomplete read.
 *
 * 🔴 write32 returning 0 means ISSUED, not OBSERVED -- the vendor class op is
 * `void` and swallows completion status.  Read back and compare if it matters. */
int tenstorrent_h2h_noc_read32(struct tenstorrent_device *tt_dev, u32 x, u32 y,
			       u64 addr, int noc, u32 *value);
int tenstorrent_h2h_noc_write32(struct tenstorrent_device *tt_dev, u32 x, u32 y,
				u64 addr, int noc, u32 value);

#endif /* TTDRIVER_GAEMA_H2H_SHIM_H_INCLUDED */
