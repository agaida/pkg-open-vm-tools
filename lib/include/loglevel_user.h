/* *************************************************************
 * Copyright 1998-2003 VMware, Inc.  All rights reserved.
 * 
 * *************************************************************
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 */
  
#ifndef _LOGLEVEL_USER_H_
#define _LOGLEVEL_USER_H_

#define INCLUDE_ALLOW_USERLEVEL
#include "includeCheck.h"

#define LOGLEVEL_EXTENSION user
#include "loglevel_defs.h"

#define LOGLEVEL_USER(LOGLEVEL_VAR) \
   /* user/main*/ \
   /* main has to be first. */ \
   LOGLEVEL_VAR(main), \
   LOGLEVEL_VAR(aio), \
   LOGLEVEL_VAR(passthrough), \
   LOGLEVEL_VAR(tools), \
   LOGLEVEL_VAR(license), \
   LOGLEVEL_VAR(vui), \
   LOGLEVEL_VAR(stats), \
   LOGLEVEL_VAR(cpucount), \
   \
   /* user/io */ \
   LOGLEVEL_VAR(disk), \
   LOGLEVEL_VAR(keyboard), \
   LOGLEVEL_VAR(vmmouse), \
   LOGLEVEL_VAR(timer), \
   LOGLEVEL_VAR(vga), \
   LOGLEVEL_VAR(svga), \
   LOGLEVEL_VAR(svga_rect), \
   LOGLEVEL_VAR(enableDetTimer), \
   LOGLEVEL_VAR(dma), \
   LOGLEVEL_VAR(floppy), \
   LOGLEVEL_VAR(cmos), \
   LOGLEVEL_VAR(vlance), \
   LOGLEVEL_VAR(e1000), \
   LOGLEVEL_VAR(serial), \
   LOGLEVEL_VAR(parallel), \
   LOGLEVEL_VAR(chipset), \
   LOGLEVEL_VAR(flashram), \
   LOGLEVEL_VAR(pci), \
   LOGLEVEL_VAR(pci_vide), \
   LOGLEVEL_VAR(pci_uhci), \
   LOGLEVEL_VAR(uhci), \
   LOGLEVEL_VAR(pci_ehci), \
   LOGLEVEL_VAR(ehci), \
   LOGLEVEL_VAR(usb), \
   LOGLEVEL_VAR(pci_1394), \
   LOGLEVEL_VAR(1394), \
   LOGLEVEL_VAR(pci_vlance), \
   LOGLEVEL_VAR(pci_svga), \
   LOGLEVEL_VAR(pci_e1000), \
   LOGLEVEL_VAR(pci_hyper), \
   LOGLEVEL_VAR(pcibridge), \
   LOGLEVEL_VAR(vide), \
   LOGLEVEL_VAR(hostonly), \
   LOGLEVEL_VAR(timeTracker), \
   LOGLEVEL_VAR(backdoorAbsMouse), \
   LOGLEVEL_VAR(oprom), \
   LOGLEVEL_VAR(http), \
   LOGLEVEL_VAR(vmci), \
   LOGLEVEL_VAR(pci_vmci), \
   \
   /* user/disk */ \
   LOGLEVEL_VAR(aioMgr), \
   LOGLEVEL_VAR(aioWin32), \
   LOGLEVEL_VAR(aioLinux), \
   LOGLEVEL_VAR(aioGeneric), \
   LOGLEVEL_VAR(cdrom), \
   LOGLEVEL_VAR(checksum), \
   \
   /* user/checkpoint */ \
   LOGLEVEL_VAR(checkpoint), \
   LOGLEVEL_VAR(dumper), \
   LOGLEVEL_VAR(migrate), \
   \
   /* user/gui */ \
   LOGLEVEL_VAR(gui), \
   LOGLEVEL_VAR(guiWin32), \
   LOGLEVEL_VAR(mks), \
   LOGLEVEL_VAR(mksClient), \
   LOGLEVEL_VAR(mksServer), \
   LOGLEVEL_VAR(mksKeyboard), \
   LOGLEVEL_VAR(mksMouse), \
   LOGLEVEL_VAR(mksHostOps), \
   LOGLEVEL_VAR(mksGLManager), \
   \
   /* user/sound */ \
   LOGLEVEL_VAR(sound), \
   \
   /* user/disklib */ \
   LOGLEVEL_VAR(disklib), \
   LOGLEVEL_VAR(sparseChecker), \
   LOGLEVEL_VAR(dataCache), \
   /* more */ \
   LOGLEVEL_VAR(dict), \
   LOGLEVEL_VAR(pci_scsi), \
   LOGLEVEL_VAR(scsi), \
   LOGLEVEL_VAR(grm), \
   LOGLEVEL_VAR(vmxnet), \
   LOGLEVEL_VAR(pciPassthru), \
   LOGLEVEL_VAR(vnet), \
   LOGLEVEL_VAR(macfilter), \
   LOGLEVEL_VAR(macbw), \
   LOGLEVEL_VAR(macfi), \
   LOGLEVEL_VAR(vmxfer), \
   LOGLEVEL_VAR(poll), \
   LOGLEVEL_VAR(barrier), \
   LOGLEVEL_VAR(mstat), \
   LOGLEVEL_VAR(vmLock), \
   LOGLEVEL_VAR(buslogic), \
   LOGLEVEL_VAR(lsilogic), \
   LOGLEVEL_VAR(diskVmnix), \
   LOGLEVEL_VAR(hbaCommon), \
   LOGLEVEL_VAR(backdoor), \
   LOGLEVEL_VAR(buslogicMdev), \
   LOGLEVEL_VAR(hgfs), \
   LOGLEVEL_VAR(memspace), \
   LOGLEVEL_VAR(dnd), \
   LOGLEVEL_VAR(appstate), \
   LOGLEVEL_VAR(vthread), \
   LOGLEVEL_VAR(vmhs), \
   LOGLEVEL_VAR(undopoint), \
   LOGLEVEL_VAR(ipc), \
   LOGLEVEL_VAR(smbios), \
   LOGLEVEL_VAR(acpi), \
   LOGLEVEL_VAR(snapshot), \
   LOGLEVEL_VAR(asyncsocket), \
   LOGLEVEL_VAR(mainMem), \
   LOGLEVEL_VAR(remoteDevice), \
   LOGLEVEL_VAR(vncDecode), \
   LOGLEVEL_VAR(vncEncode), \
   LOGLEVEL_VAR(libconnect), \
   LOGLEVEL_VAR(state3d), \
   LOGLEVEL_VAR(vmGL), \
   LOGLEVEL_VAR(guest_msg), \
   LOGLEVEL_VAR(guest_rpc), \
   LOGLEVEL_VAR(guestVars), \
   LOGLEVEL_VAR(vmkEvent), \
   LOGLEVEL_VAR(battery), \
   LOGLEVEL_VAR(fakeDma), \
   LOGLEVEL_VAR(shader), \
   LOGLEVEL_VAR(numa), \
   LOGLEVEL_VAR(machPoll), \
   LOGLEVEL_VAR(vmWindowController), \
   LOGLEVEL_VAR(dui), \
   LOGLEVEL_VAR(duiMKS), \
   LOGLEVEL_VAR(worker), \
   LOGLEVEL_VAR(duiDevices), \
   LOGLEVEL_VAR(uwt), /* lib/unityWindowTracker */ \
   LOGLEVEL_VAR(cui), \
   LOGLEVEL_VAR(automation), \
   LOGLEVEL_VAR(oemDevice), \
   LOGLEVEL_VAR(cptOps), \


LOGLEVEL_EXTENSION_DECLARE(LOGLEVEL_USER);

#endif /* _LOGLEVEL_USER_H_ */ 
