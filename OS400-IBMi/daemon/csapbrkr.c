/* ==========================================================================

  Clarasoft Foundation Server

  CSAP Broker
  Version 7.0.0

  Compile with

  CRTSQLCI OBJ(CSAPBRKRX) SRCFILE(QCSRC_CFS) SRCMBR(CSAPBRKRX)
    DBGVIEW(*SOURCE) COMPILEOPT('SYSIFCOPT(*IFSIO)')

  Build with:

  CRTPGM PGM(CSAPBRKRX) MODULE(CSAPBRKRX)
          BNDSRVPGM(CFSAPI CSLIB)

  Distributed under the MIT license

  Copyright (c) 2013 Clarasoft I.T. Solutions Inc.

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify,
  merge, publish, distribute, sub-license, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
  ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH
  THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

========================================================================== */


#include <except.h>
#include <miptrnam.h>
#include <qmhchgem.h>
#include <qusrgfa2.h>
#include <QLEAWI.h>
#include <QSYGETPH.h>
#include <QSYRUSRI.h>
#include <QUSEC.h>
#include <QUSRJOBI.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include "qcsrc_cfs/cswsck.h"
#include "qcsrc_cfs/cfsrepo.h"
#include "qcsrc_cfs/csjson.h"

#define CFS_DEF_SERVICENAME_MAX  (98)
#define CFS_DEF_LIBNAME_MAX      (11)
#define CFS_DEF_OBJNAME_MAX      (11)
#define CSAP_NULL_SESSION         "000000000000000000000000000000000000"

#define CSAP_SERVICEMODE_SINGLE_LOAD       1
#define CSAP_SERVICEMODE_ENUMERATION_LOAD  2
#define CSAP_SERVICEMODE_DYNAMIC_LOAD      3


/*
void
  QSYGETPH
    (char* user,
     char* pwd,
     void* profileHandle,
     char* errInfo,
     int pwdSize);
*/
///////////////////////////////////////////////////////
// The following is to allow for friend access
// to CSAP instance
///////////////////////////////////////////////////////

typedef struct tagCSAP {

  CSWSCK pSession;

  CSJSON pJsonIn;
  CSJSON pJsonOut;

  CSJSON_LSENTRY lse;

  char szSessionID[37];

  uint64_t OutDataSize;
  uint64_t InDataSize;
  uint64_t UsrCtlSize;
  uint64_t UsrCtlSlabSize;

  long fmt;

  char* pUsrCtlSlab;
  char* pInData;
  char* pOutData;

  CSLIST OutDataParts;

  CFSRPS repo;

} CSAP;

CSAP*
  CSAP_Constructor
    (void);

CSRESULT
  CSAP_Destructor
    (CSAP** This);

CSRESULT
  CSAP_Clear
    (CSAP* This);

typedef CSRESULT
  (*CSAP_SERVICEHANDLERPROC)
    (CSAP* pCSAP);

CSAP_SERVICEHANDLERPROC
  CSAP_GetServiceExport
    (char* pServiceCOnfig);

CSRESULT
  CSAP_Broker
    (CSAP* pSession,
     CFSRPS repo);

CSRESULT
  CSAP_Broker_Single
    (CSAP* pSession,
     char* szService,
     int login,
     CSAP_SERVICEHANDLERPROC CSAP_ServiceHandler);

CSRESULT
  CSAP_Broker_Enum
    (CSAP* pSession,
     CSMAP pServices);

void
  Cleanup
    (_CNL_Hndlr_Parms_T* args);

int conn_fd;
int stream_fd;

char szLibraryName[CFSRPS_PARAM_MAXBUFF];
char szSrvPgmName[CFSRPS_PARAM_MAXBUFF];
char szInProcHandler[CFSRPS_PARAM_MAXBUFF];

char* pHandshakeIn;

long g_CurHandsakeSlab;

volatile unsigned return_code;

