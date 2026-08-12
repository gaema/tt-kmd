// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/bitfield.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/delay.h>

#include "blackhole.h"
#include "enumerate.h"	// PCI_VENDOR_ID_TENSTORRENT
#include "pcie.h"
#include "module.h"
#include "msgqueue.h"
#include "tlb.h"
#include "telemetry.h"

#define MAX_MRRS 4096

#define TLB_2M_WINDOW_COUNT 202
#define TLB_2M_SHIFT 21
#define TLB_2M_WINDOW_SIZE (1 << TLB_2M_SHIFT)
#define TLB_2M_WINDOW_MASK (TLB_2M_WINDOW_SIZE - 1)

#define TLB_4G_WINDOW_COUNT 8	// NB: Not all are guaranteed to be exposed in BAR4; see blackhole_init().
#define TLB_4G_SHIFT 32
#define TLB_4G_WINDOW_SIZE (1UL << TLB_4G_SHIFT)
#define TLB_4G_WINDOW_MASK (TLB_4G_WINDOW_SIZE - 1)

#define TLB_REG_SIZE 12	// Same for 2M and 4G
#define TLB_TOTAL_WINDOW_COUNT (TLB_2M_WINDOW_COUNT + TLB_4G_WINDOW_COUNT)

#define TLB_REGS_START 0x1FC00000   // BAR0
#define TLB_REGS_LEN 0x00001000     // Covers all TLB registers

#define TLB_STRIDED_COUNT 32	// First 32 2M windows support non-rectangular multicast patterns
#define TLB_STRIDED_REG_SIZE 4
#define TLB_STRIDED_REGS_OFFSET (TLB_TOTAL_WINDOW_COUNT * TLB_REG_SIZE)

#define KERNEL_TLB_INDEX (TLB_2M_WINDOW_COUNT - 1)	// Last 2M window is ours
#define KERNEL_TLB_START (KERNEL_TLB_INDEX * TLB_2M_WINDOW_SIZE)
#define KERNEL_TLB_LEN TLB_2M_WINDOW_SIZE

#define NOC2AXI_CFG_START 0x1FD00000
#define NOC2AXI_CFG_LEN 0x00100000
#define NOC_ID_OFFSET 0x4044
#define NOC_STATUS_OFFSET 0x4200
#define NOC1_NOC2AXI_OFFSET 0x10000

// this points to outbound NOC_TLB_62 configured by CMFW
#define PCIE_DBI_ADDR 0xF800000000000000ULL

// PCI subsystem device IDs for Blackhole cards
#define PCI_SUBSYSTEM_DEVICE_GALAXY	0x0047

// ARC owns telemetry
#define ARC_X 8
#define ARC_Y 0
#define RESET_SCRATCH(N) (0x80030400 + ((N) * 4))
#define ARC_TELEMETRY_PTR RESET_SCRATCH(13)
#define ARC_TELEMETRY_DATA RESET_SCRATCH(12)

// ARC FW has a messaging interface, see msgqueue.c in tt-zephyr-platforms.git
#define ARC_MSG_QCB_PTR RESET_SCRATCH(11) // Message Queue Control Block
#define ARC_MSI_FIFO 0x800B0000           // Write 0 to trigger the ARC message queue processor
#define ARC_MSG_READY_MS 500              // Wait this long for ARC to be ready for message queue operations
#define ARC_MSG_TYPE_ASIC_STATE0 0xA0
#define ARC_MSG_TYPE_ASIC_STATE3 0xA3
#define ARC_MSG_TYPE_SET_WDT_TIMEOUT 0xC1
#define ARC_MSG_TYPE_TRIGGER_RESET 0x56
#define ARC_MSG_TYPE_POWER_SETTING 0x21
#define ARC_MSG_TYPE_TEST 0x90
// Re-initialize the Tensix grid + NoC (ClearNocTranslation + NocInit +
// TensixInit in CMFW).  Reachable over the ARC message queue only while the
// ARC + NoC are alive.  See tt-zephyr-platforms reset.c:ReinitTensix.
#define ARC_MSG_TYPE_REINIT_TENSIX 0x20
#define ARC_BOOT_STATUS RESET_SCRATCH(2)
#define ARC_BOOT_STATUS_READY_FOR_MSG 0x1

#define IATU_BASE 0x1000	// Relative to the start of BAR2
#define IATU_OUTBOUND 0
#define IATU_OUTBOUND_REGIONS 16
#define IATU_REGION_STRIDE 0x100
#define IATU_REGION_CTRL_1_OUTBOUND 0x00
#define IATU_REGION_CTRL_2_OUTBOUND 0x04
#define IATU_LOWER_BASE_ADDR_OUTBOUND 0x08
#define IATU_UPPER_BASE_ADDR_OUTBOUND 0x0C
#define IATU_LOWER_LIMIT_ADDR_OUTBOUND 0x10
#define IATU_LOWER_TARGET_ADDR_OUTBOUND 0x14
#define IATU_UPPER_TARGET_ADDR_OUTBOUND 0x18
#define IATU_REGION_CTRL_3_OUTBOUND 0x1C
#define IATU_UPPER_LIMIT_ADDR_OUTBOUND 0x20

// IATU_REGION_CTRL_1_OUTBOUND fields
#define INCREASE_REGION_SIZE (1 << 13)

// IATU_REGION_CTRL_2_OUTBOUND fields
#define REGION_EN (1 << 31)

#ifndef SZ_1T
#define SZ_1T 0x10000000000ULL
#endif

