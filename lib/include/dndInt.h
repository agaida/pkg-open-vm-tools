/* **********************************************************
 * Copyright (C) 2005 VMware, Inc. All Rights Reserved 
 * **********************************************************
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

/*
 * dndInt.h --
 *
 *   Private functions for the Drag and Drop library.
 */

#ifndef __DND_INT_H__
#define __DND_INT_H__

#include "vm_basic_types.h"

Bool DnDDataContainsIllegalCharacters(const char *data,
                                      const size_t dataSize,
                                      const char *illegalChars);
Bool DnDPrependFileRoot(const char *fileRoot,
                        const char delimiter,
                        char **src,
                        size_t *srcSize);
Bool DnDRootDirUsable(const char *dir);
Bool DnDSetPermissionsOnRootDir(const char *dir);
Bool DnDStagingDirectoryUsable(const char *dir);
Bool DnDSetPermissionsOnStagingDir(const char *dir);

#endif /*  __DND_INT_H__ */