int main(int argc, char** argv)
{
  char buffer = 0; // dummy byte character
                   // to send to main daemon

  int rc;
  int type;
  int login;

  long iServiceMode;

  char szConfig[CFSRPS_PATH_MAXBUFF];
  char szService[CFSRPS_PATH_MAXBUFF];

  CSAP_SERVICEHANDLERPROC CSAP_ServiceHandler;

  uint64_t size;

  CSRESULT hResult;

  CFSRPS_CONFIGINFO ci;
  CFSRPS_PARAMINFO pi;

  CFSRPS repo;
  CSMAP  pServices;
  CFSENV pEnv;
  CSAP*  pCSAP;

  return_code = 0;
  #pragma cancel_handler( Cleanup, return_code )

  // Get broker repository configuration
  strncpy(szConfig, argv[2], CFSRPS_PATH_MAXBUFF);
  szConfig[CFSRPS_PATH_MAXBUFF-1] = 0; // make sure we have a null-terminated
                                       // string no longer than the buffer

  fprintf(stdout, "\nINFO: Starting CSAP Broker - Version 7.0: %s\n",
          szConfig);
  fflush(stdout);

  pCSAP     = CSAP_Constructor();
  repo      = CFSRPS_Constructor();
  pServices = CSMAP_Constructor();

  strncpy(ci.szPath, szConfig, CFSRPS_PATH_MAXBUFF);

  g_CurHandsakeSlab = 1024;
  pHandshakeIn = (char*)malloc(1025 * sizeof(char)); // one more for NULL

  if (CS_SUCCEED(CFSRPS_LoadConfig(repo, &ci))) {

    if (CS_SUCCEED(CFSRPS_LookupParam(repo, "SECURE_CONFIG", &pi))) {

      // Get connection environment

      pEnv = CFS_OpenEnv(pi.szValue);

      if (pEnv == NULL) {
        fprintf(stdout, "\nERROR: Secure environment initialization failure "
                        "%s", szConfig);
        fflush(stdout);
        return 0;
      }
    }
    else {
      pEnv = NULL;
    }

    if (CS_SUCCEED(CFSRPS_LookupParam(repo, "USER_LOGIN", &pi))) {
      if (!strcmp(pi.szValue, "*NO")) {
        login = 0;
      }
      else {
        login = 1;
      }
    }
    else {
      login = 1;
    }

    if (CS_SUCCEED(CFSRPS_LookupParam(repo, "SERVICE_MODE", &pi))) {

      if (!strcmp(pi.szValue, "*SINGLE")) {

        fprintf(stdout, "\nINFO: Service mode: *SINGLE\n");
        fflush(stdout);

        iServiceMode = CSAP_SERVICEMODE_SINGLE_LOAD;

        if (CFSRPS_IterStart(repo, "SERVICES") == CS_SUCCESS) {

          // Fetch the first service in sequence from
          // the SERVICES enumeration

          while (CFSRPS_IterNext(repo, &pi) == CS_SUCCESS) {

            CSAP_ServiceHandler =
                CSAP_GetServiceExport(pi.szValue);

            // Get first entry only
            break;
          }

          if (CSAP_ServiceHandler == NULL) {

            fprintf(stdout,
                    "\nERROR: Service handler not loaded\n",
                    pi.szValue);
            fflush(stdout);

            CFSRPS_Destructor(&repo);
            CSMAP_Destructor(&pServices);
            CSAP_Destructor(&pCSAP);

            return 0;
          }

          strcpy(szService, pi.szValue);
        }
        else {
          fprintf(stdout, "\nERROR: Dedicated service not found\n");
          fflush(stdout);
          CFSRPS_Destructor(&repo);
          CSMAP_Destructor(&pServices);
          CSAP_Destructor(&pCSAP);
          return 0;
        }
      }
      else {

        if (!strcmp(pi.szValue, "*ENUMERATION")) {

          // limit the available services to a set (enumeration) of services

          iServiceMode = CSAP_SERVICEMODE_ENUMERATION_LOAD;

          if (CFSRPS_IterStart(repo, "SERVICES") == CS_SUCCESS) {

            // Fetch the services from
            // the SERVICES enumeration

            while (CFSRPS_IterNext(repo, &pi) == CS_SUCCESS) {

              CSAP_ServiceHandler =
                  CSAP_GetServiceExport(pi.szValue);

              if (CSAP_ServiceHandler != NULL) {
                CSMAP_Insert(pServices,
                             pi.szValue,
                             &CSAP_ServiceHandler,
                             sizeof(CSAP_ServiceHandler));
              }
            }
          }
        }
        else {

          if (!strcmp(pi.szValue, "*DYNAMIC")) {
            // Load any valid requested service
            iServiceMode = CSAP_SERVICEMODE_DYNAMIC_LOAD;
          }
          else {
            fprintf(stdout, "\nERROR: Invalid Broker service mode\n");
            fflush(stdout);
            CFSRPS_Destructor(&repo);
            CSMAP_Destructor(&pServices);
            CSAP_Destructor(&pCSAP);
            return 0;
          }
        }
      }
    }
    else {
      fprintf(stdout, "\nERROR: Broker service mode not found\n");
      fflush(stdout);
      CFSRPS_Destructor(&repo);
      CSMAP_Destructor(&pServices);
      CSAP_Destructor(&pCSAP);
      return 0;
    }
  }
  else {
    fprintf(stdout, "\nERROR: Cannot load configuration: %s\n",
            szConfig);
    fflush(stdout);
    CFSRPS_Destructor(&repo);
    CSMAP_Destructor(&pServices);
    CSAP_Destructor(&pCSAP);
    return 0;
  }

  if (argv[1][0] == 'T')
  {
    // This handler is transcient and will end once
    // the connection is ended. Note that we are accessing
    // the CSAP::pSession private member. We can do this
    // because actually, the CSAP Broker is part of the
    // CSAP implementation and as such, CSAPBROKER is
    // a friend of the CSAP class.

    conn_fd = 0;
    if (CS_SUCCEED(hResult = CSWSCK_OpenChannel
                                 (pCSAP->pSession,
                                  pEnv,
                                  szConfig,
                                  conn_fd, &rc))) {

      if (CS_DIAG(hResult) == CSWSCK_DIAG_WEBSOCKET) {

        switch(iServiceMode) {
          case CSAP_SERVICEMODE_SINGLE_LOAD:
            CSAP_Broker_Single(pCSAP, szService, login, CSAP_ServiceHandler);
            break;
          case CSAP_SERVICEMODE_ENUMERATION_LOAD:
            CSAP_Broker_Enum(pCSAP, pServices);
            break;
          case CSAP_SERVICEMODE_DYNAMIC_LOAD:
            CSAP_Broker(pCSAP, repo);
            break;
        }
      }
      else {


      }

      CSWSCK_CloseChannel(pCSAP->pSession, 0, 0, 30);
    }
  }
  else
  {
    // This handler is resident and will not exit until parent dameon
    // ends.

    stream_fd = 0;

    /////////////////////////////////////////////////////////////////////
    // Try to send parent a byte; this indicates we are ready
    // to handle a client...
    /////////////////////////////////////////////////////////////////////

    send(stream_fd, &buffer, 1, 0);

    for (;;)
    {
      /////////////////////////////////////////////////////////////////////
      // The main daemon will eventually hand over the connection socket
      // needed to communicate with a client via the IPC descriptor.
      /////////////////////////////////////////////////////////////////////

      hResult = CFS_ReceiveDescriptor(stream_fd, &conn_fd, -1);

      if (CS_SUCCEED(hResult))
      {
        // Note that we are accessing
        // the CSAP::pSession private member. We can do this
        // because actually, the CSAP Broker is part of the
        // CSAP implementation and as such, CSAPBROKER is
        // a friend of the CSAP class.

        if (CS_SUCCEED(hResult = CSWSCK_OpenChannel(pCSAP->pSession, pEnv,
                                              szConfig, conn_fd, &rc))) {

          if (CS_DIAG(hResult) == CSWSCK_DIAG_WEBSOCKET) {

            switch(iServiceMode) {
              case CSAP_SERVICEMODE_SINGLE_LOAD:
                CSAP_Broker_Single(pCSAP, szService, login, CSAP_ServiceHandler);
                break;
              case CSAP_SERVICEMODE_ENUMERATION_LOAD:
                CSAP_Broker_Enum(pCSAP, pServices);
                break;
              case CSAP_SERVICEMODE_DYNAMIC_LOAD:
                CSAP_Broker(pCSAP, repo);
                break;
            }
          }
          else {


          }

          CSWSCK_CloseChannel(pCSAP->pSession, 0, 0, 30);
        }

        //////////////////////////////////////////////////////////////////
        // Tell main daemon we can handle another connection
        //////////////////////////////////////////////////////////////////

        send(stream_fd, &buffer, 1, 0);
      }
    }
  }

  close(stream_fd);

  CSAP_Destructor(&pCSAP);
  CSMAP_Destructor(&pServices);
  CFSRPS_Destructor(&repo);

  #pragma disable_handler

  return 0;
}