#define WRITE_IATU_REG(wh_dev, direction, region, reg, value) \
	write_iatu_reg(wh_dev, IATU_##direction, region, \
		       IATU_##reg##_##direction, (value))

static void write_iatu_reg(struct blackhole_device *bh_dev, unsigned direction,
			   unsigned region, unsigned reg, u32 value) {
	u32 offset = IATU_BASE + (2 * region + direction) * IATU_REGION_STRIDE
		   + reg;

	iowrite32(value, bh_dev->bar2_mapping + offset);
}

struct TLB_2M_REG {
	union {
		struct {
			u32 low32;
			u32 mid32;
			u32 high32;
		};
		// packed to make y_start straddle mid32 and high32
		struct __attribute__((packed)) {
			u64 address : 43;
			u64 x_end : 6;
			u64 y_end : 6;
			u64 x_start : 6;
			u64 y_start : 6;
			u64 noc : 2;
			u64 multicast : 1;
			u64 ordering : 2;
			u64 linked : 1;
			u64 use_static_vc : 1;
			u64 stream_header : 1;
			u64 static_vc : 3;
			u64 reserved : 18;
		};
	};
};
static_assert(sizeof(struct TLB_2M_REG) == TLB_REG_SIZE, "TLB_2M_REG size mismatch");

struct TLB_4G_REG {
	union {
		struct {
			u32 low32;
			u32 mid32;
			u32 high32;
		};
		struct __attribute__((packed)) {
			u32 address : 32;
			u32 x_end : 6;
			u32 y_end : 6;
			u32 x_start : 6;
			u32 y_start : 6;
			u32 noc : 2;
			u32 multicast : 1;
			u32 ordering : 2;
			u32 linked : 1;
			u32 use_static_vc : 1;
			u32 stream_header : 1;
			u32 static_vc : 3;
			u32 reserved : 29;
		};
	};
};
static_assert(sizeof(struct TLB_4G_REG) == TLB_REG_SIZE, "TLB_4G_REG size mismatch");

static int blackhole_configure_tlb_2M(struct blackhole_device *bh, int tlb,
				      struct tenstorrent_noc_tlb_config *config)
{
	u8 __iomem *regs = bh->tlb_regs + (tlb * TLB_REG_SIZE);
	struct TLB_2M_REG reg = { 0 };

	// Not possible to program a 2M window that doesn't start on a 2M boundary.
	if (config->addr & TLB_2M_WINDOW_MASK)
		return -EINVAL;

	reg.address = config->addr >> TLB_2M_SHIFT;
	reg.x_end = config->x_end;
	reg.y_end = config->y_end;
	reg.x_start = config->x_start;
	reg.y_start = config->y_start;
	reg.noc = config->noc;
	reg.multicast = config->mcast;
	reg.ordering = config->ordering;
	reg.linked = config->linked;
	reg.use_static_vc = config->static_vc;

	iowrite32(reg.low32, regs + 0);
	iowrite32(reg.mid32, regs + 4);
	iowrite32(reg.high32, regs + 8);

	// Strided TLB configuration is unsupported by the CONFIGURE_TLB API.
	// Write zero to clear any strided configuration set by alternate means.
	if (tlb < TLB_STRIDED_COUNT) {
		u8 __iomem *strided_reg = bh->tlb_regs + TLB_STRIDED_REGS_OFFSET + (tlb * TLB_STRIDED_REG_SIZE);
		iowrite32(0, strided_reg);
	}

	return 0;
}

static int blackhole_configure_tlb_4G(struct blackhole_device *bh, int tlb,
				      struct tenstorrent_noc_tlb_config *config)
{
	u8 __iomem *regs = bh->tlb_regs + (tlb * TLB_REG_SIZE);
	struct TLB_4G_REG reg = { 0 };

	// Not possible to program a 4G window that doesn't start on a 4G boundary.
	if (config->addr & TLB_4G_WINDOW_MASK)
		return -EINVAL;

	reg.address = config->addr >> TLB_4G_SHIFT;
	reg.x_end = config->x_end;
	reg.y_end = config->y_end;
	reg.x_start = config->x_start;
	reg.y_start = config->y_start;
	reg.noc = config->noc;
	reg.multicast = config->mcast;
	reg.ordering = config->ordering;
	reg.linked = config->linked;
	reg.use_static_vc = config->static_vc;

	iowrite32(reg.low32, regs + 0);
	iowrite32(reg.mid32, regs + 4);
	iowrite32(reg.high32, regs + 8);

	return 0;
}

static u8 __iomem *bh_configure_kernel_tlb(struct blackhole_device *bh, u32 x, u32 y, u64 addr, int noc)
{
	struct tenstorrent_noc_tlb_config config = { 0 };
	u64 offset = addr & TLB_2M_WINDOW_MASK;

	config.addr = addr & ~TLB_2M_WINDOW_MASK;
	config.x_end = x;
	config.y_end = y;
	config.ordering	= 1; // strict
	config.noc = noc;

	blackhole_configure_tlb_2M(bh, KERNEL_TLB_INDEX, &config);
	return bh->kernel_tlb + offset;
}

// Independent liveness probe.  Mirrors is_hardware_hung() (wormhole.c:129-138):
// a PCI config read of the vendor ID does not use the NoC/TLB path that is
// suspected dead, and it is the same probe tenstorrent_pci_remove() already
// uses (enumerate.c:430-431) to decide whether the device is still there.
//
// Divergence from Wormhole, deliberate: WH's second half reads SCRATCH_REG(6)
// through a directly-mapped BAR4 window (reset_unit_regs).  Blackhole has no
// equivalent bare-BAR scratch register -- ARC scratch space is reachable only
// through the kernel TLB, i.e. exactly the path being gated -- so the
// substitute is NOC2AXI's NOC_ID, which blackhole_detect_pcie_noc_x() already
// reads straight out of BAR0 with no TLB reprogram and no mutex.
// Classify *why* the card looks dead.  Used only for logging after the latch
// has already decided to arm — never as a veto (see bh_noc_read32).
//
// - Endpoint gone: PCI config vendor-ID fails / not Tenstorrent, or BAR0
//   NOC_ID is all-ones (link/BAR dead).  Wormhole-shaped probe.
// - Path dead, endpoint alive: TLB/NoC returns all-ones while BAR0 still
//   answers.  That is the aipro ARC/NoC hang class; the first land of this
//   latch incorrectly required the endpoint probe to agree, so it never
//   armed and hwmon kept issuing TLB MMIO forever.
static bool bh_endpoint_gone(struct blackhole_device *bh)
{
	struct pci_dev *pdev = bh->tt.pdev;
	u16 vendor_id;

	if (pdev != NULL && (pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor_id) != PCIBIOS_SUCCESSFUL ||
			     vendor_id != PCI_VENDOR_ID_TENSTORRENT))
		return true;

	return (ioread32(bh->noc2axi_cfg + NOC_ID_OFFSET) == 0xFFFFFFFFu);
}

