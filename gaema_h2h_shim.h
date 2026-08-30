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
 * refuses to bind on a mismatch. */
#define TENSTORRENT_H2H_ABI_VERSION 1u

unsigned int tenstorrent_h2h_abi_version(void);

/* Refcounted lookup by the ordinal naming /dev/tenstorrent/<ordinal>.
 * NULL if absent or detached; otherwise release with put_device(). */
struct tenstorrent_device *tenstorrent_h2h_get_device(unsigned int ordinal);
void tenstorrent_h2h_put_device(struct tenstorrent_device *tt_dev);

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
