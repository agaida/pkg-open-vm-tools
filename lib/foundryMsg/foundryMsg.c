/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. - 
 * *********************************************************
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
 * foundryMsg.c --
 *
 * This is a library for formatting and parsing the messages sent
 * between a foundry client and the VMX. It is a stand-alone library 
 * so it can be used by the VMX tree without also linking in the 
 * entire foundry client-side library.
 */

#include "vmware.h"
#include "vm_version.h"
#include "util.h"
#include "str.h"
#include "base64.h"

#include "vixOpenSource.h"
#include "vixCommands.h"

static char PlainToObfuscatedCharMap[256];
static char ObfuscatedToPlainCharMap[256];

static void VixMsgInitializeObfuscationMapping(void);


/*
 *----------------------------------------------------------------------------
 *
 * VixMsg_AllocResponseMsg --
 *
 *      Allocate and initialize a response message.
 *
 * Results:
 *      The message, with the headers properly initialized.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

VixCommandResponseHeader *
VixMsg_AllocResponseMsg(VixCommandRequestHeader *requestHeader,   // IN
                        VixError error,                           // IN
                        uint32 additionalError,                   // IN
                        size_t responseBodyLength,                // IN
                        void *responseBody,                       // IN
                        size_t *responseMsgLength)                // OUT
{
   char *responseBuffer = NULL;
   VixCommandResponseHeader *responseHeader;
   size_t totalMessageSize;

   ASSERT(responseBodyLength >= 0);
   ASSERT((NULL != responseBody) || (0 == responseBodyLength));

   /*
    * We don't have scatter/gather, so copy everything into one buffer.
    */
   totalMessageSize = sizeof(VixCommandResponseHeader) + responseBodyLength;
   if (totalMessageSize > VIX_COMMAND_MAX_SIZE) {
      /*
       * We don't want to allocate any responses larger than
       * VIX_COMMAND_MAX_SIZE, since the VMX will ignore them.
       * If we hit this ASSERT, we will need to either revise this
       * value, or start packetizing certain commands.
       */
      ASSERT(0);
      return NULL;
   }

   responseBuffer = Util_SafeMalloc(totalMessageSize);
   responseHeader = (VixCommandResponseHeader *) responseBuffer;

   VixMsg_InitResponseMsg(responseHeader,
                          requestHeader,
                          error,
                          additionalError,
                          totalMessageSize);

   if ((responseBodyLength > 0) && (responseBody)) {
      memcpy(responseBuffer + sizeof(VixCommandResponseHeader), 
             responseBody,
             responseBodyLength);
   }

   if (NULL != responseMsgLength) {
      *responseMsgLength = totalMessageSize;
   }

   return responseHeader;
} // VixMsg_AllocResponseMsg


/*
 *----------------------------------------------------------------------------
 *
 * VixMsg_InitResponseMsg --
 *
 *      Initialize a response message.
 *
 * Results:
 *      The message, with the headers properly initialized.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

void
VixMsg_InitResponseMsg(VixCommandResponseHeader *responseHeader,  // IN
                       VixCommandRequestHeader *requestHeader,    // IN
                       VixError error,                            // IN
                       uint32 additionalError,                    // IN
                       size_t totalMessageSize)                   // IN
{
   size_t responseBodyLength;

   ASSERT(NULL != responseHeader);

   responseBodyLength = totalMessageSize - sizeof(VixCommandResponseHeader);
   ASSERT(responseBodyLength >= 0);

   /*
    * Fill in the response header.
    */
   responseHeader->commonHeader.magic = VIX_COMMAND_MAGIC_WORD;
   responseHeader->commonHeader.messageVersion = VIX_COMMAND_MESSAGE_VERSION;
   responseHeader->commonHeader.totalMessageLength = totalMessageSize;
   responseHeader->commonHeader.headerLength = sizeof(VixCommandResponseHeader);
   responseHeader->commonHeader.bodyLength = responseBodyLength;
   responseHeader->commonHeader.credentialLength = 0;
   responseHeader->commonHeader.commonFlags = 0;
   if (NULL != requestHeader) {
      responseHeader->requestCookie = requestHeader->cookie;
   } else {
      responseHeader->requestCookie = 0;
   }
   responseHeader->responseFlags = 0;
   responseHeader->duration = 0xFFFFFFFF;
   responseHeader->error = error;
   responseHeader->additionalError = additionalError;
   responseHeader->errorDataLength = 0;
} // VixMsg_InitResponseMsg