static bool bh_hung(struct blackhole_device *bh)
{
	return atomic_read(&bh->hung) != 0;
}

// Arm the latch.  Safe to call from any context that has already decided the
// device is not answering; idempotent.  Logs once per transition to hung.
static void bh_set_hung(struct blackhole_device *bh, const char *why, unsigned int streak)
{
	if (atomic_xchg(&bh->hung, 1) != 0)
		return;

	if (streak)
		dev_err(&bh->tt.pdev->dev,
			"device stopped answering (%u consecutive all-ones kernel-TLB reads, %s); "
			"refusing further MMIO. Reset the device to clear.\n",
			streak, why);
	else
		dev_err(&bh->tt.pdev->dev,
			"device stopped answering (%s); refusing further MMIO. "
			"Reset the device to clear.\n", why);
}

// Clear the latch.  Called only from the paths that are *allowed* to talk to a
// possibly-dead device -- reset and (re-)init -- because recovery has to be
// able to issue MMIO or the latch would make the device unrecoverable without
// a driver rebind.
static void bh_clear_hung(struct blackhole_device *bh)
{
	mutex_lock(&bh->kernel_tlb_mutex);
	bh->allones_streak = 0;
	mutex_unlock(&bh->kernel_tlb_mutex);

	atomic_set(&bh->hung, 0);
}

// Kernel-TLB read with an error channel.  Returns 0 and stores *value on
// success; -EIO if the device is latched hung (in which case NO MMIO is
// issued at all, and the kernel TLB mutex is not even taken); -EBUSY if
// nowait was requested and another NoC user holds the mutex.
//
// bh_hung_threshold consecutive 0xFFFFFFFF results on the kernel-TLB path
// latch the device.  Threshold 0 disables the latch.  Confirmation against
// BAR0/config is classification-only (log string), never a veto: an ARC/NoC
// hang with a live endpoint still produces TLB all-ones and must arm.
static int bh_noc_read32(struct blackhole_device *bh, u32 x, u32 y, u64 addr, int noc, u32 *value, bool nowait)
{
	u32 val;
	u8 __iomem *tlb_window;
	unsigned int streak = 0;
	bool latch = false;

	if (bh_hung(bh))
		return -EIO;

	if (nowait) {
		if (!mutex_trylock(&bh->kernel_tlb_mutex))
			return -EBUSY;
	} else {
		mutex_lock(&bh->kernel_tlb_mutex);
	}

	tlb_window = bh_configure_kernel_tlb(bh, x, y, addr, noc);
	val = ioread32(tlb_window);

	if (val != 0xFFFFFFFFu) {
		bh->allones_streak = 0;
	} else if (bh_hung_threshold != 0) {
		streak = ++bh->allones_streak;
		if (streak >= bh_hung_threshold)
			latch = true;
	}

	mutex_unlock(&bh->kernel_tlb_mutex);

	if (latch) {
		// Probe OUTSIDE the TLB mutex (must not convoy other NoC users
		// behind a possibly-stalling BAR0 read).
		const char *why = bh_endpoint_gone(bh) ?
			"endpoint/BAR dead" :
			"TLB/NoC path dead (endpoint still answers BAR0)";
		bh_set_hung(bh, why, streak);
		return -EIO;
	}

	*value = val;
	return 0;
}

static int noc_read32(struct blackhole_device *bh, u32 x, u32 y, u64 addr, int noc, u32 *value)
{
	return bh_noc_read32(bh, x, y, addr, noc, value, false);
}

// Telemetry-path variant: never queue behind another NoC user.  See the
// comment on blackhole_read_telemetry_tag().
static int noc_read32_nowait(struct blackhole_device *bh, u32 x, u32 y, u64 addr, int noc, u32 *value)
{
	return bh_noc_read32(bh, x, y, addr, noc, value, true);
}

// Returns 0 on success, -EIO if the device is latched hung (no MMIO issued).
static int noc_write32(struct blackhole_device *bh, u32 x, u32 y, u64 addr, u32 data, int noc)
{
	u8 __iomem *tlb_window;

	if (bh_hung(bh))
		return -EIO;

	mutex_lock(&bh->kernel_tlb_mutex);

	tlb_window = bh_configure_kernel_tlb(bh, x, y, addr, noc);
	iowrite32(data, tlb_window);

	mutex_unlock(&bh->kernel_tlb_mutex);
	return 0;
}

static int csm_read32(struct blackhole_device *bh, u64 addr, u32 *value)
{
	if (!is_range_within_csm(addr, sizeof(u32)))
		return -EINVAL;

	return noc_read32(bh, ARC_X, ARC_Y, addr, 0, value);
}

static int csm_read32_nowait(struct blackhole_device *bh, u64 addr, u32 *value)
{
	if (!is_range_within_csm(addr, sizeof(u32)))
		return -EINVAL;

	return noc_read32_nowait(bh, ARC_X, ARC_Y, addr, 0, value);
}

