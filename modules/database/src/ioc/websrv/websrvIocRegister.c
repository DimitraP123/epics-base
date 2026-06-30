/*************************************************************************\
* Copyright (c) 2007 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include "osiSock.h"
#include "iocsh.h"

#define epicsExportSharedSymbols
#include "websrv.h"
#include "epicsExport.h"


/* websrv */
static const iocshArg websrvArg0 = { "level",iocshArgInt};
static const iocshArg * const websrvArgs[1] = {&websrvArg0};
static const iocshFuncDef websrvFuncDef = {"webserver_report",1,websrvArgs};
static void websrvCallFunc(const iocshArgBuf *args)
{
    webserver_report(args[0].ival);
}

static
void websrvRegistrar(void)
{
    websrv_register_server();
    iocshRegister(&websrvFuncDef,websrvCallFunc);
}

// epicsExportAddress(int, WEBSERVER_DEBUG);
epicsExportRegistrar(websrvRegistrar);
