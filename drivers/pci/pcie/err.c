// SPDX-License-Identifier: GPL-2.0
/*
 * This file implements the error recovery as a core part of PCIe error
 * reporting. When a PCIe error is delivered, an error message will be
 * collected and printed to console, then, an error recovery procedure
 * will be executed by following the PCI error recovery rules.
 *
 * Copyright (C) 2006 Intel Corp.
 *	Tom Long Nguyen (tom.l.nguyen@intel.com)
 *	Zhang Yanmin (yanmin.zhang@intel.com)
 */

#define dev_fmt(fmt) "AER: " fmt

#include <linux/pci.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/aer.h>
#include "portdrv.h"
#include "../pci.h"

static pci_ers_result_t merge_result(enum pci_ers_result orig,
				  enum pci_ers_result new)
{
	if (new == PCI_ERS_RESULT_NO_AER_DRIVER)
		return PCI_ERS_RESULT_NO_AER_DRIVER;

	if (new == PCI_ERS_RESULT_NONE)
		return orig;

	switch (orig) {
	case PCI_ERS_RESULT_CAN_RECOVER:
	case PCI_ERS_RESULT_RECOVERED:
		orig = new;
		break;
	case PCI_ERS_RESULT_DISCONNECT:
		if (new == PCI_ERS_RESULT_NEED_RESET)
			orig = PCI_ERS_RESULT_NEED_RESET;
		break;
	default:
		break;
	}

	return orig;
}

static int report_error_detected(struct pci_dev *dev,
				 pci_channel_state_t state,
				 enum pci_ers_result *result)
{
	pci_ers_result_t vote;
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	if (!pci_dev_set_io_state(dev, state) ||
		!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->error_detected) {
		/*
		 * If any device in the subtree does not have an error_detected
		 * callback, PCI_ERS_RESULT_NO_AER_DRIVER prevents subsequent
		 * error callbacks of "any" device in the subtree, and will
		 * exit in the disconnected error state.
		 */
		if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE) {
			vote = PCI_ERS_RESULT_NO_AER_DRIVER;
			pci_info(dev, "can't recover (no error_detected callback)\n");
		} else {
			vote = PCI_ERS_RESULT_NONE;
		}
	} else {
		err_handler = dev->driver->err_handler;
		vote = err_handler->error_detected(dev, state);
	}
	pci_uevent_ers(dev, vote);
	*result = merge_result(*result, vote);
	device_unlock(&dev->dev);
	return 0;
}

static int report_normal_detected(struct pci_dev *dev, void *data)
{
	return report_error_detected(dev, pci_channel_io_normal, data);
}

static int report_mmio_enabled(struct pci_dev *dev, void *data)
{
	pci_ers_result_t vote, *result = data;
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	if (!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->mmio_enabled)
		goto out;

	err_handler = dev->driver->err_handler;
	vote = err_handler->mmio_enabled(dev);
	*result = merge_result(*result, vote);
out:
	device_unlock(&dev->dev);
	return 0;
}

static int report_slot_reset(struct pci_dev *dev, void *data)
{
	pci_ers_result_t vote, *result = data;
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	if (!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->slot_reset)
		goto out;

	err_handler = dev->driver->err_handler;
	vote = err_handler->slot_reset(dev);
	*result = merge_result(*result, vote);
out:
	device_unlock(&dev->dev);
	return 0;
}

static int report_resume(struct pci_dev *dev, void *data)
{
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	if (!pci_dev_set_io_state(dev, pci_channel_io_normal) ||
		!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->resume)
		goto out;

	err_handler = dev->driver->err_handler;
	err_handler->resume(dev);
out:
	pci_uevent_ers(dev, PCI_ERS_RESULT_RECOVERED);
	device_unlock(&dev->dev);
	return 0;
}

pci_ers_result_t pcie_do_fatal_recovery(struct pci_dev *dev,
			pci_ers_result_t (*reset_link)(struct pci_dev *pdev))
{
	struct pci_dev *udev;
	struct pci_bus *parent;
	struct pci_dev *pdev, *temp;
	pci_ers_result_t result;

	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE)
		udev = dev;
	else
		udev = dev->bus->self;

	parent = udev->subordinate;
	pci_walk_bus(parent, pci_dev_set_disconnected, NULL);

        pci_lock_rescan_remove();
        pci_dev_get(dev);
        list_for_each_entry_safe_reverse(pdev, temp, &parent->devices,
					 bus_list) {
		pci_stop_and_remove_bus_device(pdev);
	}

	result = reset_link(udev);

	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
		/*
		 * If the error is reported by a bridge, we think this error
		 * is related to the downstream link of the bridge, so we
		 * do error recovery on all subordinates of the bridge instead
		 * of the bridge and clear the error status of the bridge.
		 */
		pci_aer_clear_fatal_status(dev);
		if (pcie_aer_is_native(dev))
			pcie_clear_device_status(dev);
	}

	if (result == PCI_ERS_RESULT_RECOVERED) {
		if (pcie_wait_for_link(udev, true))
			pci_rescan_bus(udev->bus);
		pci_info(dev, "Device recovery from fatal error successful\n");
        } else {
		pci_uevent_ers(dev, PCI_ERS_RESULT_DISCONNECT);
		pci_info(dev, "Device recovery from fatal error failed\n");
        }

	pci_dev_put(dev);
	pci_unlock_rescan_remove();

	return result;
}

pci_ers_result_t pcie_do_nonfatal_recovery(struct pci_dev *dev)
{
	pci_ers_result_t status = PCI_ERS_RESULT_CAN_RECOVER;
	struct pci_bus *bus;
	int ret;

	/*
	 * Error recovery runs on all subordinates of the first downstream port.
	 * If the downstream port detected the error, it is cleared at the end.
	 */
	if (!(pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
	      pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM))
		dev = dev->bus->self;
	bus = dev->subordinate;

	pci_dbg(dev, "broadcast error_detected message\n");
	pci_walk_bus(bus, report_normal_detected, &status);

	if (status == PCI_ERS_RESULT_CAN_RECOVER) {
		status = PCI_ERS_RESULT_RECOVERED;
		pci_dbg(dev, "broadcast mmio_enabled message\n");
		pci_walk_bus(bus, report_mmio_enabled, &status);
	}

	if (status == PCI_ERS_RESULT_NEED_RESET) {
		ret = pci_reset_bus(dev);
		if (ret < 0) {
			pci_err(dev, "Failed to reset %d\n", ret);
			status = PCI_ERS_RESULT_DISCONNECT;
			goto failed;
		}
		status = PCI_ERS_RESULT_RECOVERED;
		pci_dbg(dev, "broadcast slot_reset message\n");
		pci_walk_bus(bus, report_slot_reset, &status);
	}

	if (status != PCI_ERS_RESULT_RECOVERED)
		goto failed;

	pci_dbg(dev, "broadcast resume message\n");
	pci_walk_bus(bus, report_resume, &status);

	if (pcie_aer_is_native(dev))
		pcie_clear_device_status(dev);
	pci_aer_clear_nonfatal_status(dev);
	pci_info(dev, "device recovery successful\n");
	return status;

failed:
	pci_uevent_ers(dev, PCI_ERS_RESULT_DISCONNECT);

	/* TODO: Should kernel panic here? */
	pci_info(dev, "device recovery failed\n");

	return status;
}
