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

#undef WIN32_LEAN_AND_MEAN

#if defined(_WIN32)
# include <winsock2.h> // also includes windows.h
# include <io.h>
# include <process.h>
# include "coreDump.h"
#endif

#include "vm_ctype.h"
#include "safetime.h"
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>

#if !defined(_WIN32) && !defined(N_PLAT_NLM)
#  if defined(linux)
#    include <sys/syscall.h> // for SYS_gettid
#  endif
#  include <unistd.h>
#  include <pwd.h>
extern int vasprintf(char **ptr, const char *f, va_list arg);
#endif

#if __APPLE__
#include <pthread.h>
#endif

#include "vmware.h"
#include "msg.h"
#include "util.h"
#include "str.h"
/* For HARD_EXPIRE --hpreg */
#include "vm_version.h"
#include "su.h"
#include "file.h"
#include "util_shared.h"
#include "escape.h"
#include "base64.h"
#include "hostType.h"

/*
 * ESX with userworld VMX
 */
#if defined(VMX86_SERVER)
#include "user_layout.h"
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

/*
 * Win32 doesn't have an iovec type, so do this here to avoid messy games in
 * the header files.  --Jeremy.
 */

struct UtilVector {
   void *base;
   int len;
};

#if defined(_WIN32)
#include "win32util.h"
static int UtilTokenHasGroup(HANDLE token, SID *group);
#endif


/*
 *----------------------------------------------------------------------
 *
 * Util_Init --
 *
 *	Opportunity to sanity check things
 *
 * Results:
 *	Bool - TRUE (this should never fail)
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

Bool
Util_Init(void)
{
#ifdef VMX86_DEVEL
   /* 
    * Sanity check Str_Snprintf so that we're never thrown off guard
    * by a change in the underlying libraries that Str_Snprintf doesn't
    * catch and wrap properly.
    */
   {
      char buf[2] = { 'x', 'x' };
      int rv;

      rv = Str_Snprintf(buf, sizeof(buf), "a");
      ASSERT(rv == 1);
      ASSERT(!strcmp(buf, "a"));

      rv = Str_Snprintf(buf, sizeof(buf), "ab");
      ASSERT(rv == -1);
      ASSERT(!strcmp(buf, "a"));
   }
#endif
   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * Util_Checksum32 --
 *
 * Checksums a uint32 aligned block by progressive XOR.  Basically parity
 * checking of each bit position.
 *
 *----------------------------------------------------------------------
 */

uint32
Util_Checksum32(uint32 *buf, int len)
{
   uint32 checksum = 0;
   int i;

   ASSERT((len % 4) == 0);
   for (i = 0; i < len; i+=4) checksum ^= *(buf++);
   return checksum;
}


/*
 *----------------------------------------------------------------------
 *
 * Util_Checksum --
 *
 * Checksums a block by progressive XOR.  Basically parity
 * checking of each bit position.
 *
 *----------------------------------------------------------------------
 */

uint32
Util_Checksum(uint8 *buf, int len)
{
   uint32 checksum;
   int remainder;

   remainder = len % 4;
   len -= remainder;

   checksum = Util_Checksum32((uint32 *)buf, len);

   if (remainder) {
      uint32 scratch = 0;
      uint8 *ptr = buf + len;

      switch (remainder) {
         case 1: scratch = (*(uint8 *) ptr)<<24; break;
         case 2: scratch = (*(uint16 *) ptr)<<16; break;
         case 3: scratch = (*(uint8 *) ptr)<<24;
                 scratch |= (*(uint16 *) (ptr+1))<<8; break;
      }
      checksum ^= scratch;
   }
   return checksum;
}


/*
 *----------------------------------------------------------------------
 *
 * Util_Checksumv --
 *
 * Checksums an iovector by progressive XOR.  Basically parity checking of
 * each bit position.
 *
 *----------------------------------------------------------------------
 */

uint32
Util_Checksumv(void *iov,      // IN
               int numEntries) // IN
{
   uint32 checksum = 0;
   struct UtilVector *vector = (struct UtilVector *) iov;

   ASSERT(numEntries > 0);
   do {
      checksum ^= Util_Checksum(vector->base, vector->len);
      vector++;
   } while (--numEntries > 0);
   return checksum;
}


/*
 *----------------------------------------------------------------------
 *
 * Util_ShortenPath --
 *
 *    Check the input path length agains the maxLength parameter. If 
 *    the path is too long, prepend "..." and shorten it to the 
 *    appropriate length.
 *
 *----------------------------------------------------------------------
 */

