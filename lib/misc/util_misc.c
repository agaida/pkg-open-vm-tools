/*********************************************************
 * Copyright (C) 1998 VMware, Inc. All rights reserved.
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
 * util.c --
 *
 *    misc util functions
 */

#if defined(_WIN32)
#include <winsock2.h> // also includes windows.h
#include <io.h>
#include <process.h>
#include "win32util.h"
#endif

#include "vm_ctype.h"
#include "safetime.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>

#include "vmware.h"
#include "msg.h"
#include "util.h"
#include "str.h"
#include "file.h"
/* For HARD_EXPIRE --hpreg */
#include "vm_version.h"
#include "su.h"
#include "escape.h"
#include "hostType.h"


/*
 *-----------------------------------------------------------------------------
 *
 * Util_GetCanonicalPath --
 *
 *      Canonicalizes a path name.
 *
 * Results:
 *      A freshly allocated canonicalized path name.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char*
Util_GetCanonicalPath(const char *path) // IN
{
   char *canonicalPath = NULL;
#if defined(__linux__) || defined(__APPLE__)
   char longpath[PATH_MAX];

   if (realpath(path, longpath) == NULL) {
      return NULL;
   }
   canonicalPath = strdup(longpath);
#elif defined(_WIN32)
   char driveSpec[4];
   Bool remoteDrive = FALSE;

   if (!path || !*path) {
      return NULL;
   }

   memcpy(driveSpec, path, 3);
   driveSpec[3] = '\0';

   if (strchr(VALID_DIRSEPS, driveSpec[0]) &&
       strchr(VALID_DIRSEPS, driveSpec[1])) {
      remoteDrive = TRUE;
   } else {
      remoteDrive = (GetDriveTypeA(driveSpec) == DRIVE_REMOTE);
   }

   /*
    * If the path is *potentially* a path to remote share, we do not
    * call GetLongPathName, because if the remote server is unreachable,
    * that function could hang. We sacrifice two things by doing so:
    * 1. The UNC path could refer to the local host and we incorrectly 
    *    assume remote.
    * 2. We do not resolve 8.3 names for remote paths.
    */
   if (remoteDrive) {
      canonicalPath = strdup(path);
   } else {
      canonicalPath = W32Util_GetLongPathName(path);
   }

#else
   NOT_IMPLEMENTED();
#endif
   return canonicalPath;
}


#if defined(_WIN32)
/*
 *-----------------------------------------------------------------------------
 *
 * Util_GetLowerCaseCanonicalPath --
 *
 *      Utility function to both get the canonical version of the input path
 *      and lower case it at the same time.
 *
 * Results:
 *      A lower case freshly allocated canonicalized path name.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char*
Util_GetLowerCaseCanonicalPath(const char* path) // IN
{
   char *ret = Util_GetCanonicalPath(path);
   if (ret != NULL) {
      ret = _strlwr(ret);
   }
   return ret;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Util_CanonicalPathsIdentical --
 *
 *      Utility function to compare two paths that have already been made
 *      canonical. This function exists to mask platform differences in 
 *      path case-sensitivity.
 *
 *      XXX: This implementation makes assumptions about the host filesystem's
 *           case sensitivity without any regard to what filesystem the provided
 *           paths actually use. There are many ways to break this assumption,
 *           on any of our supported host OSes! The return value of this function
 *           cannot be trusted.
 *
 * Results:
 *      TRUE if the paths are equivalenr, FALSE if they are not.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Util_CanonicalPathsIdentical(const char *path1, // IN
                             const char *path2) // IN
{
   ASSERT(path1);
   ASSERT(path2);
#if defined(linux)
   return (strcmp(path1, path2) == 0);
#elif defined(_WIN32)
   return (_stricmp(path1, path2) == 0);
#elif defined(__APPLE__)
   return (strcasecmp(path1, path2) == 0);
#else
   NOT_IMPLEMENTED();
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_IsAbsolutePath --
 *
 *      Checks if the given path is absolute.
 *
 * Results:
 *      TRUE if the path is absolute, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Util_IsAbsolutePath(const char *path)  // IN: path to check
{
#if defined(__linux__) || defined(__APPLE__)
   return path && path[0] == DIRSEPC;
#elif defined(_WIN32)
   if (!path) {
      return FALSE;
   }

   // <Drive letter>:\path
   if (CType_IsAlpha(path[0]) && path[1] == ':' && path[2] == DIRSEPC) {
      return TRUE;
   }

   // UNC paths
   if (path[0] == DIRSEPC && path[1] == DIRSEPC) {
      return TRUE;
   }

   return FALSE;
#else
   NOT_IMPLEMENTED();
#endif
   NOT_REACHED();
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_GetPrime --
 *
 *      Find next prime.
 *
 * Results:
 *      The smallest prime number greater than or equal to n0.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

unsigned
Util_GetPrime(unsigned n0)
{
   unsigned i, ii, n, nn;

   /*
    * Keep the main algorithm clean by catching edge cases here.
    * There is no 32-bit prime larger than 4294967291.
    */

   ASSERT_NOT_IMPLEMENTED(n0 <= 4294967291U);
   if (n0 <= 2) {
      return 2;
   }

   for (n = n0 | 1;; n += 2) {
      /*
       * Run through the numbers 3,5, ..., sqrt(n) and check that none divides
       * n.  We exploit the identity (i + 2)^2 = i^2 + 4i + 4 to incrementially
       * maintain the square of i (and thus save a multiplication each
       * iteration).
       *
       * 65521 is the largest prime below 0xffff, which is where
       * we can stop.  Using it instead of 0xffff avoids overflowing ii.
       */
      nn = MIN(n, 65521U * 65521U);
      for (i = 3, ii = 9;; ii += 4*i+4, i += 2) {
         if (ii > nn) {
            return n;
         }
         if (n % i == 0) {
            break;
         }
      }
   }
}
