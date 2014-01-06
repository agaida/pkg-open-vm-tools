/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
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
 * fileLockPrimitive.c --
 *
 *      Portable file locking via Lamport's Bakery algorithm.
 *
 * This implementation does rely upon a remove directory operation to fail
 * if the directory contains any files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/param.h>
#endif
#include "vmware.h"
#include "hostinfo.h"
#include "util.h"
#include "log.h"
#include "str.h"
#include "file.h"
#include "fileLock.h"
#include "fileInt.h"
#include "random.h"
#include "vm_atomic.h"

#include "unicodeTypes.h"
#include "unicodeOperations.h"

#define LOGLEVEL_MODULE main
#include "loglevel_user.h"

#define	LOCK_SHARED	"S"
#define	LOCK_EXCLUSIVE	"X"
#define FILELOCK_PROGRESS_DEARTH 8000 // Dearth of progress time in msec
#define FILELOCK_PROGRESS_SAMPLE 200  // Progress sampling time in msec

static char implicitReadToken;


/*
 *-----------------------------------------------------------------------------
 *
 * Sleeper --
 *
 *	Have the calling thread sleep "for a while". The duration of the
 *	sleep is determined by the count that is passed in. Checks are
 *	also done for exceeding the maximum wait time.
 *
 * Results:
 *	0	slept
 *	EAGAIN	maximum sleep time exceeded
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
Sleeper(LockValues *myValues, // IN/OUT:
        uint32 *loopCount)    // IN/OUT:
{
   uint32 msecSleepTime;

   if ((myValues->msecMaxWaitTime == FILELOCK_TRYLOCK_WAIT) ||
       ((myValues->msecMaxWaitTime != FILELOCK_INFINITE_WAIT) &&
        (myValues->waitTime > myValues->msecMaxWaitTime))) {
      return EAGAIN;
   }

   if (*loopCount <= 20) {
      /* most locks are "short" */
      msecSleepTime = 100;
      *loopCount += 1;
   } else if (*loopCount < 40) {
      /* lock has been around a while, linear back-off */
      msecSleepTime = 100 * (*loopCount - 19);
      *loopCount += 1;
   } else {
      /* WOW! long time... Set a maximum */
      msecSleepTime = 2000;
   }

   myValues->waitTime += msecSleepTime;

   while (msecSleepTime) {
      uint32 sleepTime = (msecSleepTime > 900) ? 900 : msecSleepTime;

      usleep(1000 * sleepTime);

      msecSleepTime -= sleepTime;
   }

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * RemoveLockingFile --
 *
 *	Remove the specified file.
 *
 * Results:
 *	0	success
 *	> 0	failure (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
RemoveLockingFile(ConstUnicode lockDir,   // IN:
                  ConstUnicode fileName)  // IN:
{
   int err;
   Unicode temp;
   Unicode path;

   ASSERT(lockDir);
   ASSERT(fileName);

   temp = Unicode_Append(lockDir, U(DIRSEPS));
   path = Unicode_Append(temp, fileName);
   Unicode_Free(temp);

   err = FileDeletion(path, FALSE);

   if (err != 0) {
      if (err == ENOENT) {
         /* Not there anymore; locker unlocked or timed out */
         err = 0;
      } else {
// XXX unicode "string" in message
         Warning(LGPFX" %s of '%s' failed: %s\n", __FUNCTION__,
                 path, strerror(err));
      }
   }

   Unicode_Free(path);

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileLockMemberValues --
 *
 *	Returns the values associated with lock directory file.
 *
 * Results:
 *	0	Valid lock file; values have been returned
 *	> 0	Lock file problem (errno); values have not been returned
 *
 * Side effects:
 *      The lock file may be deleted if it is invalid
 *
 *-----------------------------------------------------------------------------
 */

int
FileLockMemberValues(ConstUnicode lockDir,     // IN:
                     ConstUnicode fileName,    // IN:
                     char *buffer,             // OUT:
                     uint32 requiredSize,      // IN:
                     LockValues *memberValues) // OUT:
{
   uint32 i;
   FILELOCK_FILE_HANDLE handle;
   uint32 len;
   char *argv[4];
   int err;
   Unicode temp;
   Unicode path;
   FileData fileData;

   ASSERT(lockDir);
   ASSERT(fileName);

   temp = Unicode_Append(lockDir, U(DIRSEPS));
   path = Unicode_Append(temp, fileName);
   Unicode_Free(temp);

   err = FileLockOpenFile(path, O_RDONLY, &handle);

   if (err != 0) {
      /*
       * A member file may "disappear" if is deleted due to an unlock
       * immediately after a directory scan but before the scan is processed.
       * Since this is a "normal" thing ENOENT will be suppressed.
       */

      if (err != ENOENT) {
// XXX unicode "string" in message
         Warning(LGPFX" %s open failure on '%s': %s\n", __FUNCTION__,
                 path, strerror(err));
      }

      goto bail;
   }

   /* Attempt to obtain the lock file attributes now that it is opened */
   err = FileAttributes(path, &fileData);

   if (err != 0) {
// XXX unicode "string" in message
      Warning(LGPFX" %s file size failure on '%s': %s\n", __FUNCTION__, path,
              strerror(err));

      FileLockCloseFile(handle);

      goto bail;
   }

   /* Complain if the lock file is not the proper size */
   if (fileData.fileSize != requiredSize) {
// XXX unicode "string" in message
      Warning(LGPFX" %s file '%s': size %"FMT64"u, required size %u\n",
              __FUNCTION__, path, fileData.fileSize, requiredSize);

      FileLockCloseFile(handle);

      goto corrupt;
   }

   /* Attempt to read the lock file data and validate how much was read. */
   err = FileLockReadFile(handle, buffer, requiredSize, &len);

   FileLockCloseFile(handle);

   if (err != 0) {
// XXX unicode "string" in message
      Warning(LGPFX" %s read failure on '%s': %s\n",
              __FUNCTION__, path, strerror(err));

      goto bail;
   }

   if (len != requiredSize) {
// XXX unicode "string" in message
      Warning(LGPFX" %s read length issue on '%s': %u and %u\n",
              __FUNCTION__, path, len, requiredSize);

      err = EIO;
      goto bail;
   }

   /* Extract and validate the lock file data. */
   for (i = 0; i < 4; i++) {
      argv[i] = strtok((i == 0) ? buffer : NULL, " ");

      if (argv[i] == NULL) {
         Warning(LGPFX" %s mandatory argument %u is missing!\n",
                 __FUNCTION__, i);

         goto corrupt;
      }
   }

   memberValues->payload = strtok(NULL, " ");

   if (sscanf(argv[2], "%u", &memberValues->lamportNumber) != 1) {
      Warning(LGPFX" %s Lamport number conversion error\n",
              __FUNCTION__);

      goto corrupt;
   }

   if ((strcmp(argv[3], LOCK_SHARED) != 0) &&
       (strcmp(argv[3], LOCK_EXCLUSIVE) != 0)) {
      Warning(LGPFX" %s unknown lock type '%s'\n", __FUNCTION__, argv[3]);

      goto corrupt;
   }

   memberValues->machineID = argv[0];
   memberValues->executionID = argv[1];
   memberValues->lockType = argv[3];
   memberValues->memberName = Unicode_Duplicate(fileName);

   Unicode_Free(path);

   return 0;

corrupt:
// XXX unicode "string" in message
   Warning(LGPFX" %s removing problematic lock file '%s'\n", __FUNCTION__,
           path);

   /* Remove the lock file and behave like it has disappeared */
   err = FileDeletion(path, FALSE);

   if (err == 0) {
      err = ENOENT;
   }

bail:
   Unicode_Free(path);

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileLockValidName --
 *
 *	Validate the format of the file name.
 *
 * Results:
 *	TRUE	yes
 *	FALSE	No
 *
 * Side effects:
 *	None
 *
 *-----------------------------------------------------------------------------
 */

Bool
FileLockValidName(ConstUnicode fileName) // IN:
{
   uint32 i;

   ASSERT(fileName);

   /* The fileName must start with the ASCII character, 'M', 'D' or 'E' */
   if (Unicode_FindSubstrInRange(U("MDE"), 0, -1, fileName, 0,
                                 1) == UNICODE_INDEX_NOT_FOUND) {
      return FALSE;
   }

   /* The fileName must contain 5 ASCII digits after the initial character */
   for (i = 0; i < 5; i++) {
      if (Unicode_FindSubstrInRange(U("0123456789"), 0, -1, fileName, i + 1,
                                    1) == UNICODE_INDEX_NOT_FOUND) {
         return FALSE;
      }
   }

   /* The fileName must terminate with the appropriate suffix string */
   return Unicode_EndsWith(fileName, U(FILELOCK_SUFFIX));
}


/*
 *-----------------------------------------------------------------------------
 *
 * ActivateLockList
 *
 *	Insure a lock list entry exists for the lock directory.
 *
 * Results:
 *	0	success
 *	> 0	error (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
ActivateLockList(ConstUnicode dirName,  // IN:
                 LockValues *myValues)  // IN:
{
   ActiveLock   *ptr;

   ASSERT(dirName);

   ASSERT(Unicode_StartsWith(dirName, U("D")));

   /* Search the list for a matching entry */
   for (ptr = myValues->lockList; ptr != NULL; ptr = ptr->next) {
      if (Unicode_Compare(ptr->dirName, dirName) == 0) {
         break;
      }
   }

   /* No entry? Attempt to add one. */
   if (ptr == NULL) {
      ptr = malloc(sizeof *ptr);

      if (ptr == NULL) {
         return ENOMEM;
      }

      ptr->next = myValues->lockList;
      myValues->lockList = ptr;

      ptr->age = 0;
      ptr->dirName = Unicode_Duplicate(dirName);
   }

   /* Mark the entry (exists) */
   ptr->marked = TRUE;

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ScanDirectory --
 *
 *	Call the specified function for each member file found in the
 *	specified directory.
 *
 * Results:
 *	0	success
 *	> 0	failure
 *
 * Side effects:
 *	Anything that this not a valid locking file is deleted.
 *
 *-----------------------------------------------------------------------------
 */

static int
ScanDirectory(ConstUnicode lockDir,     // IN:
              int (*func)(              // IN:
                     ConstUnicode lockDir,
                     ConstUnicode fileName,
                     LockValues *memberValues,
                     LockValues *myValues
                   ),
              LockValues *myValues,    // IN:
              Bool cleanUp)            // IN:
{
   uint32 i;
   int    err;
   int    numEntries;

   char   **fileList = NULL;

   ASSERT(lockDir);

   numEntries = File_ListDirectory(lockDir, &fileList);

   if (numEntries == -1) {
// XXX unicode "string" in message
      Log(LGPFX" %s: Could not read the directory '%s'.\n",
          __FUNCTION__, lockDir);

      return EDOM;	// out of my domain
   }

   /* Pass 1: Validate entries and handle any 'D' entries */
   for (i = 0, err = 0; i < numEntries; i++) {
      /* Remove any non-locking files */
      if (!FileLockValidName(fileList[i])) {
// XXX unicode "string" in message
         Log(LGPFX" %s discarding %s from %s'; invalid file name.\n",
             __FUNCTION__, fileList[i], lockDir);

         err = RemoveLockingFile(lockDir, fileList[i]);
         if (err != 0) {
            goto bail;
         }

        free(fileList[i]);
        fileList[i] = NULL;

        continue;
      }

      /*
       * Any lockers appear to be entering?
       *
       * This should be rather rare. If a locker dies while entering
       * this will cleaned-up.
       */

      if (*fileList[i] == 'D') {
         if (cleanUp) {
            err = ActivateLockList(fileList[i], myValues);
            if (err != 0) {
               goto bail;
            }
        }

        free(fileList[i]);
        fileList[i] = NULL;
      }
   }

   if (myValues->lockList != NULL) {
      goto bail;
   }

   /* Pass 2: Handle the 'M' entries */
   for (i = 0, err = 0; i < numEntries; i++) {
      LockValues *ptr;
      Bool       myLockFile;
      LockValues memberValues;
      char       buffer[FILELOCK_DATA_SIZE];

      if ((fileList[i] == NULL) || (*fileList[i] == 'E')) {
         continue;
      }

      myLockFile = strcmp(fileList[i],
                          myValues->memberName) == 0 ? TRUE : FALSE;

      if (myLockFile) {
         /* It's me! No need to read or validate anything. */
         ptr = myValues;
      } else {
         /* It's not me! Attempt to extract the member values. */
         err = FileLockMemberValues(lockDir, fileList[i], buffer,
                                    FILELOCK_DATA_SIZE, &memberValues);

         if (err != 0) {
            if (err == ENOENT) {
               err = 0;
               /* Not there anymore; locker unlocked or timed out */
               continue;
            }

            break;
         }

         Unicode_Free(memberValues.memberName);

         /* Remove any stale locking files */
         if (FileLockMachineIDMatch(myValues->machineID,
                                    memberValues.machineID) &&
             !FileLockValidOwner(memberValues.executionID,
                                 memberValues.payload)) {
// XXX unicode "string" in message
            Log(LGPFX" %s discarding %s from %s'; invalid executionID.\n",
                __FUNCTION__, fileList[i], lockDir);

            err = RemoveLockingFile(lockDir, fileList[i]);
            if (err != 0) {
               break;
            }

            continue;
         }

         ptr = &memberValues;
      }

      /* Locking file looks good; see what happens */
      err = (*func)(lockDir, fileList[i], ptr, myValues);
      if (err != 0) {
         break;
      }
   }

bail:

   for (i = 0; i < numEntries; i++) {
      free(fileList[i]);
   }

   free(fileList);

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Scanner --
 *
 *	Call the specified function for each member file found in the
 *	specified directory. If a rescan is necessary check the list
 *	of outstanding locks and handle removing stale locks.
 *
 * Results:
 *	0	success
 *	> 0	failure
 *
 * Side effects:
 *	None
 *
 *-----------------------------------------------------------------------------
 */

static int
Scanner(ConstUnicode lockDir,    // IN:
        int (*func)(             // IN:
               ConstUnicode lockDir,
               ConstUnicode fileName,
               LockValues *memberValues,
               LockValues *myValues
            ),
        LockValues *myValues,    // IN:
        Bool cleanUp)            // IN:
{
   int        err;
   ActiveLock *ptr;

   ASSERT(lockDir);

   myValues->lockList = NULL;

   while (TRUE) {
      ActiveLock *prev;

      err = ScanDirectory(lockDir, func, myValues, cleanUp);
      if ((err > 0) || ((err == 0) && (myValues->lockList == NULL))) {
         break;
      }

      prev = NULL;
      ptr = myValues->lockList;

      /*
       * Some 'D' entries have persisted. Age them and remove those that
       * have not progressed. Remove those that have disappeared.
       */

      while (ptr != NULL) {
         Bool remove;

         if (ptr->marked) {
            if (ptr->age > FILELOCK_PROGRESS_DEARTH) {
               Unicode temp;
               Unicode path;
               UnicodeIndex index;

               ASSERT(Unicode_StartsWith(ptr->dirName, U("D")));

// XXX unicode "string" in message
               Log(LGPFX" %s discarding %s data from '%s'.\n",
                   __FUNCTION__, ptr->dirName, lockDir);

               temp = Unicode_Append(lockDir, U(DIRSEPS));
               path = Unicode_Append(temp, ptr->dirName);
               Unicode_Free(temp);

               index = Unicode_FindLast(path, U("D"));
               ASSERT(index != UNICODE_INDEX_NOT_FOUND);

               temp = Unicode_Replace(path, index, 1, U("M"));
               FileDeletion(temp, FALSE);
               Unicode_Free(temp);

               temp = Unicode_Replace(path, index, 1, U("E"));
               FileDeletion(temp, FALSE);
               Unicode_Free(temp);

               FileRemoveDirectory(path);

               Unicode_Free(path);

               remove = TRUE;
            } else {
               ptr->marked = FALSE;
               ptr->age += FILELOCK_PROGRESS_SAMPLE;

               remove = FALSE;
            }
         } else {
            remove = TRUE;
         }

         if (remove) {
            if (prev == NULL) {
               myValues->lockList = ptr->next;
            } else {
               prev->next = ptr->next;
            }
         }

         prev = ptr;
         ptr = ptr->next;
      }

      usleep(FILELOCK_PROGRESS_SAMPLE * 1000); // relax
   }

   // Clean up anything still on the list; they are no longer important
   while (myValues->lockList != NULL) {
      ptr = myValues->lockList;
      myValues->lockList = ptr->next;

      Unicode_Free(ptr->dirName);

      free(ptr);
   }

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileUnlockIntrinsic --
 *
 *	Release a lock on a file.
 *
 *	The locker is required to identify themselves in a "universally
 *	unique" manner. This is done via two parameters:
 *
 *	machineID --
 *		This a machine/hardware identifier string.
 *
 *		The MAC address of a hardware Ethernet, a WWN of a
 *		hardware FibreChannel HBA, the UUID of an Infiniband HBA
 *		and a machine serial number (e.g. Macs) are all good
 *		candidates for a machine identifier.
 *
 *	executionID --
 *		This is an string which differentiates one thread of
 *		execution from another within the host OS. In a
 *		non-threaded environment this can simply be some form
 *		of process identifier (e.g. getpid() on UNIXen or
 *		_getpid() on Windows). When a process makes use of
 *		threads AND more than one thread may perform locking
 *		this identifier must discriminate between all threads
 *		within the process.
 *
 *	All of the ID strings must encode their respective information
 *	such that any OS may utilize the strings as part of a file name.
 *	Keep them short and, at a minimum, do not use ':', '/', '\', '.'
 *	and white space characters.
 *
 * Results:
 *	0	unlocked
 *	> 0	errno
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

int
FileUnlockIntrinsic(const char *machineID,    // IN:
                    const char *executionID,  // IN:
                    ConstUnicode pathName,    // IN:
                    const void *lockToken)    // IN:
{
   int err;

   ASSERT(machineID);
   ASSERT(executionID);
   ASSERT(pathName);
   ASSERT(lockToken);

// XXX unicode "string" in message
   LOG(1, ("Releasing lock on %s (%s, %s).\n", pathName,
       machineID, executionID));

   if (lockToken == &implicitReadToken) {
      /*
       * The lock token is the fixed-address implicit read lock token.
       * Since no lock file was created no further action is required.
       */

      err = 0;
   } else {
      Unicode dirPath;

      /* The lock directory path */
      dirPath = Unicode_Append(pathName, U(FILELOCK_SUFFIX));

      /*
       * The lock token is the (unicode) path of the lock file.
       *
       * TODO: under vmx86_debug validate the contents of the lock file as
       *       matching the machineID and executionID.
       */

      err = FileDeletion((Unicode) lockToken, FALSE);

      if (err && vmx86_debug) {
// XXX unicode "string" in message
         Log(LGPFX" %s failed for '%s': %s\n",
             __FUNCTION__, (Unicode) lockToken, strerror(err));
      }

      /*
       * The lockToken (a unicode path) was allocated in FileLockIntrinsic
       * and returned to the caller hidden behind a "void *" pointer.
       */

      Unicode_Free((Unicode) lockToken);

      FileRemoveDirectory(dirPath); // just in case we can clean up

      Unicode_Free(dirPath);
   }

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaitForPossession --
 *
 *	Wait until the caller has a higher priority towards taking
 *	possession of a lock than the specified file.
 *
 * Results:
 *	0	success
 *	> 0	error (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
WaitForPossession(ConstUnicode lockDir,     // IN:
                  ConstUnicode fileName,    // IN:
                  LockValues *memberValues, // IN:
                  LockValues *myValues)     // IN:
{
   int err = 0;

   ASSERT(lockDir);
   ASSERT(fileName);

   /* "Win" or wait? */
   if (((memberValues->lamportNumber < myValues->lamportNumber) ||
       ((memberValues->lamportNumber == myValues->lamportNumber) &&
          (Unicode_Compare(memberValues->memberName,
                           myValues->memberName) < 0))) &&
        ((strcmp(memberValues->lockType, LOCK_EXCLUSIVE) == 0) ||
         (strcmp(myValues->lockType, LOCK_EXCLUSIVE) == 0))) {
      Unicode temp;
      Unicode path;
      uint32 loopCount;
      Bool   thisMachine; 

      thisMachine = FileLockMachineIDMatch(myValues->machineID,
                                           memberValues->machineID);

      loopCount = 0;

      temp = Unicode_Append(lockDir, U(DIRSEPS));
      path = Unicode_Append(temp, fileName);
      Unicode_Free(temp);

      while ((err = Sleeper(myValues, &loopCount)) == 0) {
         /* still there? */
         err = FileAttributes(path, NULL);
         if (err != 0) {
            if (err == ENOENT) {
               /* Not there anymore; locker unlocked or timed out */
               err = 0;
            }

            break;
         }

         /* still valid? */
         if (thisMachine && !FileLockValidOwner(memberValues->executionID,
                                                memberValues->payload)) {
            /* Invalid Execution ID; remove the member file */
// XXX unicode "string" in message
            Warning(LGPFX" %s discarding file '%s'; invalid executionID.\n",
                    __FUNCTION__, path);

            err = RemoveLockingFile(lockDir, fileName);
            break;
         }
      }

      /*
       * Log the disposition of each timeout for all non "try lock" locking
       * attempts. This can assist in debugging locking problems.
       */

      if ((myValues->msecMaxWaitTime != FILELOCK_TRYLOCK_WAIT) &&
          (err == EAGAIN)) {
         if (thisMachine) {
// XXX unicode "string" in message
            Log(LGPFX" %s timeout on '%s' due to a local process (%s)\n",
                    __FUNCTION__, path, memberValues->executionID);
         } else {
// XXX unicode "string" in message
            Log(LGPFX" %s timeout on '%s' due to another machine (%s)\n",
                    __FUNCTION__, path, memberValues->machineID);
         }
      }

      Unicode_Free(path);
   }

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * NumberScan --
 *
 *	Determine the maxmimum number value within the current locking set.
 *
 * Results:
 *	0	success
 *	> 0	failure (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
NumberScan(ConstUnicode lockDir,      // IN:
           ConstUnicode fileName,     // IN:
           LockValues *memberValues,  // IN:
           LockValues *myValues)      // IN/OUT:
{
   ASSERT(lockDir);
   ASSERT(fileName);

   if (memberValues->lamportNumber > myValues->lamportNumber) {
      myValues->lamportNumber = memberValues->lamportNumber;
   }

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SimpleRandomNumber --
 *
 *	Return a random number in the range of 0 and 2^16-1.
 *
 * Results:
 *	Random number is returned.
 *
 * Side Effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static uint32
SimpleRandomNumber(const char *machineID,   // IN:
                   const char *executionID) // IN:
{
   static Atomic_Ptr atomic; /* Implicitly initialized to NULL. --mbellon */
   char *context;

   context = Atomic_ReadPtr(&atomic);

   if (context == NULL) {
      void *p;
      uint32 value = 0;

      /*
       * Use the machineID and executionID to hopefully start each machine
       * and process/thread at a different place in the answer stream.
       */

      while (*machineID) {
         value += *machineID++;
      }

      while (*executionID) {
         value += *executionID++;
      }

      p = Random_QuickSeed(value);

      if (Atomic_ReadIfEqualWritePtr(&atomic, NULL, p)) {
         free(p);
      }

      context = Atomic_ReadPtr(&atomic);
      ASSERT(context);
   }

   return (Random_Quick(context) >> 8) & 0xFFFF;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MakeDirectory --
 *
 *	Create a directory.
 *
 * Results:
 *	0	success
 *	> 0	failure (errno)
 *
 * Side Effects:
 *      File system may be modified.
 *
 *-----------------------------------------------------------------------------
 */

static int
MakeDirectory(ConstUnicode pathName)
{
   int err;

   ASSERT(pathName);

#if !defined(_WIN32)
   mode_t save;

   save = umask(0);
#endif

   err = FileCreateDirectory(pathName);

#if !defined(_WIN32)
   umask(save);
#endif

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CreateEntryDirectory --
 *
 *	Create an entry directory in the specified locking directory.
 *
 *	Due to FileLock_UnlockFile() attempting to remove the locking
 *	directory on an unlock operation (to "clean up" and remove the
 *	locking directory when it is no longer needed), this routine
 *	must carefully handle a number of race conditions to insure the
 *	the locking directory exists and the entry directory is created
 *	within.
 *
 * Results:
 *	0	success
 *	> 0	failure (errno)
 *
 * Side Effects:
 *	On success returns the number identifying the entry directory and
 *	the entry directory path name.
 *
 *-----------------------------------------------------------------------------
 */

static int
CreateEntryDirectory(const char *machineID,    // IN:
                     const char *executionID,  // IN:
                     ConstUnicode lockDir,     // IN:
                     Unicode *entryDirectory,  // OUT:
                     Unicode *entryFilePath,   // OUT:
                     Unicode *memberFilePath,  // OUT:
                     Unicode *memberName)      // OUT:
{
   int err = 0;
   uint32 randomNumber = 0;

   ASSERT(lockDir);

   *entryDirectory = NULL;
   *entryFilePath = NULL;
   *memberFilePath = NULL;
   *memberName = NULL;

   /* Fun at the races */

   while (TRUE) {
      Unicode temp;
      Unicode temp2;
      FileData fileData;
      char string[FILELOCK_OVERHEAD];

      err = FileAttributes(lockDir, &fileData);
      if (err == 0) {
        /* The name exists. Deal with it... */

        if (fileData.fileType == FILE_TYPE_REGULAR) {
           /*
            * It's a file. Assume this is an (active?) old style lock
            * and err on the safe side - don't remove it (and
            * automatically upgrade to a new style lock).
            */

// XXX unicode "string" in message
            Log(LGPFX" %s: '%s' exists; an old style lock file?\n",
                      __FUNCTION__, lockDir);

            err = EAGAIN;
            break;
        }

        if (fileData.fileType != FILE_TYPE_DIRECTORY) {
           /* Not a directory; attempt to remove the debris */
           if (FileDeletion(lockDir, FALSE) != 0) {
// XXX unicode "string" in message
              Warning(LGPFX" %s: '%s' exists and is not a directory.\n",
                      __FUNCTION__, lockDir);

              err = ENOTDIR;
              break;
           }

           continue;
        }
      } else {
         if (err == ENOENT) {
            /* Not there anymore; locker unlocked or timed out */
            err = MakeDirectory(lockDir);

            if ((err != 0) && (err != EEXIST)) {
// XXX unicode "string" in message
               Warning(LGPFX" %s creation failure on '%s': %s\n",
                       __FUNCTION__, lockDir, strerror(err));

               break;
            }
         } else {
// XXX unicode "string" in message
            Warning(LGPFX" %s stat failure on '%s': %s\n",
                    __FUNCTION__, lockDir, strerror(err));

            break;
         }
      }

      /* There is a small chance of collision/failure; grab stings now */
      randomNumber = SimpleRandomNumber(machineID, executionID);

      Str_Sprintf(string, sizeof string, "M%05u%s", randomNumber,
                  FILELOCK_SUFFIX);

      *memberName = Unicode_Alloc(string, STRING_ENCODING_US_ASCII);

      Str_Sprintf(string, sizeof string, "D%05u%s", randomNumber,
                  FILELOCK_SUFFIX);

      temp = Unicode_Append(lockDir, U(DIRSEPS));
      temp2 = Unicode_Alloc(string, STRING_ENCODING_US_ASCII);
      *entryDirectory = Unicode_Append(temp, temp2);
      Unicode_Free(temp);
      Unicode_Free(temp2);

      Str_Sprintf(string, sizeof string, "E%05u%s", randomNumber,
                  FILELOCK_SUFFIX);

      temp = Unicode_Append(lockDir, U(DIRSEPS));
      temp2 = Unicode_Alloc(string, STRING_ENCODING_US_ASCII);
      *entryFilePath = Unicode_Append(temp, temp2);
      Unicode_Free(temp);
      Unicode_Free(temp2);

      temp = Unicode_Append(lockDir, U(DIRSEPS));
      *memberFilePath = Unicode_Append(temp, *memberName);
      Unicode_Free(temp);

      err = MakeDirectory(*entryDirectory);

      if (err == 0) {
         /*
          * The entry directory was safely created. See if a member file
          * is in use (the entry directory is removed once the member file
          * is created). If a member file is in use, choose another number,
          * otherwise the use of the this number is OK.
          *
          * Err on the side of caution... don't want to trash perfectly
          * good member files.
          */

         err = FileAttributes(*memberFilePath, NULL);

         if (err != 0) {
            if (err == ENOENT) {
               err = 0;
               break;
            }

            if (vmx86_debug) {
// XXX unicode "string" in message
               Log(LGPFX" %s stat failure on '%s': %s\n",
                   __FUNCTION__, *memberFilePath, strerror(err));
             }
         }

         FileRemoveDirectory(*entryDirectory);
      } else {
          if (err != EEXIST) {
// XXX unicode "string" in message
             Warning(LGPFX" %s creation failure on '%s': %s\n",
                     __FUNCTION__, *entryDirectory, strerror(err));

             break;
          }
      }

      Unicode_Free(*entryDirectory);
      Unicode_Free(*entryFilePath);
      Unicode_Free(*memberFilePath);
      Unicode_Free(*memberName);

      *entryDirectory = NULL;
      *entryFilePath = NULL;
      *memberFilePath = NULL;
      *memberName = NULL;
   }

   if (err != 0) {
      Unicode_Free(*entryDirectory);
      Unicode_Free(*entryFilePath);
      Unicode_Free(*memberFilePath);
      Unicode_Free(*memberName);

      *entryDirectory = NULL;
      *entryFilePath = NULL;
      *memberFilePath = NULL;
      *memberName = NULL;
   }

   return err;
}

/*
 *-----------------------------------------------------------------------------
 *
 * CreateMemberFile --
 *
 *	Create the member file.
 *
 * Results:
 *	0	success
 *	> 0	failure (errno)
 *
 * Side Effects:
 *	None
 *
 *-----------------------------------------------------------------------------
 */

static int
CreateMemberFile(FILELOCK_FILE_HANDLE entryHandle,  // IN:
                 const LockValues *myValues,        // IN:
                 ConstUnicode entryFilePath,        // IN:
                 ConstUnicode memberFilePath)       // IN:
{
   int err;
   uint32 len;
   char buffer[FILELOCK_DATA_SIZE] = { 0 };

   ASSERT(entryFilePath);
   ASSERT(memberFilePath);

   /* Populate the buffer with appropriate data */
   Str_Sprintf(buffer, sizeof buffer, "%s %s %u %s %s", myValues->machineID,
               myValues->executionID, myValues->lamportNumber,
               myValues->lockType,
               myValues->payload == NULL ? "" : myValues->payload);

   /* Attempt to write the data */
   err = FileLockWriteFile(entryHandle, buffer, sizeof buffer, &len);

   if (err != 0) {
// XXX unicode "string" in message
      Warning(LGPFX" %s write of '%s' failed: %s\n", __FUNCTION__,
              entryFilePath, strerror(err));

      FileLockCloseFile(entryHandle);

      return err;
   }

   err = FileLockCloseFile(entryHandle);

   if (err != 0) {
// XXX unicode "string" in message
      Warning(LGPFX" %s close of '%s' failed: %s\n", __FUNCTION__,
              entryFilePath, strerror(err));

      return err;
   }

   if (len != sizeof buffer) {
// XXX unicode "string" in message
      Warning(LGPFX" %s write length issue on '%s': %u and %"FMTSZ"d\n",
              __FUNCTION__, entryFilePath, len, sizeof buffer);

      return EIO;
   }

   err = FileRename(entryFilePath, memberFilePath);

   if (err != 0) {
// XXX unicode "string" in message
      Warning(LGPFX" %s FileRename of '%s' to '%s' failed: %s\n",
              __FUNCTION__, entryFilePath, memberFilePath,
              strerror(err));

      if (vmx86_debug) {
// XXX unicode "string" in message
         Log(LGPFX" %s FileLockFileType() of '%s': %s\n",
             __FUNCTION__, entryFilePath,
            strerror(FileAttributes(entryFilePath, NULL)));

// XXX unicode "string" in message
         Log(LGPFX" %s FileLockFileType() of '%s': %s\n",
             __FUNCTION__, memberFilePath,
            strerror(FileAttributes(memberFilePath, NULL)));
      }

      return err;
   }

   return 0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FileLockIntrinsic --
 *
 *	Obtain a lock on a file; shared or exclusive access.
 *
 *	Each locker is required to identify themselves in a "universally
 *	unique" manner. This is done via two parameters:
 *
 *	machineID --
 *		This a machine/hardware identifier string.
 *
 *		The MAC address of a hardware Ethernet, a WWN of a
 *		hardware FibreChannel HBA, the UUID of an Infiniband HBA
 *		and a machine serial number (e.g. Macs) are all good
 *		candidates for a machine identifier.
 *
 *		The machineID is "univerally unique", discriminating
 *		between all computational platforms.
 *
 *	executionID --
 *		This is an string which differentiates one thread of
 *		execution from another within the host OS. In a
 *		non-threaded environment this can simply be some form
 *		of process identifier (e.g. getpid() on UNIXen or
 *		_getpid() on Windows). When a process makes use of
 *		threads AND more than one thread may perform locking
 *		this identifier must discriminate between all threads
 *		within the process.
 *
 *	All of the ID strings must encode their respective information
 *	such that any OS may utilize the strings as part of a file name.
 *	Keep them short and, at a minimum, do not use ':', '/', '\', '.'
 *	and white space characters.
 *
 *	msecMaxWaitTime specifies the maximum amount of time, in
 *	milliseconds, to wait for the lock before returning the "not
 *	acquired" status. A value of FILELOCK_TRYLOCK_WAIT is the
 *	equivalent of a "try lock" - the lock will be acquired only if
 *	there is no contention. A value of FILELOCK_INFINITE_WAIT
 *	specifies "waiting forever" to acquire the lock.
 *
 * Results:
 *	NULL	Lock not acquired. Check err.
 *		err	0	Lock Timed Out
 *		err	> 0	errno
 *	!NULL	Lock Acquired. This is the "lockToken" for an unlock.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

void *
FileLockIntrinsic(const char *machineID,    // IN:
                  const char *executionID,  // IN:
                  const char *payload,      // IN:
                  ConstUnicode pathName,    // IN:
                  Bool exclusivity,         // IN:
                  uint32 msecMaxWaitTime,   // IN:
                  int *err)                 // OUT:
{
   FILELOCK_FILE_HANDLE handle;
   LockValues myValues;

   Unicode dirPath = NULL;
   Unicode entryFilePath = NULL;
   Unicode memberFilePath = NULL;
   Unicode entryDirectory = NULL;

   ASSERT(machineID);
   ASSERT(executionID);
   ASSERT(pathName);
   ASSERT(err);

   /* establish our values */
   myValues.machineID = (char *) machineID;
   myValues.executionID = (char *) executionID;
   myValues.payload = (char *) payload;
   myValues.lockType = exclusivity ? LOCK_EXCLUSIVE : LOCK_SHARED;
   myValues.lamportNumber = 0;
   myValues.waitTime = 0;
   myValues.msecMaxWaitTime = msecMaxWaitTime;
   myValues.memberName = NULL;

// XXX unicode "string" in message
   LOG(1, ("Requesting %s lock on %s (%s, %s, %u).\n",
       myValues.lockType, pathName, myValues.machineID,
       myValues.executionID, myValues.msecMaxWaitTime));

   /*
    * Enforce the maximum path length restriction explicitely. Apparently
    * the Windows POSIX routine mappings cannot be trusted to return
    * ENAMETOOLONG when it is appropriate.
    */

   if ((Unicode_LengthInCodeUnits(pathName) +
                                  FILELOCK_OVERHEAD) >= FILE_MAXPATH) {
      *err = ENAMETOOLONG;
      goto bail;
   }

   /* Construct the locking directory path */
   dirPath = Unicode_Append(pathName, U(FILELOCK_SUFFIX));

   /*
    * Attempt to create the locking and entry directories; obtain the
    * entry and member path names.
    */

   *err = CreateEntryDirectory(machineID, executionID, dirPath,
                               &entryDirectory, &entryFilePath,
                               &memberFilePath, &myValues.memberName);

   switch (*err) {
   case 0:
      break;

   case EROFS:
      /* FALL THROUGH */
   case EACCES:
      if (!exclusivity) {
         /*
          * Lock is for read/shared access however the lock directory could
          * not be created. Grant an implicit read lock whenever possible.
          * The address of a private variable will be used for the lock token.
          */

// XXX unicode "string" in message
         Warning(LGPFX" %s implicit %s lock succeeded on '%s'.\n",
                 __FUNCTION__, LOCK_SHARED, pathName);

         *err = 0;
         memberFilePath = &implicitReadToken;
      }

      /* FALL THROUGH */
   default:
      goto bail;
   }

   ASSERT(Unicode_LengthInCodeUnits(memberFilePath) -
          Unicode_LengthInCodeUnits(pathName) <= FILELOCK_OVERHEAD);

   /* Attempt to create the entry file */
   *err = FileLockOpenFile(entryFilePath, O_CREAT | O_WRONLY, &handle);

   if (*err != 0) {
      /* clean up */
      FileRemoveDirectory(entryDirectory);
      FileRemoveDirectory(dirPath);

      goto bail;
   }

   /* what is max(Number[1]... Number[all lockers])? */
   *err = Scanner(dirPath, NumberScan, &myValues, FALSE);

   if (*err != 0) {
      /* clean up */
      FileLockCloseFile(handle);
      FileDeletion(entryFilePath, FALSE);
      FileRemoveDirectory(entryDirectory);
      FileRemoveDirectory(dirPath);

      goto bail;
   }

   /* Number[i] = 1 + max([Number[1]... Number[all lockers]) */
   myValues.lamportNumber++;

   /* Attempt to create the member file */
   *err = CreateMemberFile(handle, &myValues, entryFilePath, memberFilePath);

   /* Remove entry directory; it has done its job */
   FileRemoveDirectory(entryDirectory);

   if (*err != 0) {
      /* clean up */
      FileDeletion(entryFilePath, FALSE);
      FileDeletion(memberFilePath, FALSE);
      FileRemoveDirectory(dirPath);

      goto bail;
   }

   /* Attempt to acquire the lock */
   *err = Scanner(dirPath, WaitForPossession, &myValues, TRUE);

   switch (*err) {
   case 0:
      break;

   case EAGAIN:
      /* clean up */
      FileDeletion(memberFilePath, FALSE);
      FileRemoveDirectory(dirPath);

      /* FALL THROUGH */
   default:
      break;
   }

bail:

   Unicode_Free(dirPath);
   Unicode_Free(entryDirectory);
   Unicode_Free(entryFilePath);
   Unicode_Free(myValues.memberName);

   if (*err != 0) {
      Unicode_Free(memberFilePath);
      memberFilePath = NULL;

      if (*err == EAGAIN) {
         *err = 0; // lock not acquired
      }
   }

   return (void *) memberFilePath;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ScannerVMX --
 *
 *	VMX hack scanner
 *
 * Results:
 *	0	success
 *	> 0	error (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
ScannerVMX(ConstUnicode lockDir,     // IN:
           ConstUnicode fileName,    // IN:
           LockValues *memberValues, // IN:
           LockValues *myValues)     // IN/OUT:
{
   ASSERT(lockDir);
   ASSERT(fileName);

   myValues->lamportNumber++;

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * FileLockHackVMX --
 *
 *	The VMX file delete primitive.
 *
 * Results:
 *	0	unlocked
 *	> 0	errno
 *
 * Side effects:
 *      Changes the host file system.
 *
 * Note:
 *	THIS IS A HORRIBLE HACK AND NEEDS TO BE REMOVED ASAP!!!
 *
 *----------------------------------------------------------------------
 */

int
FileLockHackVMX(const char *machineID,    // IN:
                const char *executionID,  // IN:
                ConstUnicode pathName)    // IN:
{
   int        err;
   LockValues myValues;

   Unicode dirPath = NULL;
   Unicode entryFilePath = NULL;
   Unicode memberFilePath = NULL;
   Unicode entryDirectory = NULL;

   ASSERT(pathName);

// XXX unicode "string" in message
   LOG(1, ("%s on %s (%s, %s).\n", __FUNCTION__, pathName,
       machineID, executionID));

   /* establish our values */
   myValues.machineID = (char *) machineID;
   myValues.executionID = (char *) executionID;
   myValues.lamportNumber = 0;
   myValues.memberName = NULL;

   /* first the locking directory path name */
   dirPath = Unicode_Append(pathName, U(FILELOCK_SUFFIX));

   err = CreateEntryDirectory(machineID, executionID, dirPath,
                              &entryDirectory, &entryFilePath,
                              &memberFilePath, &myValues.memberName);

   if (err != 0) {
      goto bail;
   }

   /* Scan the lock directory */
   err = Scanner(dirPath, ScannerVMX, &myValues, FALSE);

   if (err == 0) {
      /* if no members are valid, clean up */
      if (myValues.lamportNumber == 1) {
         FileDeletion(pathName, FALSE);
      }
   } else {
      if (vmx86_debug) {
// XXX unicode "string" in message
         Warning(LGPFX" %s clean-up failure for '%s': %s\n",
                 __FUNCTION__, pathName, strerror(err));
      }
   }

   /* clean up */
   FileRemoveDirectory(entryDirectory);
   FileRemoveDirectory(dirPath);

bail:

   Unicode_Free(dirPath);
   Unicode_Free(entryDirectory);
   Unicode_Free(entryFilePath);
   Unicode_Free(memberFilePath);
   Unicode_Free(myValues.memberName);

   return err;
}
