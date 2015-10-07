/*-
* Copyright (c) 2007-2008, Juniper Networks, Inc.
* All rights reserved.
*
* SPDX-License-Identifier:	GPL-2.0
*/

#include <StdAfx.h>
#include <pci_drv.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include "usb_defs.h"
#include "usbdescriptors.h"
#include "ch9.h"
#include "usb.h"
#include "ehci.h"

//Only available when EHCI function is enabled.
#ifdef CONFIG_USB_EHCI

/* Information about a USB port */
struct ehci_pci_priv {
	struct ehci_ctrl ehci;
};

static BOOL ehci_pci_common_init(__PHYSICAL_DEVICE* pdev, struct ehci_hccr **ret_hccr,
struct ehci_hcor **ret_hcor)
{
	struct ehci_hccr *hccr = NULL;
	struct ehci_hcor *hcor = NULL;
	uint32_t cmd;
#ifdef __CFG_SYS_VMM
	DWORD dwMemSize = 0;
	LPVOID pMemRegion = NULL;
#endif
	BOOL  bResult = FALSE;

	//Get hccr from configuration space.
	hccr = (struct ehci_hccr*)pdev->ReadDeviceConfig(pdev, PCI_CONFIG_OFFSET_BASE1, sizeof(DWORD));

	//Reserve the HCCR memory region in case of VMM enabled.
#ifdef __CFG_SYS_VMM
	for (int i = 0; i < MAX_RESOURCE_NUM; i++)
	{
		if (pdev->Resource[i].dwResType == RESOURCE_TYPE_EMPTY)
		{
			break;
		}
		if (pdev->Resource[i].dwResType == RESOURCE_TYPE_MEMORY)
		{
			if ((DWORD)pdev->Resource[i].Dev_Res.MemoryRegion.lpStartAddr == (DWORD)hccr)
			{
				//Reserve the memory region in virtual address space.
				dwMemSize = (DWORD)pdev->Resource[i].Dev_Res.MemoryRegion.lpEndAddr -
					(DWORD)pdev->Resource[i].Dev_Res.MemoryRegion.lpStartAddr;
				dwMemSize += 1;
				pMemRegion = VirtualAlloc((LPVOID)hccr,
					dwMemSize,
					VIRTUAL_AREA_ALLOCATE_IO,
					VIRTUAL_AREA_ACCESS_RW,
					"EHCI_REG");
				if (pMemRegion != (LPVOID)hccr)
				{
					goto __TERMINAL;
				}
				else  //Allocate successfully.
				{
					bResult = TRUE;
					break;
				}
			}
		}
	}
#else
	//Just mark the initialization process is successful.
	bResult = TRUE;
#endif

	//Get HCOR from HCCR.
	hcor = (struct ehci_hcor *)((uint32_t)hccr +
		HC_LENGTH(ehci_readl(&hccr->cr_capbase)));

	debug("USB: EHCI-PCI init hccr 0x%x and hcor 0x%x hc_length %d.\r\n",
		(uint32_t)hccr, (uint32_t)hcor,
		(uint32_t)HC_LENGTH(ehci_readl(&hccr->cr_capbase)));

	*ret_hccr = hccr;
	*ret_hcor = hcor;

	/* enable busmaster */
	cmd = pdev->ReadDeviceConfig(pdev, PCI_CONFIG_OFFSET_COMMAND, 2);
	cmd |= 0x04;
	pdev->WriteDeviceConfig(pdev, PCI_CONFIG_OFFSET_COMMAND, cmd, 2);

__TERMINAL:
	if (!bResult)  //Failure of initialization.
	{
		if (pMemRegion)  //Should release it.
		{
			VirtualFree(pMemRegion);
		}
	}
	return bResult;
}

#ifndef CONFIG_DM_USB

#ifdef CONFIG_PCI_EHCI_DEVICE
static struct pci_device_id ehci_pci_ids[] = {
	/* Please add supported PCI EHCI controller ids here */
	{ 0x1033, 0x00E0 },	/* NEC */
	{ 0x10B9, 0x5239 },	/* ULI1575 PCI EHCI module ids */
	{ 0x12D8, 0x400F },	/* Pericom */
	{ 0, 0 }
};
#endif

/*
* Create the appropriate control structures to manage
* a new EHCI host controller.
*/
#define PCI_EHCI_CLASS_ID 0x0C0320

//Save the USB controller's physical device object,to use as 
//begining iterating position when GetDevice is called next time.
static __PHYSICAL_DEVICE* pOldUsbCtrl = NULL;

int ehci_hcd_init(int index, enum usb_init_type init,
struct ehci_hccr **ret_hccr, struct ehci_hcor **ret_hcor)
{
	__PHYSICAL_DEVICE* pUsbCtrl = NULL;
	__IDENTIFIER id;

	//Set searching ID.
	id.dwBusType = BUS_TYPE_PCI;
	id.Bus_ID.PCI_Identifier.ucMask  = PCI_IDENTIFIER_MASK_CLASS;
	id.Bus_ID.PCI_Identifier.dwClass = (PCI_EHCI_CLASS_ID << 8);

	pUsbCtrl = DeviceManager.GetDevice(&DeviceManager,BUS_TYPE_PCI,
		&id, pOldUsbCtrl);
	if (NULL == pUsbCtrl) {
		printf("USB: EHCI host controller [%d] is not found.\r\n",index);
		return -1;
	}

	//Save the physical device object pointer.
	pOldUsbCtrl = pUsbCtrl;

	if (ehci_pci_common_init(pUsbCtrl, ret_hccr, ret_hcor))
	{
		return 0;
	}
	return -1;
}

/*
* Destroy the appropriate control structures corresponding
* the the EHCI host controller.
*/
int ehci_hcd_stop(int index)
{
	return 0;
}
#endif /* nCONFIG_DM_USB */

#ifdef CONFIG_DM_USB
static int ehci_pci_probe(struct udevice *dev)
{
	struct ehci_hccr *hccr;
	struct ehci_hcor *hcor;

	ehci_pci_common_init(pci_get_bdf(dev), &hccr, &hcor);

	return ehci_register(dev, hccr, hcor, NULL, 0, USB_INIT_HOST);
}

static int ehci_pci_remove(struct udevice *dev)
{
	int ret;

	ret = ehci_deregister(dev);
	if (ret)
		return ret;

	return 0;
}

U_BOOT_DRIVER(ehci_pci) = {
	.name = "ehci_pci",
	.id = UCLASS_USB,
	.probe = ehci_pci_probe,
	.remove = ehci_pci_remove,
	.ops = &ehci_usb_ops,
	.platdata_auto_alloc_size = sizeof(struct usb_platdata),
	.priv_auto_alloc_size = sizeof(struct ehci_pci_priv),
	.flags = DM_FLAG_ALLOC_PRIV_DMA,
};

static struct pci_device_id ehci_pci_supported[] = {
	{ PCI_DEVICE_CLASS(PCI_CLASS_SERIAL_USB_EHCI, ~0) },
	{},
};

U_BOOT_PCI_DEVICE(ehci_pci, ehci_pci_supported);

#endif /* CONFIG_DM_USB */

#endif //CONFIG_USB_EHCI
