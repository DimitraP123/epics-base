/*************************************************************************\
* Copyright (c) 2016 Michael Davidsaver
* Copyright (c) 2015 Brookhaven Science Assoc. as operator of Brookhaven
*               National Laboratory.
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

/*
 *  Author: Jeffrey O. Hill
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#include "addrList.h"
#include "epicsEvent.h"
#include "epicsMutex.h"
#include "epicsSignal.h"
#include "epicsStdio.h"
#include "epicsTime.h"
#include "errlog.h"
#include "freeList.h"
#include "osiPoolStatus.h"
#include "osiSock.h"
#include "taskwd.h"
#include "cantProceed.h"

#include "epicsExport.h"

#define epicsExportSharedSymbols
#include "dbChannel.h"
#include "dbCommon.h"
#include "dbEvent.h"
#include "db_field_log.h"
#include "dbServer.h"
#include "websrv.h"

#include <drogon/drogon.h>

#define GLBLSOURCE

epicsThreadPrivateId webserverCurrentClient = 0;


int webserver_client_initiating_current_thread ( char * pBuf, size_t bufSize )
{

}

/*
 * webserver_init ()
 */
static
void webserver_init (void)
{
    printf("Hello from our webserver!\n");
    // Start webserver with drogan
}

static
void webserver_run (void)
{
}

static
void webserver_pause (void)
{
}

/*
 *  webserver_report()
 */
void webserver_report (unsigned level)
{

}


void webserver_stats ( unsigned *pChanCount, unsigned *pCircuitCount )
{

}


static dbServer webserver = {
    ELLNODE_INIT,
    "webserver",
    webserver_report,
    webserver_stats,
    webserver_client_initiating_current_thread,
    webserver_init,
    webserver_run,
    webserver_pause
};

void websrv_register_server(void)
{
    dbRegisterServer(&webserver);
}