static int csm_write32(struct blackhole_device *bh, u64 addr, u32 value)
{
	if (!is_range_within_csm(addr, sizeof(u32)))
		return -EINVAL;

	// Propagate hung as -EIO so arc_msg_push/pop (which already check
	// csm_write32 != 0) do not believe a queue pointer advanced on a
	// dead card.
	return noc_write32(bh, ARC_X, ARC_Y, addr, value, 0);
}

static int blackhole_csm_read32(struct tenstorrent_device *tt_dev, u64 addr, u32 *value)
{
	return csm_read32(tt_dev_to_bh_dev(tt_dev), addr, value);
}

static int blackhole_csm_write32(struct tenstorrent_device *tt_dev, u64 addr, u32 value)
{
	return csm_write32(tt_dev_to_bh_dev(tt_dev), addr, value);
}

// BH has two PCIE instances, the function reads NOC ID to find out which one is active
static bool blackhole_detect_pcie_noc_x(struct blackhole_device *bh, u32 *noc_x) {
	if (bh_hung(bh))
		return false;

	*noc_x = ioread32(bh->noc2axi_cfg + NOC_ID_OFFSET) & 0x3F;
	return (*noc_x == 2 || *noc_x == 11);
}

static void blackhole_save_reset_state(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 x;
	u32 y = 0;
	u32 device_control;

	if (!blackhole_detect_pcie_noc_x(bh, &x))
		return;

	if (noc_read32(bh, x, y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, 0, &device_control) != 0)
		return;

	bh->saved_mps = FIELD_GET(PCI_EXP_DEVCTL_PAYLOAD, device_control);
}

static void blackhole_restore_reset_state(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 x;
	u32 y = 0;
	u32 device_control;

	if (!blackhole_detect_pcie_noc_x(bh, &x))
		return;

	if (noc_read32(bh, x, y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, 0, &device_control) != 0)
		return;

	device_control &= ~PCI_EXP_DEVCTL_PAYLOAD;
	device_control |= FIELD_PREP(PCI_EXP_DEVCTL_PAYLOAD, bh->saved_mps);
	(void)noc_write32(bh, x, y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, device_control, 0);
}

static ssize_t bh_show_pcie_single_counter(struct device *dev, char *buf, u32 counter_offset, int noc)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u64 offset = NOC_STATUS_OFFSET + (4 * counter_offset) + (noc * NOC1_NOC2AXI_OFFSET);
	u32 value;

	// Bare BAR0 path — was not covered by the TLB latch.  Do not poke a
	// card we have already given up on (or that PCI recovery detached).
	if (tt_dev->detached || bh_hung(bh))
		return -EIO;

	value = ioread32(bh->noc2axi_cfg + offset);
	return scnprintf(buf, PAGE_SIZE, "%u\n", value);
}

