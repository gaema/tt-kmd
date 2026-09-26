// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/debugfs.h>
#include <linux/proc_fs.h>
#include <linux/version.h>

#include "chardev.h"
#include "enumerate.h"

#include "module.h"
#include "gaema-build.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
#error "tt-kmd requires Linux 5.4 or later"
#endif

#define TENSTORRENT_DRIVER_VERSION_STRING \
	__stringify(TENSTORRENT_DRIVER_VERSION_MAJOR) "." \
	__stringify(TENSTORRENT_DRIVER_VERSION_MINOR) "." \
	__stringify(TENSTORRENT_DRIVER_VERSION_PATCH) \
	TENSTORRENT_DRIVER_VERSION_SUFFIX

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Tenstorrent AI kernel driver");
MODULE_VERSION(TENSTORRENT_DRIVER_VERSION_STRING);

// Which commit of our fork this .ko was built from.  MODULE_VERSION alone says
// "some gaema build"; this says which one, readable on a host with no source
// tree via `modinfo tenstorrent | grep gaema_build`.
MODULE_INFO(gaema_build, GAEMA_BUILD_ID);

struct dentry *tt_debugfs_root;
struct proc_dir_entry *tt_procfs_root;

static uint max_devices = 32;
module_param(max_devices, uint, 0444);
MODULE_PARM_DESC(max_devices, "Maximum number of tenstorrent devices (chips) to support.");

uint dma_address_bits = 0;
module_param(dma_address_bits, uint, 0444);
MODULE_PARM_DESC(dma_address_bits, "DMA address bits, 0 for automatic.");

uint reset_limit = 10;
module_param(reset_limit, uint, 0444);
MODULE_PARM_DESC(reset_limit, "Maximum number of times to reset device during boot.");

unsigned char auto_reset_timeout = 10;
module_param(auto_reset_timeout, byte, 0444);
MODULE_PARM_DESC(auto_reset_timeout, "Timeout duration in seconds for M3 auto reset to occur.");

bool power_policy = true;
module_param(power_policy, bool, 0444);
MODULE_PARM_DESC(power_policy, "Enable power policy: low power at probe, re-aggregate on close (default=on).");

uint idle_power_down_grace_ms = 5000;
module_param(idle_power_down_grace_ms, uint, 0644);
MODULE_PARM_DESC(idle_power_down_grace_ms,
		 "Delay in ms between the last fd closing a device and the "
		 "idle power-down message being sent.  0 sends the message "
		 "synchronously at close.  Only honored by device classes "
		 "that opt in via defer_idle_powerdown.");

// DEFAULT OFF since 2026-08-29 (upstream parity: upstream never sends this).
// Measured on the p300c (2-card) host: REINIT_TENSIX to a post-PERST p300c
// hung the ARC at 0x1002BF80 (WriteReg <- NOC2AXIWrite32 inside
// tensix_inject_instruction, TensixInit's broadcast) on all four dies, the DMC
// watchdog then reset the ASIC, and the endpoint came back with its config
// space at defaults -- the whole "four dead dies" wedge of 2026-08-28.  The
// re-init is a wedge-PREVENTION nicety; on an unready chip it is the wedge.
// Opt in with reset_on_last_close=1; the gate in blackhole_last_release()
// then still requires a live, heartbeat-advancing ARC.
bool reset_on_last_close = false;
module_param(reset_on_last_close, bool, 0644);
MODULE_PARM_DESC(reset_on_last_close,
		 "DEFAULT OFF. On the last fd closing a device, re-initialize the Tensix "
		 "grid + NoC so a process killed mid-init (SIGKILL/OOM) does "
		 "not wedge the device for the next opener.  Only honored by "
		 "device classes that opt in via last_release_cb (Blackhole). "
		 "Default on.");

// The ARC firmware answers every aggregated power-state message that covers the
// L2CPU domain by calling bh_set_l2cpu_enable(), whether or not the state
// changed, and that reprograms the shared L2CPU PLL (PLL4) to its cached
// 800 MHz under any running X280 guest. A legacy open() sends such a message.
// Measured on a p150a 2026-09-24: a bare open of /dev/tenstorrent/0 dropped a
// running guest from 1750 to 800 MHz (claude:ai/tenstorrent/blackhole/p150a/
// audit/2026-09-24-l2cpu-plumbing-p4-p6-p8-*). With this set, the aggregate
// is sent with a flag count that stops BELOW the L2CPU flag, so the firmware
// skips that domain; AICLK, MRISC and Tensix are applied as before.
bool l2cpu_power_hands_off = false;
module_param(l2cpu_power_hands_off, bool, 0644);
MODULE_PARM_DESC(l2cpu_power_hands_off,
		 "DEFAULT OFF. Never send the L2CPU power domain to firmware, so "
		 "no device open/close can reprogram the L2CPU PLL under a running "
		 "guest. The L2CPU clock domain then stays as the last explicit "
		 "setting left it.");

uint bh_hung_threshold = 3;
module_param(bh_hung_threshold, uint, 0644);
MODULE_PARM_DESC(bh_hung_threshold,
		 "Blackhole: number of consecutive all-ones (0xFFFFFFFF) kernel "
		 "TLB/NoC reads after which the device is latched as not "
		 "responding and further MMIO is refused with -EIO.  Arms on the "
		 "TLB path alone (ARC/NoC hang with a live endpoint is enough); "
		 "BAR0/config probe is log classification only.  Cleared by "
		 "device reset or re-init.  0 disables the latch.  Default 3.");

const struct pci_device_id tenstorrent_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_GRAYSKULL),
	  .driver_data=(kernel_ulong_t)NULL}, // Deprecated
	{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_WORMHOLE),
	  .driver_data=(kernel_ulong_t)&wormhole_class },
	{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_BLACKHOLE),
	  .driver_data=(kernel_ulong_t)&blackhole_class },
	{ 0 },
};

MODULE_DEVICE_TABLE(pci, tenstorrent_ids);

static int __init ttdriver_init(void)
{
	int err = 0;

	pr_info("Loading Tenstorrent AI driver module v%s (build %s)\n",
		TENSTORRENT_DRIVER_VERSION_STRING, GAEMA_BUILD_ID);

	tt_debugfs_root = debugfs_create_dir("tenstorrent", NULL);

	tt_procfs_root = proc_mkdir("driver/tenstorrent", NULL);
	if (!tt_procfs_root) {
		err = -ENOMEM;
		goto fail_procfs;
	}

	err = init_char_driver(max_devices);
	if (err != 0)
		goto fail_char_driver;

	err = tenstorrent_pci_register_driver();
	if (err != 0)
		goto fail_pci_register;

	return 0;

fail_pci_register:
	cleanup_char_driver();
fail_char_driver:
	proc_remove(tt_procfs_root);
fail_procfs:
	debugfs_remove(tt_debugfs_root);

	return err;
}

static void __exit ttdriver_cleanup(void)
{
	pr_info("Unloading Tenstorrent AI driver module\n");

	tenstorrent_pci_unregister_driver();
	cleanup_char_driver();
	debugfs_remove(tt_debugfs_root);
	proc_remove(tt_procfs_root);
}

module_init(ttdriver_init);
module_exit(ttdriver_cleanup);
