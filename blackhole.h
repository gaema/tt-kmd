// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_BLACKHOLE_H_INCLUDED
#define TTDRIVER_BLACKHOLE_H_INCLUDED

#include <linux/atomic.h>
#include <linux/types.h>
#include "device.h"

struct blackhole_device {
	struct tenstorrent_device tt;

	struct mutex kernel_tlb_mutex;	// Guards access to kernel_tlb
	u8 __iomem *tlb_regs;   // All TLB registers
	u8 __iomem *kernel_tlb; // Topmost 2M window, reserved for kernel
	u8 __iomem *noc2axi_cfg;
	u8 __iomem *bar2_mapping;

	u8 saved_mps;

	// Device-liveness latch (Wormhole parity).  Arms after N consecutive
	// kernel-TLB all-ones reads — including ARC/NoC hang while BAR0 still
	// answers.  allones_streak is only touched under kernel_tlb_mutex;
	// hung is atomic and tested before taking that mutex.
	unsigned int allones_streak;
	// First jiffies at which ARC_BOOT_STATUS read all-ones with no good read
	// since; 0 when not in an all-ones run.  The ARC-ready poll cannot use a
	// read COUNT to tell "still booting" from "dead" -- a healthy ARC reads
	// all-ones for its whole SYS_INIT (measured up to 35.9 s) and a tight
	// poll racks up thousands of reads in that window.  Duration separates
	// them.  Touched only under kernel_tlb_mutex.
	unsigned long allones_since;
	atomic_t hung;

	bool pcie_perf_group_registered;
	bool telemetry_group_registered;
	// Carries the tt_hung latch control.  Registered unconditionally and
	// kept separate from telemetry_group so the recovery handle survives a
	// failed telemetry probe on an already-fenced die.
	bool hang_group_registered;
};

#define tt_dev_to_bh_dev(ttdev) \
	container_of((tt_dev), struct blackhole_device, tt)

#endif
