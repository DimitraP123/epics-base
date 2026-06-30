/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

/*
 *  Author: Jeffrey O. Hill
 *      hill@luke.lanl.gov
 *      (505) 665 1831
 *  Date:   5-88
 */

#ifndef websrvh
#define websrvh

#include <stddef.h>
#include "shareLib.h"

#define WEBSRV_OK 0
#define WEBSRV_ERROR (-1)

#ifdef __cplusplus
extern "C" {
#endif

epicsShareFunc void websrv_register_server(void);

epicsShareFunc void webserver_report (unsigned level);
epicsShareFunc int webserver_client_initiating_current_thread (
                        char * pBuf, size_t bufSize );
epicsShareFunc void webserver_stats (
                        unsigned *pChanCount, unsigned *pConnCount );

#ifdef __cplusplus
}
#endif

#endif /*websrvh */