// The pair are for NOC0 and NOC1; counters for each NOC exposed separately.
#define BH_PCIE_COUNTER_ATTR_RO_PAIR(_base_name_str, _counter_type_id_const) \
static ssize_t _base_name_str##0_show(struct device *dev, struct device_attribute *attr, char *buf) \
{ \
	return bh_show_pcie_single_counter(dev, buf, _counter_type_id_const, 0); \
} \
static ssize_t _base_name_str##1_show(struct device *dev, struct device_attribute *attr, char *buf) \
{ \
	return bh_show_pcie_single_counter(dev, buf, _counter_type_id_const, 1); \
} \
static DEVICE_ATTR_RO(_base_name_str##0); \
static DEVICE_ATTR_RO(_base_name_str##1)

#define SLV_POSTED_WR_DATA_WORD_RECEIVED 0x39
#define SLV_NONPOSTED_WR_DATA_WORD_RECEIVED 0x38
#define SLV_RD_DATA_WORD_SENT 0x33
#define MST_POSTED_WR_DATA_WORD_SENT 0x9
#define MST_NONPOSTED_WR_DATA_WORD_SENT 0x8
#define MST_RD_DATA_WORD_RECEIVED 0x3

BH_PCIE_COUNTER_ATTR_RO_PAIR(slv_posted_wr_data_word_received, SLV_POSTED_WR_DATA_WORD_RECEIVED);
BH_PCIE_COUNTER_ATTR_RO_PAIR(slv_nonposted_wr_data_word_received, SLV_NONPOSTED_WR_DATA_WORD_RECEIVED);
BH_PCIE_COUNTER_ATTR_RO_PAIR(slv_rd_data_word_sent, SLV_RD_DATA_WORD_SENT);
BH_PCIE_COUNTER_ATTR_RO_PAIR(mst_posted_wr_data_word_sent, MST_POSTED_WR_DATA_WORD_SENT);
BH_PCIE_COUNTER_ATTR_RO_PAIR(mst_nonposted_wr_data_word_sent, MST_NONPOSTED_WR_DATA_WORD_SENT);
BH_PCIE_COUNTER_ATTR_RO_PAIR(mst_rd_data_word_received, MST_RD_DATA_WORD_RECEIVED);

#define ATTR_LIST(name) (&dev_attr_##name.attr)

static struct attribute *bh_pcie_perf_counters_attrs[] = {
	ATTR_LIST(slv_posted_wr_data_word_received0),
	ATTR_LIST(slv_nonposted_wr_data_word_received0),
	ATTR_LIST(slv_rd_data_word_sent0),
	ATTR_LIST(mst_posted_wr_data_word_sent0),
	ATTR_LIST(mst_nonposted_wr_data_word_sent0),
	ATTR_LIST(mst_rd_data_word_received0),
	ATTR_LIST(slv_posted_wr_data_word_received1),
	ATTR_LIST(slv_nonposted_wr_data_word_received1),
	ATTR_LIST(slv_rd_data_word_sent1),
	ATTR_LIST(mst_posted_wr_data_word_sent1),
	ATTR_LIST(mst_nonposted_wr_data_word_sent1),
	ATTR_LIST(mst_rd_data_word_received1),
	NULL,
};

static const struct attribute_group bh_pcie_perf_counters_group = {
	.name = "pcie_perf_counters",
	.attrs = bh_pcie_perf_counters_attrs,
};

static const struct tt_hwmon_label bh_hwmon_labels[] = {
	{ "asic_temp", hwmon_temp,  hwmon_temp_label  },
	{ "vcore",     hwmon_in,    hwmon_in_label    },
	{ "current",   hwmon_curr,  hwmon_curr_label  },
	{ "power",     hwmon_power, hwmon_power_label },
	{ "fan_rpm",   hwmon_fan,   hwmon_fan_label   },
	{ NULL },	// sentinel
};

static const struct tt_hwmon_attr bh_hwmon_attrs[] = {
	{ TELEMETRY_ASIC_TEMP,          hwmon_temp,  hwmon_temp_input  },
	{ TELEMETRY_THM_LIMIT_THROTTLE, hwmon_temp,  hwmon_temp_max    },
	{ TELEMETRY_VCORE,              hwmon_in,    hwmon_in_input    },
	{ TELEMETRY_VDD_LIMITS,         hwmon_in,    hwmon_in_max      },
	{ TELEMETRY_CURRENT,            hwmon_curr,  hwmon_curr_input  },
	{ TELEMETRY_TDC_LIMIT_MAX,      hwmon_curr,  hwmon_curr_max    },
	{ TELEMETRY_POWER,              hwmon_power, hwmon_power_input },
	{ TELEMETRY_TDP_LIMIT_MAX,      hwmon_power, hwmon_power_max   },
	{ TELEMETRY_FAN_RPM,            hwmon_fan,   hwmon_fan_input   },
	{ 0 },	// sentinel
};

static struct tenstorrent_sysfs_attr bh_sysfs_attributes[] = {
	{ TELEMETRY_AICLK, __ATTR(tt_aiclk,  S_IRUGO, tt_sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_AXICLK, __ATTR(tt_axiclk, S_IRUGO, tt_sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_ARCCLK, __ATTR(tt_arcclk, S_IRUGO, tt_sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_BOARD_ID, __ATTR(tt_serial, S_IRUGO, tt_sysfs_show_u64_hex, NULL) },
	{ TELEMETRY_BOARD_ID, __ATTR(tt_card_type, S_IRUGO, tt_sysfs_show_card_type, NULL) },
	{ TELEMETRY_FLASH_BUNDLE_VERSION, __ATTR(tt_fw_bundle_ver, S_IRUGO, tt_sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_BM_APP_FW_VERSION, __ATTR(tt_m3app_fw_ver, S_IRUGO, tt_sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_ASIC_ID, __ATTR(tt_asic_id, S_IRUGO, tt_sysfs_show_u64_hex, NULL) },
	{ TELEMETRY_TIMER_HEARTBEAT, __ATTR(tt_heartbeat, S_IRUGO, tt_sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_THERM_TRIP_COUNT, __ATTR(tt_therm_trip_count, S_IRUGO, tt_sysfs_show_u32_dec, NULL) },
};

static const struct hwmon_channel_info *bh_hwmon_channel_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL | HWMON_I_MAX),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_MAX),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_MAX),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL),
	NULL,
};

static const struct hwmon_chip_info bh_hwmon_chip_info = {
	.ops = &tt_hwmon_ops,
	.info = bh_hwmon_channel_info,
};

static int blackhole_read_telemetry_tag(struct tenstorrent_device *tt_dev, u16 tag_id, u32 *value)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u64 addr;

	if (tag_id >= TELEM_TAG_CACHE_SIZE)
		return -EINVAL;

	addr = tt_dev->telemetry_tag_cache[tag_id];
	if (addr == 0)
		return -ENODATA;

	// Telemetry is the lowest-value, highest-frequency NoC consumer in the
	// driver (one hwmon/sysfs read per poll), and kernel_tlb_mutex is the
	// device's ONLY kernel-side NoC serializer -- the ARC message queue,
	// save/restore_reset_state and the last-close grid re-init all go
	// through it.  A telemetry read that stalls on the wire while holding
	// it convoys every one of those, including the recovery paths.  So
	// telemetry never waits for the mutex: if someone else is on the NoC,
	// the sample is dropped (-EBUSY) rather than queued.
	return csm_read32_nowait(bh, addr, value);
}

static int telemetry_probe(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 base_addr, data_addr;
	u32 version, major_ver, minor_ver, patch_ver;
	u32 tags_addr;
	u32 num_entries;
	u32 i;

	memset(tt_dev->telemetry_tag_cache, 0, sizeof(tt_dev->telemetry_tag_cache));

	if (noc_read32(bh, ARC_X, ARC_Y, ARC_TELEMETRY_PTR, 0, &base_addr) != 0 ||
	    noc_read32(bh, ARC_X, ARC_Y, ARC_TELEMETRY_DATA, 0, &data_addr) != 0) {
		dev_err(&tt_dev->pdev->dev, "Telemetry not available (device not responding)\n");
		return -EIO;
	}

	tags_addr = base_addr + 8;

	if (!is_range_within_csm(base_addr, 1) || !is_range_within_csm(data_addr, 1)) {
		dev_err(&tt_dev->pdev->dev, "Telemetry not available\n");
		return -ENODEV;
	}

	if (noc_read32(bh, ARC_X, ARC_Y, base_addr, 0, &version) != 0)
		return -EIO;

	major_ver = (version >> 16) & 0xFF;
	minor_ver = (version >> 8) & 0xFF;
	patch_ver = version & 0xFF;

	if (major_ver > 1) {
		dev_err(&tt_dev->pdev->dev, "Unsupported telemetry version %u.%u.%u\n", major_ver, minor_ver, patch_ver);
		return -ENOTSUPP;
	}

	if (noc_read32(bh, ARC_X, ARC_Y, base_addr + 4, 0, &num_entries) != 0)
		return -EIO;

	for (i = 0; i < num_entries; ++i) {
		u32 tag_entry;
		u16 tag_id;
		u16 offset;
		u32 addr;

		if (noc_read32(bh, ARC_X, ARC_Y, tags_addr + (i * 4), 0, &tag_entry) != 0)
			return -EIO;

		tag_id = tag_entry & 0xFFFF;
		offset = (tag_entry >> 16) & 0xFFFF;
		addr = data_addr + (offset * 4);

		if (tag_id < TELEM_TAG_CACHE_SIZE)
			tt_dev->telemetry_tag_cache[tag_id] = addr;
	}

	return 0;
}

static bool send_arc_message(struct blackhole_device *bh, struct arc_msg *msg)
{
	u32 boot_status;
	u32 queue_ctrl_addr;
	u32 queue_base;
	u32 queue_info;
	u32 num_entries;
	unsigned long timeout = jiffies + msecs_to_jiffies(ARC_MSG_READY_MS);

	do {
		if (noc_read32(bh, ARC_X, ARC_Y, ARC_BOOT_STATUS, 0, &boot_status) != 0)
			return false; // device latched hung, or MMIO refused
		if (boot_status == 0xFFFFFFFFu) {
			// Single all-ones on the ARC boot status is already the
			// driver's historical "NOC is hung" signal; arm the latch
			// so subsequent hwmon/ioctl paths stop issuing MMIO.
			bh_set_hung(bh, "ARC_BOOT_STATUS all-ones", 0);
			return false;
		}
		if (boot_status & ARC_BOOT_STATUS_READY_FOR_MSG)
			break;
	} while (time_before(jiffies, timeout));

	if (!(boot_status & ARC_BOOT_STATUS_READY_FOR_MSG))
		return false;

	if (noc_read32(bh, ARC_X, ARC_Y, ARC_MSG_QCB_PTR, 0, &queue_ctrl_addr) != 0)
		return false;

	if (csm_read32(bh, queue_ctrl_addr + 0, &queue_base) != 0)
		return false;

	if (csm_read32(bh, queue_ctrl_addr + 4, &queue_info) != 0)
		return false;

	num_entries = queue_info & 0xFF;

	if (!arc_msg_push(&bh->tt, msg, queue_base, num_entries))
		return false;

	// Trigger ARC interrupt
	if (noc_write32(bh, ARC_X, ARC_Y, ARC_MSI_FIFO, 0, 0) != 0)
		return false;

	if (!arc_msg_pop(&bh->tt, msg, queue_base, num_entries))
		return false;

	return msg->header == 0;
}

static bool blackhole_reset(struct tenstorrent_device *tt_dev, u32 reset_flag)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct pci_dev *pdev = tt_dev->pdev;

	// Reset is a recovery path: it is explicitly allowed to talk to a device
	// we have given up on, so drop the latch before trying.  If the device is
	// still dead the first read re-latches it.
	bh_clear_hung(bh);

	if (reset_flag == TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET) {
		struct arc_msg msg = { 0 };
		u16 reset_arg = 3; // Argument for ASIC + M3 reset

		msg.header = ARC_MSG_TYPE_TEST;
		if (!send_arc_message(bh, &msg)) {
			dev_warn(&tt_dev->pdev->dev, "Couldn't communicate with firmware; NOC is likely hung.\n");
			return false;
		}

		set_reset_marker(pdev);

		memset(&msg, 0, sizeof(msg));
		msg.header = ARC_MSG_TYPE_TRIGGER_RESET;
		msg.payload[0] = reset_arg;
		send_arc_message(bh, &msg);
		return true; // Possibly a lie...
	} else if (reset_flag == TENSTORRENT_RESET_DEVICE_ASIC_RESET) {
		set_reset_marker(pdev);
		return pcie_timer_interrupt(pdev);
	}

	return false;
}

// Called from tt_cdev_release via dev_class->last_release_cb when the LAST fd
// closes the device (chardev_open_count == 0).  A process killed mid-init
// (SIGKILL / OOM) leaves the Tensix grid + NoC in a partially-programmed state
// that wedges the next opener; re-initializing here makes the wedge
// self-healing.  Because this only fires at count==0 there is no surviving
// client to disturb.
//
// REINIT_TENSIX rides the ARC message queue, so it only works while the ARC +
// NoC are still alive.  If the NoC is already hung send_arc_message fails
// (boot_status reads 0xFFFFFFFF); we no-op gracefully in that case -- the
// deeper recovery is the DMC-side auto-reset watchdog (sideband, not NoC),
// which is out of scope for this last-close path.  Never crash the release
// path: this runs under chardev_mutex with no fd to return an error to.
static void blackhole_last_release(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct arc_msg msg = { 0 };

	if (!reset_on_last_close)
		return;

	// Device is gone or mid-reset: nothing safe to talk to.
	if (tt_dev->detached || tt_dev->needs_hw_init)
		return;

	msg.header = ARC_MSG_TYPE_REINIT_TENSIX;
	if (!send_arc_message(bh, &msg))
		dev_warn(&tt_dev->pdev->dev,
			 "last-close grid re-init skipped; ARC/NoC unresponsive (likely hung). "
			 "A DMC/cold reset may be required.\n");
}

static bool blackhole_init(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct device *dev = &tt_dev->pdev->dev;
	resource_size_t bar4_len = pci_resource_len(tt_dev->pdev, 4);
	int i;

	// Limit 4G window count to what's available; partial windows not supported.
	tt_dev->tlb_counts[1] = bar4_len / TLB_4G_WINDOW_SIZE;

	tt_dev->telemetry_attrs = devm_kcalloc(dev, ARRAY_SIZE(bh_sysfs_attributes) + 1, sizeof(struct attribute *), GFP_KERNEL);

	if (!tt_dev->telemetry_attrs)
		return false;

	bh->tlb_regs = pci_iomap_range(bh->tt.pdev, 0, TLB_REGS_START, TLB_REGS_LEN);
	bh->kernel_tlb = pci_iomap_range(bh->tt.pdev, 0, KERNEL_TLB_START, KERNEL_TLB_LEN);
	bh->noc2axi_cfg = pci_iomap_range(bh->tt.pdev, 0, NOC2AXI_CFG_START, NOC2AXI_CFG_LEN);
	bh->bar2_mapping = pci_iomap(bh->tt.pdev, 2, 0);

	if (!bh->tlb_regs || !bh->kernel_tlb || !bh->noc2axi_cfg) {
		if (bh->tlb_regs)
			pci_iounmap(bh->tt.pdev, bh->tlb_regs);

		if (bh->kernel_tlb)
			pci_iounmap(bh->tt.pdev, bh->kernel_tlb);

		if (bh->noc2axi_cfg)
			pci_iounmap(bh->tt.pdev, bh->noc2axi_cfg);

		if (bh->bar2_mapping)
			pci_iounmap(bh->tt.pdev, bh->bar2_mapping);

		return false;
	}

	// Claim the topmost 2M window for kernel use.
	set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs);
	mutex_init(&bh->kernel_tlb_mutex);

	bh->allones_streak = 0;
	atomic_set(&bh->hung, 0);

	for (i = 0; i < ARRAY_SIZE(bh_sysfs_attributes); ++i)
		tt_dev->telemetry_attrs[i] = &bh_sysfs_attributes[i].attr.attr;
	tt_dev->telemetry_group.attrs = tt_dev->telemetry_attrs;
	tt_dev->telemetry_group.is_visible = tt_sysfs_telemetry_is_visible;

	return true;
}