/////////////////////////////////////////////////////////////////////////////
//
//  This function activates a service program and
//  retrieves a pointer from an exported procedure
//
/////////////////////////////////////////////////////////////////////////////

CSAP_SERVICEHANDLERPROC
  CSAP_GetServiceExport
    (char* pServiceConfig) {

  int type;

  char szLibraryName[11];
  char szSrvPgmName[11];
  char szInProcHandler[CFSRPS_PARAM_MAXBUFF];

  CFSRPS repo;
  CFSRPS_CONFIGINFO ci;
  CFSRPS_PARAMINFO pi;

  _SYSPTR pSrvPgm;  // OS400 System pointer

  // Pointer to CSAP service handler function exported by service program.

  CSAP_SERVICEHANDLERPROC CSAP_ServiceHandler;

  repo = CFSRPS_Constructor();

  strncpy(ci.szPath, pServiceConfig, CFSRPS_PATH_MAXBUFF);

  if (CS_SUCCEED(CFSRPS_LoadConfig(repo, &ci))) {

    if (CS_SUCCEED(CFSRPS_LookupParam(repo, "INPROCHANDLER_SRV", &pi))) {

      strncpy(szSrvPgmName, pi.szValue, 11);
      szSrvPgmName[10] = 0;

      if (CS_SUCCEED(CFSRPS_LookupParam(repo, "INPROCHANDLER_LIB", &pi))) {

        strncpy(szLibraryName, pi.szValue, 11);
        szLibraryName[10] = 0;

        if (CS_SUCCEED(CFSRPS_LookupParam(repo,
                       "INPROCHANDLER_EXPORT", &pi))) {

          strncpy(szInProcHandler, pi.szValue, 256);

          fprintf(stdout, "\nINFO: Handler export: %s/%s/%s\n",
                  szLibraryName, szSrvPgmName, szInProcHandler);
          fflush(stdout);

#pragma exception_handler(DLOPEN_ERR, 0, 0, _C2_MH_ESCAPE, _CTLA_HANDLE_NO_MSG)

          pSrvPgm = rslvsp(WLI_SRVPGM,
                           szSrvPgmName,
                           szLibraryName,
                           _AUTH_NONE);

#pragma disable_handler

          QleActBndPgm(&pSrvPgm,
                       NULL,
                       NULL,
                       NULL,
                       NULL);

          type = 0;  // pointer to function

          QleGetExp(NULL,
                    NULL,
                    NULL,
                    szInProcHandler,
                    (void**)&CSAP_ServiceHandler,
                    &type,
                    NULL);

          if (CSAP_ServiceHandler) {
            CFSRPS_Destructor(&repo);
            return CSAP_ServiceHandler;
          }
        }
        else {

          fprintf(stdout, "\nERROR: Cannot read config parameter: "
                          "%s: INPROCHANDLER_EXPORT\n",
                  pServiceConfig);
          fflush(stdout);
        }
      }
      else {

        fprintf(stdout, "\nERROR: Cannot read config parameter: "
                        "%s: INPROCHANDLER_LIB\n",
                pServiceConfig);
        fflush(stdout);
      }
    }
    else {

      fprintf(stdout, "\nERROR: Cannot read config parameter: "
                      "%s: INPROCHANDLER_SRV\n",
              pServiceConfig);
      fflush(stdout);
    }
  }
  else {

    fprintf(stdout, "\nERROR: Handler config not found: %s\n",
            pServiceConfig);
    fflush(stdout);
  }

//////////////////////////////////////////////////////////////////////////////
// Branching label
DLOPEN_ERR:

  fprintf(stdout, "\nERROR: Cannot retrieve export procedure pointer\n");
  fflush(stdout);

  CFSRPS_Destructor(&repo);
  return NULL;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSAP Broker over websocket
//
/////////////////////////////////////////////////////////////////////////////

CSRESULT
  CSAP_Broker
    (CSAP* pCSAP,
     CFSRPS repo) {
#if 0

  int i;
  int type;
  int argSize;
  int iSSLResult;

  volatile unsigned return_code;
  volatile int rsl_ok;
  volatile _INTRPT_Hndlr_Parms_T excbuf;

  short dummySocket;

  char szHandshake[50];
  char szAuthType[11];
  char szStatus[11];
  char szConfig[CFSRPS_PATH_MAXBUFF];

  char szServiceName[CFSRPS_VALUE_MAXBUFF];
  char szLibraryName[11];
  char szSrvPgmName[11];
  char szInProcHandler[CFSRPS_VALUE_MAXBUFF];
  char szSessionID[37];
  char szSrvId[33];
  char szAuthorizationType[11];
  char szConnTimeout[11];

  char  szInstance[33];

  Qus_EC_t ErrorCode;
  Qsy_USRI0300_T pData300;

  uint64_t size;
  uint64_t offset;

  CFSRPS_CONFIGINFO ci;
  CFSRPS_PARAMINFO pi;

  CSRESULT hResult;

  _SYSPTR pSrvPgm;  // OS400 System pointer

  ///////////////////////////////////////////////////////////////////////////
  // Pointer to CSAP service handler function exported by service program.
  ///////////////////////////////////////////////////////////////////////////

  CSAP_SERVICEHANDLERPROC CSAP_ServiceHandler;

  ///////////////////////////////////////////////////////////////////////////
  // Get CSAP request (version 3.0)
  //
  // Format is:
  //
  //      SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS ... S
  //
  // Where S == service name (98 bytes)
  //
  ///////////////////////////////////////////////////////////////////////////

  if (CS_SUCCEED(CSWSCK_Receive(pSession, &size, -1))) {

    // As a rule, the CSAP handshake must arrive in one frame;
    // we should have the service in a single read (98 bytes).

    if (size != 98 ) {
      return CS_FAILURE;
    }

    // get service name
    CSWSCK_GetData(pSession, szServiceName, 0, 98);

    szServiceName[CFSRPS_PATH_MAXBUFF-1] = 0;

    // Log connection

    ////////////////////////////////////////////////
    // Get service program name and exported
    // sub-procedure that implements the
    // required service.
    ////////////////////////////////////////////////

    strncpy(ci.szPath, szServiceName, CFSRPS_PATH_MAXBUFF);

    if (CS_SUCCEED(CFSRPS_LoadConfig(repo, &ci))) {

      if (CS_FAIL(CFSRPS_LookupParam(repo, "INPROCHANDLER_SRV", &pi))) {

        // protocol implementation not found

        memcpy(szHandshake,     "101",              3);
        memcpy(szHandshake + 3, "1000010011",       10);
        memcpy(szHandshake + 13, CSAP_NULL_SESSION, 36);

        CSWSCK_Send(pSession,
                    CSWSCK_OPER_TEXT,
                    szHandshake,
                    49,
                    CSWSCK_FIN_ON,
                    -1);

        return CS_FAILURE;
      }

      strncpy(szSrvPgmName, pi.szValue, 11);
      szSrvPgmName[10] = 0;

      if (CS_FAIL(CFSRPS_LookupParam(repo, "INPROCHANDLER_LIB", &pi))) {

        // protocol implementation not found

        memcpy(szHandshake,     "101",              3);
        memcpy(szHandshake + 3, "1000010012",       10);
        memcpy(szHandshake + 13, CSAP_NULL_SESSION, 36);

        CSWSCK_Send(pSession,
                    CSWSCK_OPER_TEXT,
                    szHandshake,
                    49,
                    CSWSCK_FIN_ON,
                    -1);

        return CS_FAILURE;
      }

      strncpy(szLibraryName, pi.szValue, 11);
      szLibraryName[10] = 0;

      if (CS_FAIL(CFSRPS_LookupParam(repo, "INPROCHANDLER_EXPORT", &pi))) {

        // protocol implementation not found

        memcpy(szHandshake,     "101",              3);
        memcpy(szHandshake + 3, "1000010013",       10);
        memcpy(szHandshake + 13, CSAP_NULL_SESSION, 36);

        CSWSCK_Send(pSession,
                    CSWSCK_OPER_TEXT,
                    szHandshake,
                    49,
                    CSWSCK_FIN_ON,
                    -1);

        return CS_FAILURE;
      }

      strncpy(szInProcHandler, pi.szValue, 256);

#pragma exception_handler(DLOPEN_ERR, 0, 0, _C2_MH_ESCAPE, _CTLA_HANDLE_NO_MSG)

      pSrvPgm = rslvsp(WLI_SRVPGM,
                       szSrvPgmName,
                       szLibraryName,
                       _AUTH_NONE);

#pragma disable_handler

//#pragma exception_handler(DLOPEN_ERR, excbuf, 0, _C2_MH_ESCAPE, _CTLA_HANDLE_NO_MSG)

      QleActBndPgm(&pSrvPgm,
                   NULL,
                   NULL,
                   NULL,
                   NULL);

//#pragma disable_handler

      type = 0;  // pointer to function

//#pragma exception_handler(DLOPEN_ERR, 0, 0, _C2_MH_ESCAPE, _CTLA_HANDLE_NO_MSG)

      QleGetExp(NULL,
                NULL,
                NULL,
                szInProcHandler,
                (void**)&CSAP_ServiceHandler,
                &type,
                NULL);

//#pragma disable_handler

      if (!CSAP_ServiceHandler) {

//////////////////////////////////////////////////////////////////////////////
// Branching label
DLOPEN_ERR:

        // protocol implementation not loaded

        memcpy(szHandshake,     "101",              3);
        memcpy(szHandshake + 3, "1000010014",       10);
        memcpy(szHandshake + 13, CSAP_NULL_SESSION, 36);

        CSWSCK_Send(pSession,
                    CSWSCK_OPER_TEXT,
                    szHandshake,
                    49,
                    CSWSCK_FIN_ON,
                    -1);

        return CS_FAILURE;
      }

      // Create a session ID

      CSSYS_MakeUUID(szSessionID,
                     CSSYS_UUID_LOWERCASE | CSSYS_UUID_DASHES);

      memcpy(szHandshake,     "000",        3);
      memcpy(szHandshake + 3, "4000010202", 10);
      memcpy(szHandshake + 13, szSessionID, 36);

      // Send ACK


      CSWSCK_Send(pSession,
                  CSWSCK_OPER_TEXT,
                  szHandshake,
                  49,
                  CSWSCK_FIN_ON,
                  -1);

      /////////////////////////////////////////////
      // Run the requested service.
      /////////////////////////////////////////////

      CSAP_ServiceHandler(pSession, szSessionID);

    }
    else {

      // Service configuration not found

      memcpy(szHandshake,     "101",              3);
      memcpy(szHandshake + 3, "1000010015",       10);
      memcpy(szHandshake + 13, CSAP_NULL_SESSION, 36);

      CSWSCK_Send(pSession,
                  CSWSCK_OPER_TEXT,
                  szHandshake,
                  49,
                  CSWSCK_FIN_ON,
                  -1);

      return CS_FAILURE;

    }
  }
  else {

    // Error reading client

    memcpy(szHandshake,     "101",              3);
    memcpy(szHandshake + 3, "1000010061",       10);
    memcpy(szHandshake + 13, CSAP_NULL_SESSION, 36);

    CSWSCK_Send(pSession,
                CSWSCK_OPER_TEXT,
                szHandshake,
                49,
                CSWSCK_FIN_ON,
                -1);

    return CS_FAILURE;
  }
#endif

  return CS_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSAP Broker over websocket: dedicated (single) service broker
//
/////////////////////////////////////////////////////////////////////////////

CSRESULT
  CSAP_Broker_Single
    (CSAP* pCSAP,
     char* szService,
     int login,
     CSAP_SERVICEHANDLERPROC CSAP_ServiceHandler) {

  char* pHandshake;
  char szBuffer[1025];
  char szUser[10];
  char szPassword[513];
  char szProfileHandle[12];
  char cpf[8];
  char errInfo[16];

  int bytesProvided;
  int bytesAvail;
  int pwdSize;

  uint64_t size;

  if (CS_SUCCEED(CSWSCK_Receive(pCSAP->pSession, &size, -1))) {

    if (size > g_CurHandsakeSlab) {
      free(pHandshakeIn);
      pHandshakeIn = (char*)malloc((size + 1) * sizeof(char));
      g_CurHandsakeSlab = size;
    }

    CSWSCK_GetData(pCSAP->pSession, pHandshakeIn, 0, size);

    pHandshakeIn[size] = 0;

    CSJSON_Init(pCSAP->pJsonOut, JSON_TYPE_OBJECT);
    CSJSON_MkDir(pCSAP->pJsonOut, "/", "handshake", JSON_TYPE_OBJECT);

    if (CS_SUCCEED(CSJSON_Parse(pCSAP->pJsonIn, pHandshakeIn, 0))) {

      if (login == 1) {

        // authenticate user
        // get user id

        if (CS_FAIL(CSJSON_LookupKey(pCSAP->pJsonIn,
                                      "/", "u",
                                      &(pCSAP->lse)))) {

          fprintf(stdout,
                  "\nERROR: Invalid User Credentail 1000010111\n");
          fflush(stdout);

          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "version", "7.0");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "status", "701");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "reason", "1000010111");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "sid", CSAP_NULL_SESSION);

          size = CSJSON_Serialize(pCSAP->pJsonOut, "/", &pHandshake, 0);

          CSWSCK_Send(pCSAP->pSession,
                      CSWSCK_OPER_TEXT,
                      pHandshake,
                      size,
                      CSWSCK_FIN_ON,
                      -1);

          return CS_FAILURE;
        }

        memset(szUser, ' ', 10);
        memcpy(szUser, pCSAP->lse.szValue, strlen(pCSAP->lse.szValue));

        // get password
        if (CS_FAIL(CSJSON_LookupKey(pCSAP->pJsonIn,
                                      "/", "p",
                                      &(pCSAP->lse)))) {

          sprintf(szBuffer,
             "\nERROR: Invalid User Credentail 1000010222: User: %s\n",
             pCSAP->lse.szValue);

          fprintf(stdout,
                  szBuffer);
          fflush(stdout);

          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "version", "7.0");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "status", "701");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "reason", "1000010222");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "sid", CSAP_NULL_SESSION);

          size = CSJSON_Serialize(pCSAP->pJsonOut, "/", &pHandshake, 0);

          CSWSCK_Send(pCSAP->pSession,
                      CSWSCK_OPER_TEXT,
                      pHandshake,
                      size,
                      CSWSCK_FIN_ON,
                      -1);

          return CS_FAILURE;
        }

        pwdSize = strlen(pCSAP->lse.szValue);

        if (pwdSize > 512) {
          pCSAP->lse.szValue[512] = 0;
          pwdSize = 512;
        }

        memset(errInfo, ' ', 16);
        memset(szProfileHandle, ' ', 12);

        bytesProvided = 16;
        memcpy(errInfo, &bytesProvided, sizeof(int));

        QSYGETPH(szUser,
                 pCSAP->lse.szValue,
                 (  void*)szProfileHandle,
                 errInfo,
                 pwdSize,
                 0);

        memcpy(cpf, errInfo + 8, 7);
        cpf[7] = 0;

        if (strcmp("       ", cpf)) {

          sprintf(szBuffer,
             "\nERROR: User authentication failure 1000010333: Exception: %s\n",
             cpf);

          fprintf(stdout, szBuffer);
          fflush(stdout);

          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "version", "7.0");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "status", "701");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "reason", "1000010333");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "sid", CSAP_NULL_SESSION);

          size = CSJSON_Serialize(pCSAP->pJsonOut, "/", &pHandshake, 0);

          CSWSCK_Send(pCSAP->pSession,
                      CSWSCK_OPER_TEXT,
                      pHandshake,
                      size,
                      CSWSCK_FIN_ON,
                      -1);

          return CS_FAILURE;
        }
      }

      // get service name
      if (CS_SUCCEED(CSJSON_LookupKey(pCSAP->pJsonIn,
                                      "/", "service",
                                      &(pCSAP->lse)))) {

        if (strcmp(pCSAP->lse.szValue, szService) == 0) {

          // Create a session ID

          CSSYS_MakeUUID(pCSAP->szSessionID,
                     CSSYS_UUID_LOWERCASE | CSSYS_UUID_DASHES);

          CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "version", "7.0");
          CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "status", "000");
          CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "reason", "0000000000");
          CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "sid", pCSAP->szSessionID);

          size = CSJSON_Serialize(pCSAP->pJsonOut, "/", &pHandshake, 0);

          CSWSCK_Send(pCSAP->pSession,
                      CSWSCK_OPER_TEXT,
                      pHandshake,
                      size,
                      CSWSCK_FIN_ON,
                      -1);

          CSAP_Clear(pCSAP);

          /////////////////////////////////////////////
          // Run the requested service.
          /////////////////////////////////////////////

          CSAP_ServiceHandler(pCSAP);

        }
        else {

          fprintf(stdout,
                  "\nERROR: Requested Service %s not supported\n",
                  pCSAP->lse.szValue);
          fflush(stdout);

          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "version", "7.0");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "status", "101");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "reason", "1000010011");
          CSJSON_InsertString(pCSAP->pJsonOut,
                            "/handshake", "sid", CSAP_NULL_SESSION);

          size = CSJSON_Serialize(pCSAP->pJsonOut, "/", &pHandshake, 0);

          CSWSCK_Send(pCSAP->pSession,
                      CSWSCK_OPER_TEXT,
                      pHandshake,
                      size,
                      CSWSCK_FIN_ON,
                      -1);

          return CS_FAILURE;
        }
      }
      else {

        fprintf(stdout,
                "\nERROR: No specified service in request\n");
        fflush(stdout);

        CSJSON_InsertString(pCSAP->pJsonOut,
                          "/handshake", "version", "7.0");
        CSJSON_InsertString(pCSAP->pJsonOut,
                          "/handshake", "status", "101");
        CSJSON_InsertString(pCSAP->pJsonOut,
                          "/handshake", "reason", "1000010015");
        CSJSON_InsertString(pCSAP->pJsonOut,
                          "/handshake", "sid", CSAP_NULL_SESSION);

        size = CSJSON_Serialize(pCSAP->pJsonOut, "/", &pHandshake, 0);

        CSWSCK_Send(pCSAP->pSession,
                    CSWSCK_OPER_TEXT,
                    pHandshake,
                    size,
                    CSWSCK_FIN_ON,
                    -1);

        return CS_FAILURE;
      }
    }
    else {

      // Handshake parse error

      fprintf(stdout,
            "\nERROR: Handshake segment format error\n");
      fflush(stdout);

      CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "version", "7.0");
      CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "status", "101");
      CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "reason", "1000010062");
      CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "sid", CSAP_NULL_SESSION);

      size = CSJSON_Serialize(pCSAP->pJsonOut, "/", &pHandshake, 0);

      CSWSCK_Send(pCSAP->pSession,
                  CSWSCK_OPER_TEXT,
                  pHandshake,
                  size,
                  CSWSCK_FIN_ON,
                  -1);

      return CS_FAILURE;

    }
  }
  else {

    // Error reading client

    fprintf(stdout,
            "\nERROR: Reading client service request failed\n");
    fflush(stdout);

    CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "version", "7.0");
    CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "status", "101");
    CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "reason", "1000010061");
    CSJSON_InsertString(pCSAP->pJsonOut,
                        "/handshake", "sid", CSAP_NULL_SESSION);

    size = CSJSON_Serialize(pCSAP->pJsonOut, "/", &pHandshake, 0);

    CSWSCK_Send(pCSAP->pSession,
                CSWSCK_OPER_TEXT,
                pHandshake,
                size,
                CSWSCK_FIN_ON,
                -1);

    return CS_FAILURE;
  }

  return CS_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSAP Broker over websocket