void
Util_ShortenPath(char *dst,       // OUT
                 const char *src, // IN
                 int maxLength)   // IN
{
   if (strlen(src) >= maxLength - 4) {
      Str_Strcpy(dst, "...", maxLength);
      Str_Strcat(dst, 
                 src + strlen(src) - maxLength + 4,
                 maxLength);
   } else {
      Str_Strcpy(dst, src, maxLength);   
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Util_LogWrapper --
 *
 *      Adapts the Log function to meet the interface required by backtracing
 *      functions by adding an ignored void* argument.
 *
 * Results:
 *      Same effect as Log(fmt, ...)
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
void
Util_LogWrapper(void *ignored, const char *fmt, ...)
{
   va_list ap;
   char thisLine[UTIL_BACKTRACE_LINE_LEN];
   
   va_start(ap, fmt);
   Str_Vsnprintf(thisLine, UTIL_BACKTRACE_LINE_LEN-1, fmt, ap);
   thisLine[UTIL_BACKTRACE_LINE_LEN-1] = '\0';
   va_end(ap);

   Log("%s", thisLine);
}


/*
 *----------------------------------------------------------------------
 *
 * Util_BacktraceFromPointer --
 *
 *      log the stack backtrace given a frame pointer
 *
 * Results:
 *      
 *      void
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Util_BacktraceFromPointer(uintptr_t *basePtr)
{
   Util_BacktraceFromPointerWithFunc(basePtr, Util_LogWrapper, NULL);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_BacktraceFromPointerWithFunc --
 *
 *      Output a backtrace from the given frame porinter, using
 *      "outputFunc" as the logging function. For each line of the backtrace,
 *      this will call "outputFunc(outputFuncData, fmt, ...)"
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calls outFunc repeatedly.
 *
 *-----------------------------------------------------------------------------
 */

void
Util_BacktraceFromPointerWithFunc(uintptr_t *basePtr,
                                  Util_OutputFunc outFunc,
                                  void *outFuncData)
{
   uintptr_t *x = basePtr;
   int i;

   for (i = 0; i < 256; i++) {
      if (x < basePtr ||
	  (uintptr_t) x - (uintptr_t) basePtr > 0x8000) {
         break;
      }
      outFunc(outFuncData, "Backtrace[%d] %#08x eip %#08x \n", i, x[0], x[1]);
      x = (uintptr_t *) x[0];
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Util_Data2Buffer --
 *
 *	Format binary data for printing
 *
 * Results:
 *	TRUE if all data fits into buffer, FALSE otherwise.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

Bool
Util_Data2Buffer(char *buf,         // OUT
                 size_t bufSize,    // IN
                 const void* data0, // IN
                 size_t dataSize)   // IN
{
   char* cp = buf;
   const uint8* data = (const uint8*) data0;
   size_t n = MIN(dataSize, ((bufSize-1) / 3));

   while (n > 0) {
      Str_Sprintf(cp, 4, " %02X", *data);
      cp += 3;
      data++, n--;
   }
   *cp = '\0';
   return (dataSize <= bufSize);
}


/*
 *----------------------------------------------------------------------
 *
 * Util_Backtrace --
 *
 *      log the stack backtrace for a particular bug number
 *
 * Results:
 *      
 *      void
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Util_Backtrace(int bugNr) // IN
{
   Util_BacktraceWithFunc(bugNr, Util_LogWrapper, NULL);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_BacktraceWithFunc --
 *
 *      Outputs the stack backtrace for the bug "bugNr," using the
 *      log function "outFunc" as described in the comment for
 *      Util_BacktraceFromPointerWithFunc.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calls outFunc several times.
 *
 *-----------------------------------------------------------------------------
 */

void
Util_BacktraceWithFunc(int bugNr, Util_OutputFunc outFunc, void *outFuncData)
{
#if !defined(_WIN32)
   uintptr_t *x = (uintptr_t *) &bugNr;
#endif

   if (bugNr == 0) {
      outFunc(outFuncData, "Backtrace:\n");
   } else {
      outFunc(outFuncData, "Backtrace for bugNr=%d\n",bugNr);
   }
#if defined(_WIN32)
   CoreDump_Backtrace(outFunc, outFuncData);
#else
   Util_BacktraceFromPointerWithFunc(&x[-2], outFunc, outFuncData);
#endif
}

   
#if !defined(_WIN32) && !defined(N_PLAT_NLM)
/*
 *----------------------------------------------------------------------
 *
 * UtilDoTildeSubst --
 *
 *	Given a string following a tilde, this routine returns the
 *	corresponding home directory.
 *
 * Results:
 *	The result is a pointer to a static string containing the home
 *	directory in native format.  The returned string is a newly
 *      allocated string which may/must be freed by the caller
 *
 * Side effects:
 *	Information may be left in resultPtr.
 *
 * Credit: derived from J.K.Ousterhout's Tcl
 *----------------------------------------------------------------------
 */

static char *
UtilDoTildeSubst(char *user)  // IN - name of user
{
   char *dir;
   char *str = NULL;

   if (*user == '\0') {
      dir = getenv("HOME");
      if (dir == NULL) {
         Log("Could not expand environment variable HOME.\n");
      } else {
         str = strdup(dir);
         if (str == NULL) { 
	    MSG_POST_NOMEM();
	 }
      }
   } else {
      struct passwd *pwPtr;
      pwPtr = getpwnam(user);
      if (pwPtr == NULL) {
         endpwent();
         Log("Could not get information for user '%s'.\n",user);
      } else {
         str = strdup(pwPtr->pw_dir);
         if (str == NULL) { 
            MSG_POST_NOMEM();
         }
         endpwent();
      }
   }
   return str;
}
#endif


#ifndef N_PLAT_NLM
#if defined(linux)
/*
 *-----------------------------------------------------------------------------
 *
 * gettid --
 *
 *      Retrieve unique thread identification suitable for kill or setpriority.
 *	Do not call this function directly, use Util_GetCurrentThreadId() instead.
 *
 * Results:
 *      Unique thread identification on success.
 *      (pid_t)-1 on error, errno set (when kernel does not support this call)
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE
pid_t gettid(void)
{
#if defined(SYS_gettid)
   return (pid_t)syscall(SYS_gettid);
#else
   return -1;
#endif
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Util_GetCurrentThreadId --
 *
 *      Retrieves a unique thread identification suitable to identify a thread
 *      to kill it or change its scheduling priority.
 *
 * Results:
 *      Unique thread identification on success.
 *	ASSERTs on failure (should not happen).
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

uint32
Util_GetCurrentThreadId(void)
{
#if defined(linux)
   /*
    * It is possible that two threads can enter gettid() path simultaneously,
    * both eventually clearing useTid to zero. It does not matter - only
    * problem which can happen is that useTid will be set to zero twice.
    * And it has no impact on useTid value...
    */

   static int useTid = 1;
   pid_t tid;

   // ESX with userworld VMX
#if defined(VMX86_SERVER)
   if (HostType_OSIsVMK()) {
      return User_GetTid();
   }
#endif

   if (useTid) {
      tid = gettid();
      if (tid != (pid_t)-1) {
         return tid;
      }
      ASSERT(errno == ENOSYS);
      useTid = 0;
   }
   tid = getpid();
   ASSERT(tid != (pid_t)-1);
   return tid;
#elif __FreeBSD__ || sun
   pid_t tid;

   tid = getpid();
   ASSERT(tid != (pid_t)-1);
   return tid;
#elif __APPLE__
   ASSERT_ON_COMPILE(sizeof(uint32) == sizeof(pthread_t));
   return (uint32)pthread_self();
#else
   return GetCurrentThreadId();
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Util_ExpandString --
 *
 *      converts the strings by expanding "~", "~user" and environment
 *      variables
 *
 * Results:
 *
 *	Return a newly allocated string.  The caller is responsible
 *	for deallocating it.
 *
 *      Return NULL in case of error.
 *
 * Side effects:
 *      memory allocation
 *
 * Bugs:
 *      the handling of enviroment variable references is very
 *	simplistic: there can be only one in a pathname segment
 *	and it must appear last in the string
 *
 *----------------------------------------------------------------------
 */

#define UTIL_MAX_PATH_CHUNKS 100

char *
Util_ExpandString(const char *fileName)
{
   char *copy = NULL;
   char *result = NULL;
   int nchunk = 0;
   char *chunks[UTIL_MAX_PATH_CHUNKS];
   int chunkSize[UTIL_MAX_PATH_CHUNKS];
   Bool freeChunk[UTIL_MAX_PATH_CHUNKS];
   char *cp;
   int i;

   ASSERT(fileName);

   copy = strdup(fileName);
   if (copy == NULL) {
      Msg_Append(MSGID(util.ExpandStringNoMemForCopy)
		 "Cannot allocate memory to expand \"%s\".\n", fileName);
      goto out;
   }

   /*
    * quick exit
    */

   if (fileName[0] != '~' && Str_Strchr(fileName, '$') == NULL) {
      return copy;
   }

   /*
    * Break string into nice chunks for separate expansion.
    *
    * The rule for terminating a ~ expansion is historical.  -- edward
    */

   nchunk = 0;
   for (cp = copy; *cp;) {
      int len;
      if (*cp == '$') {
	 char *p;
	 for (p = cp + 1; isalnum(*p & 0xFF) || *p == '_'; p++) {
	 }
	 len = p - cp;
#if !defined(_WIN32)
      } else if (cp == copy && *cp == '~') {
	 len = strcspn(cp, DIRSEPS);
#endif
      } else {
	 len = strcspn(cp, "$");
      }
      if (nchunk >= UTIL_MAX_PATH_CHUNKS) {
         Msg_Append(MSGID(util.expandStringTooManyChunks)
		    "Filename \"%s\" has too many chunks.\n",
		    fileName);
	 goto out;
      }
      chunks[nchunk] = cp;
      chunkSize[nchunk] = len;
      freeChunk[nchunk] = FALSE;
      nchunk++;
      cp += len;
   }

   /*
    * Expand leanding ~
    */

#if !defined(_WIN32)
   if (chunks[0][0] == '~') {
      char save = (cp = chunks[0])[chunkSize[0]];
      cp[chunkSize[0]] = 0;
      ASSERT(!freeChunk[0]);
      chunks[0] = UtilDoTildeSubst(chunks[0] + 1);
      cp[chunkSize[0]] = save;
      if (chunks[0] == NULL) {
         /* It could not be expanded, therefore leave as original. */
         chunks[0] = cp;
      } else {
         /* It was expanded, so adjust the chunks. */
         chunkSize[0] = strlen(chunks[0]);
         freeChunk[0] = TRUE;
      }
   }
#endif 
   
   /*
    * Expand $
    */

   for (i = 0; i < nchunk; i++) {
      char save;
      char *expand;
      char buf[100];

      cp = chunks[i];

      if (*cp != '$' || chunkSize[i] == 1) {

         /*
          * Skip if the chuck has only the $ character.
          * $ will be kept as a part of the pathname.
          */

	 continue;
      }

      save = cp[chunkSize[i]];
      cp[chunkSize[i]] = 0;

      /*
       * $PID and $USER are interpreted specially.
       * Others are just getenv().
       */

      expand = getenv(cp + 1);
      if (expand != NULL) {
      } else if (strcasecmp(cp + 1, "PID") == 0) {
	 Str_Snprintf(buf, sizeof buf, "%"FMTPID, getpid());
	 expand = buf;
      } else if (strcasecmp(cp + 1, "USER") == 0) {
#if !defined(_WIN32)
	 int uid = getuid();
	 struct passwd *p = getpwuid(uid);
	 if (p != NULL) {
	    expand = p->pw_name;
	 }
#else
	 int n = sizeof buf;
	 if (GetUserName(buf, &n)) {
	    expand = buf;
	 }
#endif
	 else {
	    expand = "unknown";
	 }
      } else {
	 Warning("Environment variable '%s' not defined in '%s'.\n",
		 cp + 1, fileName);
#if !defined(_WIN32)
         /*
          * Strip off the env variable string from the pathname.
          */

	 expand = "";

#else    // _WIN32

         /*
          * The problem is we have really no way to distinguish the caller's
          * intention is a dollar sign ($) is used as a part of the pathname
          * or as an environment variable.
          *
          * If the token does not expand to an environment variable,
          * then assume it is a part of the pathname. Do not strip it
          * off like it is done in linux host (see above)
          *
          * XXX   We should also consider using %variable% convention instead
          *       of $variable for Windows platform.
          */

         Str_Strcpy(buf, cp, 100);
         expand = buf;
#endif
      }

      cp[chunkSize[i]] = save;

      ASSERT(expand != NULL);
      ASSERT(!freeChunk[i]);
      chunks[i] = strdup(expand);
      if (chunks[i] == NULL) {
	 Msg_Append(MSGID(util.ExpandStringNoMemForChunk)
		    "Cannot allocate memory to expand \"%s\" in \"%s\".\n",
		    expand, fileName);
	 goto out;
      }
      chunkSize[i] = strlen(expand);
      freeChunk[i] = TRUE;
   }
 
   /*
    * Put all the chunks back together.
    */

   {
      int size = 1;	// 1 for the terminating null
      for (i = 0; i < nchunk; i++) {
	 size += chunkSize[i];
      }
      result = malloc(size);
   }
   if (result == NULL) {
      Msg_Append(MSGID(util.expandStringNoMemForResult)
		 "Cannot allocate memory for the expansion of \"%s\".\n",
		 fileName);
      goto out;
   }
   cp = result;
   for (i = 0; i < nchunk; i++) {
      memcpy(cp, chunks[i], chunkSize[i]);
      cp += chunkSize[i];
   }
   *cp = 0;

out:
   /*
    * Clean up and return.
    */

   for (i = 0; i < nchunk; i++) {
      if (freeChunk[i]) {
	 free(chunks[i]);
      }
   }
   free(copy);

   return result;
}
#endif /* !defined(N_PLAT_NLM) */


/* XXX This should go in a separate utilPosix.c file --hpreg */
#if !defined(_WIN32) && !defined(N_PLAT_NLM)
/*
 *----------------------------------------------------------------------
 *
 * Util_MakeSureDirExists --
 *
 *    Make sure a directory exists
 *      
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

Bool
Util_MakeSureDirExistsAndAccessible(char const *path,  // IN
				    unsigned int mode) // IN
{
   char *epath;
   struct stat statbuf;
   
   epath = Util_ExpandString(path);
   if (epath == NULL) {
      return FALSE;
   }

   if (stat(epath, &statbuf) == 0) {
      if (! S_ISDIR(statbuf.st_mode)) {
	 Msg_Append(MSGID(util.msde.notDir)
		    "The path \"%s\" exists, but it is not a directory.\n",
		    epath);
	 free(epath);
	 return FALSE;
      }
   } else {
      if (mkdir(epath, mode) != 0) {
	 Msg_Append(MSGID(util.msde.mkdir)
		    "Cannot create directory \"%s\": %s.\n",
		    epath, Msg_ErrString());
	 free(epath);
	 return FALSE;
      }
   }
   if (FileIO_Access(epath, FILEIO_ACCESS_READ | FILEIO_ACCESS_WRITE | FILEIO_ACCESS_EXEC | FILEIO_ACCESS_EXISTS) ==
       FILEIO_ERROR) {
      Msg_Append(MSGID(util.msde.noAccess)
		 "Directory \"%s\" is not accessible: %s.\n",
		 epath, Msg_ErrString());
      free(epath);
      return FALSE;
   }
   free(epath);
   
   return TRUE;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * Util_ExitProcessAbruptly
 *
 *    On Win32, terminate the process and all of its threads, without
 *    calling any of the DLL termination handlers.
 *    On Linux, call exit().
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

void
Util_ExitProcessAbruptly(int code) // IN
{
#if defined(_WIN32)
   TerminateProcess(GetCurrentProcess(),code);
#else
   exit(code);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Util_ExitThread --
 *
 *    Terminate the running thread.
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

void
Util_ExitThread(int code) // IN
{
#if defined(_WIN32)
   ExitThread(code);
#else
   exit(code);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Util_CompareDotted --
 *
 *      Compares two version numbers encoded as dotted strings.
 *
 * Results:
 *      0 if equal, -1 if s1 is less than s2, else 1.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int
Util_CompareDotted(const char *s1, const char *s2)
{
   int i, x[5], y[5];

   for (i = 0; i < 5; i++) {
      x[i] = 0;
      y[i] = 0;
   }

   if (sscanf(s1, "%d.%d.%d.%d.%d", &x[0], &x[1], &x[2], &x[3], &x[4]) < 1) {
      x[0] = 1;
   }
   if (sscanf(s2, "%d.%d.%d.%d.%d", &y[0], &y[1], &y[2], &y[3], &y[4]) < 1) {
      y[0] = 1;
   }

   for (i = 0; i < 5; i++) {
      if (x[i] < y[i]) {
         return -1;
      }
      if (x[i] > y[i]) {
         return 1;
      }
   }

   return 0;
}


#if defined(_WIN32)
/*
 *-----------------------------------------------------------------------------
 *
 * UtilTokenHasGroup --
 *
 *    Determine if the specified token has a particular group
 *
 * Results:
 *    1 if yes
 *    0 if no
 *    <0 on error
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

static int 
UtilTokenHasGroup(HANDLE token, 
		  SID *group)
{
   /*
    * The code is from
    * http://support.microsoft.com/support/kb/articles/Q118/6/26.ASP
    * (HOWTO: Determine Whether a Thread Is Running in User Context of Local
    * Administrator Account), modified as follows:
    *
    * . Removed the exception stuff
    *
    * . Fixed the token handle leak
    *
    * . Used DuplicateToken() which does just what we need instead of
    *   ImpersonateSelf()/RevertToSelf() which does more, and which I believe
    *   will not work if we are already impersonating ourselves
    *
    * . Allocated the SD on the stack
    *
    * . Got rid of the hardcoded DWORD in the computation of the ACL size
    *
    * . Used malloc()/free() instead of Local*()
    *
    * . Added comments
    *
    *  --hpreg
    */

   /*
    * Make up private access rights --hpreg
    */
#define Util_HasAdminPriv_Read  (1 << 0)
#define Util_HasAdminPriv_Write (1 << 1)

   int ret;
   HANDLE iToken;
   SECURITY_DESCRIPTOR sd;
   DWORD aclLen;
   ACL *acl;
   GENERIC_MAPPING gm;
   PRIVILEGE_SET ps;
   DWORD psLen;
   DWORD granted;
   BOOL status;

   iToken = INVALID_HANDLE_VALUE;
   acl = NULL;

   /*
    * Duplicate the token, because AccessCheck() requires an impersonation
    * token --hpreg
    */

   if (DuplicateToken(token, SecurityImpersonation, &iToken) == 0) {
      ret = -3;
      goto end;
   }

   /*
    * Construct a Security Descriptor with a Discretionary Access Control List
    * that contains an Access Control Entry for the administrator group's
    * Security IDentifier --hpreg
    */

   if (InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) == 0) {
      ret = -5;
      goto end;
   }

   /*
    * This magic formula comes from the documentation for
    * InitializeAcl() --hpreg
    */

   aclLen  =    sizeof(ACL)
             + (  sizeof(ACCESS_ALLOWED_ACE)
                - sizeof(((ACCESS_ALLOWED_ACE *)0)->SidStart)
                + GetLengthSid(group));
   acl = malloc(aclLen);
   if (acl == NULL) {
      ret = -6;
      goto end;
   }

   if (InitializeAcl(acl, aclLen, ACL_REVISION) == 0) {
      ret = -7;
      goto end;
   }

   if (AddAccessAllowedAce(acl, ACL_REVISION,
          Util_HasAdminPriv_Read | Util_HasAdminPriv_Write, group) == 0) {
      ret = -8;
      goto end;
   }

   if (SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE) == 0) {
      ret = -9;
      goto end;
   }

   /*
    * Set the owner and group of the SD, because AccessCheck() requires
    * it --hpreg
    */

   if (SetSecurityDescriptorGroup(&sd, group, FALSE) == 0) {
      ret = -10;
      goto end;
   }
   
   if (SetSecurityDescriptorOwner(&sd, group, FALSE) == 0) {
      ret = -11;
      goto end;
   }

   /*
    * Finally, check if the SD grants access to the calling thread --hpreg
    */

   gm.GenericRead    = Util_HasAdminPriv_Read;
   gm.GenericWrite   = Util_HasAdminPriv_Write;
   gm.GenericExecute = 0;
   gm.GenericAll     = Util_HasAdminPriv_Read | Util_HasAdminPriv_Write;

   psLen = sizeof(ps);
   if (AccessCheck(&sd, iToken, Util_HasAdminPriv_Read, &gm, &ps, &psLen,
          &granted, &status) == 0) {
      ret = -12;
      goto end;
   }

   ret = status ? 1 : 0;

end:

   if (iToken != INVALID_HANDLE_VALUE) {
      if (CloseHandle(iToken) == 0 && ret >= 0) {
         ret = -14;
      }
   }

   free(acl);

   return ret;

#undef Util_HasAdminPriv_Read
#undef Util_HasAdminPriv_Write
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_TokenHasAdminPriv --
 *
 *    Determine if the specified token has administrator privileges
 *
 * Results:
 *    1 if yes
 *    0 if no
 *    <0 on error
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

int
Util_TokenHasAdminPriv(HANDLE token)
{
   SID_IDENTIFIER_AUTHORITY sidIdentAuth = SECURITY_NT_AUTHORITY;
   SID *adminGrp = NULL;
   int ret;

   /*
    * Build the Security IDentifier of the administrator group --hpreg
    */

   if (AllocateAndInitializeSid(&sidIdentAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
			        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, 
				&adminGrp) == 0) {
      ret = -4;
      goto end;
   }

   ret = UtilTokenHasGroup(token, adminGrp);

end:
   if (adminGrp) {
      FreeSid(adminGrp);
   }
   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_TokenHasInteractPriv --
 *
 *    Determine if the specified token is logged in interactively.
 *    -- local logons and Remote Desktops logons
 *
 * Results:
 *    1 if yes
 *    0 if no
 *    <0 on error
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

int
Util_TokenHasInteractPriv(HANDLE token)
{
   SID_IDENTIFIER_AUTHORITY sidIdentAuth = SECURITY_NT_AUTHORITY;
   SID *interactiveGrp = NULL;
   int ret;

   /*
    * Build the Security IDentifier of the administrator group --hpreg
    */

   if (AllocateAndInitializeSid(&sidIdentAuth, 1, SECURITY_INTERACTIVE_RID,
			        0, 0, 0, 0, 0, 0, 0, 
				&interactiveGrp) == 0) {
      ret = -4;
      goto end;
   }

   ret = UtilTokenHasGroup(token, interactiveGrp);

end:
   if (interactiveGrp) {
      FreeSid(interactiveGrp);
   }
   return ret;
}

#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Util_HasAdminPriv --
 *
 *    Determine if the calling code has administrator privileges --hpreg
 *
 * Results:
 *    1 if yes
 *    0 if no
 *    <0 on error
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

int
Util_HasAdminPriv(void)
{
#ifdef N_PLAT_NLM
   return 1;
#elif !defined(_WIN32)
   return IsSuperUser() ? 1 : 0;
#else
   HANDLE token = INVALID_HANDLE_VALUE;
   int ret = -1;

   /*
    * Retrieve the access token of the calling thread --hpreg
    *
    * On some machines OpenThreadToken with openAsSelf set to FALSE fails.
    * Empirically, it seems that, in the security context of another user
    * (even when the impersonation token is at SecurityImpersonation level)
    * it is not able to obtain the thread token with TOKEN_DUPLICATE access.
    * Calling OpenThreadToken to open as self is more reliable and does not
    * seem to hurt. -- vui
    */

   if (OpenThreadToken(GetCurrentThread(), TOKEN_DUPLICATE, TRUE,
          &token) == 0) {
      if (GetLastError() != ERROR_NO_TOKEN) {
         ret = -1;
         goto end;
      }

      if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE,
             &token) == 0) {
         ret = -2;
         goto end;
      }
   }

   ret = Util_TokenHasAdminPriv(token);

end:

   if (token != INVALID_HANDLE_VALUE) {
      if (CloseHandle(token) == 0 && ret >= 0) {
         ret = -13;
      }
   }

   return ret;
#endif
}

#if !defined(N_PLAT_NLM)
/*
 *-----------------------------------------------------------------------------
 *
 * Util_DeriveFileName --
 *
 *      This function is admittedly weird.  The basic idea is that we
 *      have a path to a dictionary file, and we need to make a path to
 *      a another file that's named in a similar way to that dictionary
 *      file (e.g., only difference is extension, or filename and
 *      extension).
 *
 *      This function returns a pointer to the result
 *
 * Results:
 *      Pointer to string (on success, caller should free string), 
 *      otherwise NULL.
 *
 * Side effects:
 *      Allocates memory to be freed by caller.
 *
 *-----------------------------------------------------------------------------
 */

char *
Util_DeriveFileName(const char *source, // IN: path to dict file (incl filename)
                    const char *name,   // IN: what to replace filename with (optional)
                    const char *ext)    // IN: what to replace extension with (optional)
{
   char *returnResult = NULL;
   char *path = NULL;
   char *base = NULL;

   if (source == NULL || (name == NULL && ext == NULL)) {
      Warning("invalid use of function\n");
      return NULL;
   }
   File_GetPathName(source, &path, &base);
   if (path == NULL || base == NULL) {
      Warning("Util_DeriveFileName couldn't get path/base\n");
      free(path);
      free(base);
      goto end;
   }

   /* If replacing name and extension */
   if (name != NULL) {
      free(base);

      /*
       * If the "name" we have to append is a relative path (i.e., not an
       * absolute path), then we need to concatenate the "name" to the
       * path of "source". If the path of "source" doesn't exist or is
       * just ".", then we don't need to bother with concatenating results
       * together.
       */

      if (!Util_IsAbsolutePath(name) && strlen(path) > 0 &&
          strcmp(path, ".") != 0) {
	 if (ext == NULL) {
	    returnResult = Str_Asprintf(NULL, "%s%s%s", 
					path, DIRSEPS, name);
	 } else {
	    returnResult = Str_Asprintf(NULL, "%s%s%s.%s", 
				        path, DIRSEPS, name, ext);
	 }
      } else {

	 /* 
          * Path is non-existent or is just the current directory (or the
	  * result from the dictionary is an absolute path), so we
	  * just need to use the filename (using the DIRSEPS method above
          * for a non-existent path might result in something undesireable 
	  * like "\foobar.vmdk")
	  */

	 if (ext == NULL) {
	    returnResult = Str_Asprintf(NULL, "%s", name);
	 } else {
	    returnResult = Str_Asprintf(NULL, "%s.%s", name, ext);
	 }
      }
      free(path);
      return returnResult;
   }

   /* replacing only the file extension */

   /* strip off the existing file extension, if present */
   {
      char *p = Str_Strrchr(base, '.');
      if (p != NULL) {
	 *p = '\0';
      }
   }

   /* Combine disk path with parent path */
   if (strlen(path) > 0 && strcmp(path, ".") != 0) {
      returnResult = Str_Asprintf(NULL, "%s%s%s.%s", 
				  path, DIRSEPS, base, ext);
   } else {

      /* 
       * Path is non-existent or is just the current directory, so we
       * just need to use the filename (using the DIRSEPS method might
       * result in something undesireable like "\foobar.vmdk")
       */
      returnResult = Str_Asprintf(NULL, "%s.%s", base, ext);
   }
   free(path);
   free(base);
end:
   return returnResult;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_CombineStrings --
 *
 *      Takes a vector of strings, and combines them into one string,
 *      where each string is separated by a 0 (zero) byte.
 *
 *      The 0 bytes are then escaped out, and the result is returned.
 *
 * Results:
 *
 *      A NULL terminated string
 *
 * Side effects:
 *      
 *      The result string is allocated
 *
 *-----------------------------------------------------------------------------
 */

char *
Util_CombineStrings(char **sources,             // IN
                    int count)                  // IN
{
   size_t size = 0;
   int index = 0;

   char *combinedString = NULL;
   char *cursor = NULL;
   char *escapedString = NULL;

   int bytesToEsc[256];

   ASSERT(sources != NULL);

   memset(bytesToEsc, 0, sizeof bytesToEsc);
   bytesToEsc[0] = 1;
   bytesToEsc['#'] = 1;

   for (index = 0; index < count; index++) {
      /*
       * Count the size of each string + the delimeter
       */

      size += strlen(sources[index]) + 1;
   }

   combinedString = Util_SafeMalloc(size);

   cursor = combinedString;
   for (index = 0; index < count; index++) {
      memcpy(cursor, sources[index], strlen(sources[index]));
      cursor += strlen(sources[index]);
      cursor[0] = '\0';
      cursor++;
   }

   escapedString = Escape_Do('#', bytesToEsc, combinedString, size, NULL);

   free(combinedString);

   return escapedString;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_SeparateStrings --
 *
 *      Takes as input the result of a call to Util_CombineStrings, and
 *      separates the strings back onto a vector of strings.
 *
 * Results:
 *
 *      A vector of strings, the count will also reflect the number of
 *      entries in the vector
 *
 * Side effects:
 *
 *      The vector is allocated, and each string in the vector must be
 *      freed by the caller
 *
 *-----------------------------------------------------------------------------
 */

char **
Util_SeparateStrings(char *source,              // IN
                     int *count)                // OUT
{
   char *data = NULL;
   size_t dataSize = 0;

   int index = 0;
   char *cursor = NULL;
   char *endCursor = NULL;

   char **stringVector = NULL;

   ASSERT(count != NULL);

   *count = 0;

   data = Escape_Undo('#', source, strlen(source), &dataSize);
   ASSERT(data != NULL);

   endCursor = data + dataSize;
   ASSERT(endCursor[0] == '\0');

   cursor = data;
   while (cursor < endCursor) {
      (*count)++;
      cursor += strlen(cursor) + 1;
   }

   stringVector = Util_SafeMalloc(sizeof(char *) * (*count));

   cursor = data;
   for (index = 0; index < (*count); index++) {
      stringVector[index] = Util_SafeStrdup(cursor);
      cursor += strlen(cursor) + 1;
   }

   free(data);

   return stringVector;
}


#endif /* !defined(N_PLAT_NLM) */