/*
 *-----------------------------------------------------------------------------
 *
 * VixMsg_AllocRequestMsg --
 *
 *      Allocate and initialize a request message.
 *
 * Results:
 *      The message, with the headers properly initialized.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VixCommandRequestHeader *
VixMsg_AllocRequestMsg(size_t msgHeaderAndBodyLength,    // IN
                       int opCode,                    // IN
                       uint64 cookie,                 // IN
                       int credentialType,            // IN
                       const char *userNamePassword)  // IN
{
   size_t totalMessageSize;
   VixCommandRequestHeader *commandRequest = NULL;
   size_t credentialLength = 0;
   size_t namePasswordLength = 0;
   char *destPtr;

   if ((VIX_USER_CREDENTIAL_NAME_PASSWORD == credentialType) 
      || (VIX_USER_CREDENTIAL_HOST_CONFIG_SECRET == credentialType)) {
      /*
       * Both of these are optional.
       */
      if (NULL != userNamePassword) {
         namePasswordLength = strlen(userNamePassword);
         credentialLength += namePasswordLength;
      }
      /*
       * Add 1 to each string to include '\0' for the end of the string.
       */
      credentialLength += 1;
   } else {
      credentialLength = 0;
   }

   totalMessageSize = msgHeaderAndBodyLength + credentialLength;
   if (totalMessageSize > VIX_COMMAND_MAX_REQUEST_SIZE) {
      /*
       * We don't want to allocate any requests larger than
       * VIX_COMMAND_MAX_REQUEST_SIZE, since the VMX will ignore them.
       * If we hit this ASSERT, we will need to either revise this
       * value, or start packetizing certain commands.
       */
      ASSERT(0);
      return NULL;
   }

   commandRequest = (VixCommandRequestHeader *) 
                        Util_SafeCalloc(1, totalMessageSize);

   commandRequest->commonHeader.magic = VIX_COMMAND_MAGIC_WORD;
   commandRequest->commonHeader.messageVersion = VIX_COMMAND_MESSAGE_VERSION;
   commandRequest->commonHeader.totalMessageLength = (uint32)(msgHeaderAndBodyLength + credentialLength);
   commandRequest->commonHeader.headerLength = (uint32)sizeof(VixCommandRequestHeader);
   commandRequest->commonHeader.bodyLength = (uint32)(msgHeaderAndBodyLength - sizeof(VixCommandRequestHeader));
   commandRequest->commonHeader.credentialLength = (uint32)credentialLength;
   commandRequest->commonHeader.commonFlags = VIX_COMMAND_REQUEST;

   commandRequest->opCode = opCode;
   commandRequest->cookie = cookie;
   commandRequest->timeOut = 0xFFFFFFFF;
   commandRequest->requestFlags = 0;

   commandRequest->userCredentialType = credentialType;

   if ((VIX_USER_CREDENTIAL_NAME_PASSWORD == credentialType)
         || (VIX_USER_CREDENTIAL_HOST_CONFIG_SECRET == credentialType)) {
      destPtr = (char *) commandRequest;
      destPtr += commandRequest->commonHeader.headerLength;
      destPtr += commandRequest->commonHeader.bodyLength;
      if (NULL != userNamePassword) {
         Str_Strcpy(destPtr, userNamePassword, namePasswordLength + 1);
         destPtr += namePasswordLength;
      }
      *(destPtr++) = 0;
   }

   return commandRequest;
} // VixMsg_AllocRequestMsg


/*
 *-----------------------------------------------------------------------------
 *
 * VixMsg_ValidateMessage --
 *
 *
 * Results:
 *      VixError
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VixError
VixMsg_ValidateMessage(void *vMsg,       // IN
                       size_t msgLength) // IN
{
   VixMsgHeader *message;

   if ((NULL == vMsg) || (msgLength < sizeof *message)) {
      return VIX_E_INVALID_MESSAGE_HEADER;
   }

   /*
    * Sanity check the header.
    * Some basic rules: All the length values in the VixMsgHeader
    * struct are uint32. The headerLength must be large enough to
    * accomodate the base header: VixMsgHeader. The bodyLength and
    * the credentialLength can be 0. 
    */
   message = vMsg;
   if ((VIX_COMMAND_MAGIC_WORD != message->magic)
         || (message->headerLength < sizeof(VixMsgHeader)) 
         || (message->totalMessageLength < msgLength)
         || (message->totalMessageLength
               < ((uint64)message->headerLength + message->bodyLength + message->credentialLength))
         || (message->totalMessageLength > VIX_COMMAND_MAX_SIZE)
         || (VIX_COMMAND_MESSAGE_VERSION != message->messageVersion)) {
      return VIX_E_INVALID_MESSAGE_HEADER;
   }

   return VIX_OK;
} // VixMsg_ValidateMessage


