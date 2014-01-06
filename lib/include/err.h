/*********************************************************
 * Copyright (C) 2004 VMware, Inc. All rights reserved.
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
 *
 *********************************************************/

/*
 * err.h --
 *
 *      General error handling library
 */

#ifndef _ERR_H_
#define _ERR_H_

#if !_WIN32
#include <errno.h>
#endif

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"


#ifdef __cplusplus
extern "C" {
#endif

#if !_WIN32
   typedef int Err_Number;
#else
   typedef DWORD Err_Number;
#endif

const char *Err_ErrString(void);

const char *Err_Errno2String(Err_Number errorNumber);


/*
 *----------------------------------------------------------------------
 *
 * Err_Errno --
 *
 *      Gets last error in a platform independent way.
 *
 * Results:
 *      Last error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

#if !_WIN32
   #define Err_Errno() errno
#else
   #define Err_Errno() GetLastError()
#endif


/*
 *----------------------------------------------------------------------
 *
 * Err_SetErrno --
 *
 *      Set the last error in a platform independent way.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Yes.
 *
 *----------------------------------------------------------------------
 */

#if !_WIN32
   #define Err_SetErrno(e) (errno = (e))
#else
   #define Err_SetErrno(e) SetLastError(e)
#endif


/*
 *----------------------------------------------------------------------
 *
 * WITH_ERRNO --
 *
 *      Execute "body" with "e" bound to the last error number
 *	and preserving the last error in surrounding code.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Yes.
 *
 *----------------------------------------------------------------------
 */

#define WITH_ERRNO(e, body) do { \
      Err_Number e = Err_Errno(); \
      body; \
      Err_SetErrno(e); \
   } while (FALSE)

#ifdef __cplusplus
}
#endif

#endif