static bool blackhole_init_hardware(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct pci_dev *pdev = tt_dev->pdev;
	struct arc_msg msg = { 0 };

	// (Re-)init is the other recovery path -- probe, resume, and the PCIe
	// error handler's .resume all land here.  Same reasoning as reset.
	bh_clear_hung(bh);

	if (bh_hung_threshold != 0)
		dev_info(&pdev->dev,
			 "host-hang MMIO latch enabled (bh_hung_threshold=%u)\n",
			 bh_hung_threshold);

	pcie_set_readrq(pdev, MAX_MRRS);

	msg.header = ARC_MSG_TYPE_ASIC_STATE0;
	if (!send_arc_message(bh, &msg))
		dev_err(&tt_dev->pdev->dev, "Failed to send ARC message for A0 state\n");

	memset(&msg, 0, sizeof(msg));
	msg.header = ARC_MSG_TYPE_SET_WDT_TIMEOUT;
	msg.payload[0] = 1000 * auto_reset_timeout; // Convert seconds to milliseconds
	if (!send_arc_message(bh, &msg))
		dev_warn(&tt_dev->pdev->dev, "Failed to set ARC watchdog timeout (this is normal for old FW)\n");

	return true;
}

static bool blackhole_init_telemetry(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	int r;

	r = device_add_group(&tt_dev->dev, &bh_pcie_perf_counters_group);
	if (r)
		dev_err(&tt_dev->pdev->dev, "PCIe perf counters unavailable: %d\n", r);
	else
		bh->pcie_perf_group_registered = true;

	r = telemetry_probe(tt_dev);
	if (!r) {
		struct device *dev = &tt_dev->pdev->dev;
		struct device *hwmon_device;

		r = device_add_group(&tt_dev->dev, &tt_dev->telemetry_group);
		if (!r)
			bh->telemetry_group_registered = true;

		tt_dev->hwmon_attributes = bh_hwmon_attrs;
		tt_dev->hwmon_labels = bh_hwmon_labels;

		hwmon_device = hwmon_device_register_with_info(dev, "blackhole", tt_dev, &bh_hwmon_chip_info, NULL);
		if (IS_ERR(hwmon_device))
			return false;

		tt_dev->hwmon_dev = hwmon_device;

		// Notify udev that telemetry attributes are now available.
		kobject_uevent(&tt_dev->dev.kobj, KOBJ_CHANGE);
	}

	return true;
}