/*
 *-----------------------------------------------------------------------------
 *
 * VixMsg_ValidateRequestMsg --
 *
 *
 * Results:
 *      VixError
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VixError
VixMsg_ValidateRequestMsg(void *vMsg,       // IN
                          size_t msgLength) // IN
{
   VixError err;
   VixCommandRequestHeader *message;

   err = VixMsg_ValidateMessage(vMsg, msgLength);
   if (VIX_OK != err) {
      return(err);
   }

   /*
    * Sanity check the parts of the header that are specific to requests.
    */
   message = vMsg;
   if (message->commonHeader.headerLength < sizeof(VixCommandRequestHeader)) {
      return VIX_E_INVALID_MESSAGE_HEADER;
   }

   if (message->commonHeader.totalMessageLength > VIX_COMMAND_MAX_REQUEST_SIZE) {
      return VIX_E_INVALID_MESSAGE_HEADER;
   }

   if (!(VIX_COMMAND_REQUEST & message->commonHeader.commonFlags)) {
      return VIX_E_INVALID_MESSAGE_HEADER;
   }

   return VIX_OK;
} // VixMsg_ValidateRequestMsg


/*
 *-----------------------------------------------------------------------------
 *
 * VixMsg_ValidateResponseMsg --
 *
 *
 * Results:
 *      VixError
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

VixError
VixMsg_ValidateResponseMsg(void *vMsg,       // IN
                           size_t msgLength) // IN
{
   VixError err;
   VixCommandResponseHeader *message;

   if ((NULL == vMsg) || (msgLength < sizeof *message)) {
      return VIX_E_INVALID_MESSAGE_HEADER;
   }

   err = VixMsg_ValidateMessage(vMsg, msgLength);
   if (VIX_OK != err) {
      return(err);
   }

   /*
    * Sanity check the parts of the header that are specific to responses.
    */
   message = vMsg;
   if (message->commonHeader.headerLength < sizeof(VixCommandResponseHeader)) {
      return VIX_E_INVALID_MESSAGE_HEADER;
   }

   if (VIX_COMMAND_REQUEST & message->commonHeader.commonFlags) {
      return VIX_E_INVALID_MESSAGE_HEADER;
   }

   return VIX_OK;
} // VixMsg_ValidateResponseMsg



