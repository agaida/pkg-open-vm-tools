/*
 * Copyright 1998 VMware, Inc.  All rights reserved. 
 *
 *
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
 * vixTools.h --
 *
 *    Vix Tools functionality.
 *
 */

#ifndef __VIX_TOOLS_H__
#define __VIX_TOOLS_H__


struct DblLnkLst_Links;
struct GuestApp_Dict;


typedef void (*VixToolsReportProgramDoneProcType)(const char *requestName,
                                                  VixError err,
                                                  int exitCode,
                                                  int64 pid);

VixError VixTools_Initialize(Bool thisProcessRunsAsRootArg,
                             struct DblLnkLst_Links *globalEventQueue,
                             VixToolsReportProgramDoneProcType reportProgramDoneProc);

void VixTools_SetConsoleUserPolicy(Bool allowConsoleUserOpsParam);

void VixTools_SetRunProgramCallback(VixToolsReportProgramDoneProcType reportProgramDoneProc);

VixError VixTools_ProcessVixCommand(VixCommandRequestHeader *requestMsg,
                                    char *requestName,
                                    size_t maxResultBufferSize,
                                    struct GuestApp_Dict **confDictRef,
                                    char **resultBuffer,
                                    size_t *resultLen,
                                    Bool *deleteResultBufferResult);

/*
 * These are internal procedures that are exposed for the legacy
 * tclo callbacks.
 */
VixError VixToolsRunProgramImpl(char *requestName,
                                char *commandLine,
                                char *commandLineArgs,
                                int runProgramOptions,
                                void *userToken,
                                int64 *pid);

VixError VixTools_GetToolsPropertiesImpl(struct GuestApp_Dict **confDictRef,
                                         char **resultBuffer,
                                         size_t *resultBufferLength);

Bool VixToolsImpersonateUserImpl(char const *credentialTypeStr, 
                                 int credentialType,
                                 char const *password,
                                 void **userToken);

void VixToolsUnimpersonateUser(void *userToken);

void VixToolsLogoutUser(void *userToken);

VixError VixToolsGetUserTmpDir(void *userToken,
                               char **tmpDirPath);

#if IMPLEMENT_SOCKET_MGR
VixError VixToolsSocketConnect(VixCommandRequestHeader *requestMsg,
                               char **result);

VixError VixToolsSocketListen(VixCommandRequestHeader *requestMsg,
                              char **result);

VixError VixToolsSocketAccept(VixCommandRequestHeader *requestMsg,
                              char *testName);

VixError VixToolsSocketSend(VixCommandRequestHeader *requestMsg,
                            char *testName);

VixError VixToolsSocketRecv(VixCommandRequestHeader *requestMsg,
                            char *testName);

VixError VixToolsSocketClose(VixCommandRequestHeader *requestMsg);
#endif // IMPLEMENT_SOCKET_MGR


#endif /* __VIX_TOOLS_H__ */