static void blackhole_cleanup_telemetry(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	if (tt_dev->hwmon_dev) {
		hwmon_device_unregister(tt_dev->hwmon_dev);
		tt_dev->hwmon_dev = NULL;
	}

	if (bh->telemetry_group_registered) {
		device_remove_group(&tt_dev->dev, &tt_dev->telemetry_group);
		bh->telemetry_group_registered = false;
	}

	if (bh->pcie_perf_group_registered) {
		device_remove_group(&tt_dev->dev, &bh_pcie_perf_counters_group);
		bh->pcie_perf_group_registered = false;
	}
}

static void blackhole_cleanup_hardware(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct arc_msg msg = { 0 };

	if (tt_dev->detached)
		return;

	msg.header = ARC_MSG_TYPE_ASIC_STATE3;
	if (!send_arc_message(bh, &msg))
		dev_err(&tt_dev->pdev->dev, "Failed to send ARC message for A3 state\n");
}

static void blackhole_cleanup(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	if (bh->tlb_regs)
		pci_iounmap(tt_dev->pdev, bh->tlb_regs);
	if (bh->kernel_tlb)
		pci_iounmap(tt_dev->pdev, bh->kernel_tlb);
	if (bh->noc2axi_cfg)
		pci_iounmap(tt_dev->pdev, bh->noc2axi_cfg);
	if (bh->bar2_mapping)
		pci_iounmap(tt_dev->pdev, bh->bar2_mapping);
}