/*
 *-----------------------------------------------------------------------------
 *
 * VixMsgInitializeObfuscationMapping --
 *
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void 
VixMsgInitializeObfuscationMapping(void)
{
   size_t charIndex;
   static Bool initializedTable = FALSE;

   if (initializedTable) {
      return;
   }

   for (charIndex = 0; charIndex < sizeof(PlainToObfuscatedCharMap); charIndex++) {
      PlainToObfuscatedCharMap[charIndex] = 0;
      ObfuscatedToPlainCharMap[charIndex] = 0;
   }

   PlainToObfuscatedCharMap['\\'] = '1';
   PlainToObfuscatedCharMap['\''] = '2';
   PlainToObfuscatedCharMap['\"'] = '3';
   PlainToObfuscatedCharMap[' '] = '4';
   PlainToObfuscatedCharMap['\r'] = '5';
   PlainToObfuscatedCharMap['\n'] = '6';
   PlainToObfuscatedCharMap['\t'] = '7';

   ObfuscatedToPlainCharMap['1'] = '\\';
   ObfuscatedToPlainCharMap['2'] = '\'';
   ObfuscatedToPlainCharMap['3'] = '\"';
   ObfuscatedToPlainCharMap['4'] = ' ';
   ObfuscatedToPlainCharMap['5'] = '\r';
   ObfuscatedToPlainCharMap['6'] = '\n';
   ObfuscatedToPlainCharMap['7'] = '\t';

   initializedTable = TRUE;
} // VixMsgInitializeObfuscationMapping


/*
 *-----------------------------------------------------------------------------
 *
 * VixMsg_ObfuscateNamePassword --
 *
 *       This is NOT ENCRYPTION.
 *
 *       This function does 2 things:
 *          * It removes spaces, quotes and other characters that may make 
 *             parsing params in a string difficult. The name and password is
 *             passed fromt he VMX to the tools through the backdoor as a 
 *             string containing quoted parameters.
 *
 *          * It means that somebody doing a trivial string search on 
 *             host memory won't see a name/password.
 *
 *          This is used ONLY between the VMX and guest through the backdoor.
 *          This is NOT secure.
 *
 * Results:
 *      VixError
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char *
VixMsg_ObfuscateNamePassword(const char *userName,      // IN
                             const char *password)      // IN
{
   char *packedBufferString = NULL;
   char *base64String = NULL;
   char *resultString = NULL;
   size_t resultBufferLength = 0;
   size_t resultLength = 0;
   size_t nameLength = 0;
   size_t passwordLength = 0;
   char *srcPtr;
   char *endSrcPtr;
   char *destPtr;
   size_t base64Length;
   
   if (NULL != userName) {
      nameLength = strlen(userName);
   }
   if (NULL != password) {
      passwordLength = strlen(password);
   }
   /*
    * Leave space for null terminating characters.
    */
   resultLength = nameLength + 1 + passwordLength + 1;

   /*
    * Expand it to make space for base64 encoding, and escaping some characters.
    */
   resultBufferLength = resultLength * 3;
   packedBufferString = Util_SafeMalloc(resultBufferLength + 1);
   destPtr = packedBufferString;
   if (NULL != userName) {
      Str_Strcpy(destPtr, userName, nameLength + 1);
      destPtr += nameLength;
   }
   *(destPtr++) = 0;
   if (NULL != password) {
      Str_Strcpy(destPtr, password, passwordLength + 1);
      destPtr += passwordLength;
   }
   *(destPtr++) = 0;

   base64String = Util_SafeMalloc(resultBufferLength + 1);
   if (!(Base64_Encode((uint8 const *) packedBufferString,
                       destPtr - packedBufferString,
                       base64String, 
                       resultBufferLength,
                       &base64Length))) {
      goto abort;
   }

   /*
    * Now, escape problematic characters.
    */
   VixMsgInitializeObfuscationMapping();
   resultString = Util_SafeMalloc(resultBufferLength + 1);
   destPtr = resultString;
   srcPtr = base64String;
   endSrcPtr = base64String + base64Length;
   while ( srcPtr < endSrcPtr )
   {
      if (PlainToObfuscatedCharMap[(int) (*srcPtr)]) {
         *(destPtr++) = '\\';
         *(destPtr++) = PlainToObfuscatedCharMap[(int) (*srcPtr)];
      } else {
         *(destPtr++) = *srcPtr;
      }

      srcPtr++;
   }
   *destPtr = 0;

abort:
   free(packedBufferString);
   free(base64String);

   return(resultString);
} // VixMsg_ObfuscateNamePassword


/*
 *-----------------------------------------------------------------------------
 *
 * VixMsg_DeObfuscateNamePassword --
 *
 *       This reverses VixMsg_ObfuscateNamePassword. 
 *       See the notes for that procedure.
 *
 * Results:
 *      VixError
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
VixMsg_DeObfuscateNamePassword(const char *packagedName,   // IN
                               char **userNameResult,      // OUT
                               char **passwordResult)      // OUT
{
   Bool success = FALSE;
   char *base64String = NULL;
   char *packedString = NULL;
   char *srcPtr;
   char *destPtr;
   size_t base64Length;
   size_t packedStringLength;

   
   /*
    * Remove escaped special characters.
    * Do this in a private copy because we will change the string in place.
    */
   VixMsgInitializeObfuscationMapping();
   base64String = Util_SafeStrdup(packagedName);
   destPtr = base64String;
   srcPtr = base64String;
   while ( *srcPtr ) {
      if ( '\\' == *srcPtr ) {
         srcPtr++;
         *(destPtr++) = ObfuscatedToPlainCharMap[(int) (*srcPtr)];
      } else {
         *(destPtr++) = *srcPtr;
      }
      srcPtr++;
   }
   *destPtr = 0;
   base64Length = strlen(base64String);

   packedString = Util_SafeMalloc(base64Length + 1);
   if (!Base64_Decode(base64String, packedString, base64Length, &packedStringLength)) {
      goto abort;
   }
   
   srcPtr = packedString;
   if (NULL != userNameResult) {
      *userNameResult = Util_SafeStrdup(srcPtr);
   }
   srcPtr = srcPtr + strlen(srcPtr);
   srcPtr++;
   if (NULL != passwordResult) {
      *passwordResult = Util_SafeStrdup(srcPtr);
   }
   success = TRUE;