//
/////////////////////////////////////////////////////////////////////////////
CSRESULT
  CSAP_Broker_Enum
    (CSAP* pCSAP,
     CSMAP pServices) {
#if 0

  char szHandshake[50];

  char szServiceName[CFSRPS_PARAM_MAXBUFF];
  char szSessionID[37];

  long dataSize;

  uint64_t size;

  CSAP_SERVICEHANDLERPROC CSAP_ServiceHandler;

  ///////////////////////////////////////////////////////////////////////////
  // Get CSAP request (version 3.0)
  //
  // Format is:
  //
  //      SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS ... S
  //
  // Where S == service name (98 bytes)
  //
  ///////////////////////////////////////////////////////////////////////////

  if (CS_SUCCEED(CSWSCK_Receive(pSession, &size, -1))) {

    // As a rule, the CSAP handshake must arrive in one frame;
    // we should have the service in a single read (98 bytes).

    if (size != 98 ) {
      return CS_FAILURE;
    }

    // get service name
    CSWSCK_GetData(pSession, szServiceName, 0, 98);

    szServiceName[CFSRPS_PATH_MAXBUFF-1] = 0;

    // Check if requested service is the one that is active

    if (CS_FAIL(CSMAP_Lookup(pServices,
                             szServiceName,
                             &CSAP_ServiceHandler,
                             &dataSize))) {

      memcpy(szHandshake,     "101",              3);
      memcpy(szHandshake + 3, "1000010011",       10);
      memcpy(szHandshake + 13, CSAP_NULL_SESSION, 36);

      CSWSCK_Send(pSession,
                  CSWSCK_OPER_TEXT,
                  szHandshake,
                  49,
                  CSWSCK_FIN_ON,
                  -1);

      return CS_FAILURE;
    }

    // Create a session ID

    CSSYS_MakeUUID(szSessionID,
                     CSSYS_UUID_LOWERCASE | CSSYS_UUID_DASHES);

    memcpy(szHandshake,     "000",        3);
    memcpy(szHandshake + 3, "4000010202", 10);
    memcpy(szHandshake + 13, szSessionID, 36);

    // Send ACK

    CSWSCK_Send(pSession,
                CSWSCK_OPER_TEXT,
                szHandshake,
                49,
                CSWSCK_FIN_ON,
                -1);

    /////////////////////////////////////////////
    // Run the requested service.
    /////////////////////////////////////////////

    CSAP_ServiceHandler(pSession, szSessionID);

  }
  else {

    // Error reading client

    memcpy(szHandshake,     "101",              3);
    memcpy(szHandshake + 3, "1000010061",       10);
    memcpy(szHandshake + 13, CSAP_NULL_SESSION, 36);

    CSWSCK_Send(pSession,
                CSWSCK_OPER_TEXT,
                szHandshake,
                49,
                CSWSCK_FIN_ON,
                -1);

    return CS_FAILURE;

  }
#endif

  return CS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Cleanup
 * ----------------------------------------------------------------------- */

void Cleanup(_CNL_Hndlr_Parms_T* data) {

  //CSWSCK_CloseChannel(pConn, 0, 0, 30);
  close(stream_fd);
}