static int blackhole_configure_tlb(struct tenstorrent_device *tt_dev, int tlb,
				   struct tenstorrent_noc_tlb_config *config)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	if (tlb >= 0 && tlb < TLB_2M_WINDOW_COUNT)
		return blackhole_configure_tlb_2M(bh, tlb, config);

	if (tlb >= TLB_2M_WINDOW_COUNT && tlb < TLB_TOTAL_WINDOW_COUNT)
		return blackhole_configure_tlb_4G(bh, tlb, config);

	return -EINVAL;
}

static int blackhole_describe_tlb(struct tenstorrent_device *tt_dev, int tlb,
				  struct tlb_descriptor *desc)
{
	bool is_2M = tlb < TLB_2M_WINDOW_COUNT;

	if (tlb < 0 || tlb >= TLB_TOTAL_WINDOW_COUNT)
		return -EINVAL;

	desc->bar = is_2M ? 0 : 4;
	desc->size = is_2M ? TLB_2M_WINDOW_SIZE : TLB_4G_WINDOW_SIZE;
	desc->bar_offset =
		is_2M ? tlb * TLB_2M_WINDOW_SIZE :
			(tlb - TLB_2M_WINDOW_COUNT) * TLB_4G_WINDOW_SIZE;

	return 0;
}

static int blackhole_configure_outbound_atu(struct tenstorrent_device *tt_dev, u32 region, u64 base, u64 limit,
					    u64 target)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u64 size = limit - base + 1;
	u32 region_ctrl_1 = INCREASE_REGION_SIZE;
	u32 region_ctrl_2 = (limit == 0) ? 0 : REGION_EN;
	u32 region_ctrl_3 = 0;
	u32 lower_base = lower_32_bits(base);
	u32 upper_base = upper_32_bits(base);
	u32 lower_target = lower_32_bits(target);
	u32 upper_target = upper_32_bits(target);
	u32 lower_limit = lower_32_bits(limit);
	u32 upper_limit = upper_32_bits(limit);

	// iATU has a max region size of 1T.
	if (size > SZ_1T)
		return -EINVAL;

	if (region >= IATU_OUTBOUND_REGIONS)
		return -EINVAL;

	WRITE_IATU_REG(bh, OUTBOUND, region, LOWER_BASE_ADDR, lower_base);
	WRITE_IATU_REG(bh, OUTBOUND, region, UPPER_BASE_ADDR, upper_base);
	WRITE_IATU_REG(bh, OUTBOUND, region, LOWER_TARGET_ADDR, lower_target);
	WRITE_IATU_REG(bh, OUTBOUND, region, UPPER_TARGET_ADDR, upper_target);
	WRITE_IATU_REG(bh, OUTBOUND, region, LOWER_LIMIT_ADDR, lower_limit);
	WRITE_IATU_REG(bh, OUTBOUND, region, UPPER_LIMIT_ADDR, upper_limit);
	WRITE_IATU_REG(bh, OUTBOUND, region, REGION_CTRL_1, region_ctrl_1);
	WRITE_IATU_REG(bh, OUTBOUND, region, REGION_CTRL_2, region_ctrl_2);
	WRITE_IATU_REG(bh, OUTBOUND, region, REGION_CTRL_3, region_ctrl_3);

	return 0;
}

static void blackhole_noc_write32(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr, u32 data, int noc)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	(void)noc_write32(bh, x, y, addr, data, noc);
}

static int blackhole_set_power_state(struct tenstorrent_device *tt_dev, struct tenstorrent_power_state *power_state)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct arc_msg msg = {0};
	bool ok;

	msg.header = ARC_MSG_TYPE_POWER_SETTING | (power_state->validity << 8) | (power_state->power_flags << 16);
	BUILD_BUG_ON(sizeof(power_state->power_settings) != sizeof(msg.payload));
	memcpy(msg.payload, power_state->power_settings, sizeof(msg.payload));

	ok = send_arc_message(bh, &msg);
	if (!ok)
		return -EINVAL;

	return 0;
}

struct tenstorrent_device_class blackhole_class = {
	.name = "Blackhole",
	.instance_size = sizeof(struct blackhole_device),
	.dma_address_bits = 58,
	.noc_dma_limit = (1ULL << 58) - 1,
	.noc_pcie_offset = (4ULL << 58),
	.tlb_kinds = 2,
	.tlb_counts = { TLB_2M_WINDOW_COUNT, TLB_4G_WINDOW_COUNT },
	.tlb_sizes = { TLB_2M_WINDOW_SIZE, TLB_4G_WINDOW_SIZE },
	.reset = blackhole_reset,
	.init_device = blackhole_init,
	.init_hardware = blackhole_init_hardware,
	.init_telemetry = blackhole_init_telemetry,
	.cleanup_telemetry = blackhole_cleanup_telemetry,
	.read_telemetry_tag = blackhole_read_telemetry_tag,
	.probe_telemetry = telemetry_probe,
	.cleanup_hardware = blackhole_cleanup_hardware,
	.cleanup_device = blackhole_cleanup,
	.last_release_cb = blackhole_last_release,
	.configure_tlb = blackhole_configure_tlb,
	.describe_tlb = blackhole_describe_tlb,
	.save_reset_state = blackhole_save_reset_state,
	.restore_reset_state = blackhole_restore_reset_state,
	.configure_outbound_atu = blackhole_configure_outbound_atu,
	.noc_write32 = blackhole_noc_write32,
	.csm_read32 = blackhole_csm_read32,
	.csm_write32 = blackhole_csm_write32,
	.set_power_state = blackhole_set_power_state,
};