abort:
   free(base64String);
   free(packedString);

   return(success);
} // VixMsg_DeObfuscateNamePassword


/*
 *-----------------------------------------------------------------------------
 *
 * VixMsg_EncodeString --
 *
 *       This makes a string safe to pass over a backdoor Tclo command as a 
 *       string. It base64 encodes a string, which removes quote, space,
 *       backslash, and other characters. This will also allow us to pass
 *       UTF-8 strings.
 *
 * Results:
 *      VixError
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char *
VixMsg_EncodeString(const char *str)  // IN
{
   char *base64String = NULL;
   char *resultString = NULL;
   size_t resultBufferLength = 0;
   size_t strLength = 0;
   char *srcPtr;
   char *endSrcPtr;
   char *destPtr;
   size_t base64Length;
   
   if (NULL == str) {
      str = "";
   }
   strLength = strlen(str);

   resultBufferLength = (strLength + 1) * 3;
   base64String = Util_SafeMalloc(resultBufferLength + 1);
   if (!(Base64_Encode((uint8 const *) str,
                       strLength,
                       base64String, 
                       resultBufferLength,
                       &base64Length))) {
      goto abort;
   }

   VixMsgInitializeObfuscationMapping();
   resultString = Util_SafeMalloc(resultBufferLength + 1);
   destPtr = resultString;
   srcPtr = base64String;
   endSrcPtr = base64String + base64Length;

   /*
    * Start with the character-set type. 
    *   'a' means ASCII.
    */
   *(destPtr++) = 'a';

   /*
    * Now, escape problematic characters.
    */
   while ( srcPtr < endSrcPtr )
   {
      if (PlainToObfuscatedCharMap[(int) (*srcPtr)]) {
         *(destPtr++) = '\\';
         *(destPtr++) = PlainToObfuscatedCharMap[(int) (*srcPtr)];
      } else {
         *(destPtr++) = *srcPtr;
      }

      srcPtr++;
   }
   *destPtr = 0;

abort:
   free(base64String);

   return(resultString);
} // VixMsg_EncodeString


/*
 *-----------------------------------------------------------------------------
 *
 * VixMsg_DecodeString --
 *
 *       This reverses VixMsg_EncodeString. 
 *       See the notes for that procedure.
 *
 * Results:
 *      VixError
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char *
VixMsg_DecodeString(const char *str)   // IN
{
   char *base64String = NULL;
   char *resultStr = NULL;
   char *srcPtr;
   char *destPtr;
   size_t base64Length;
   size_t resultStringLength;

   /*
    * Check the character set. 
    *   'a' means ASCII.
    */
   if ((NULL == str) || ('a' != *str)) {
      return(NULL);
   }
   str += 1;

   VixMsgInitializeObfuscationMapping();
   base64String = Util_SafeStrdup(str);
   destPtr = base64String;
   srcPtr = base64String;

   /*
    * Remove escaped special characters.
    * Do this in a private copy because we will change the string in place.
    */
   while ( *srcPtr ) {
      if ( '\\' == *srcPtr ) {
         srcPtr++;
         *(destPtr++) = ObfuscatedToPlainCharMap[(int) (*srcPtr)];
      } else {
         *(destPtr++) = *srcPtr;
      }
      srcPtr++;
   }
   *destPtr = 0;
   base64Length = strlen(base64String);

   resultStr = Util_SafeMalloc(base64Length + 1);
   if (!Base64_Decode(base64String, resultStr, base64Length, &resultStringLength)) {
      free(resultStr);
      resultStr = NULL;
      goto abort;
   }
   resultStr[resultStringLength] = 0;

abort:
   free(base64String);

   return(resultStr);
} // VixMsg_DecodeString







