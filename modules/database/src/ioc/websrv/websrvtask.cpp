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


#include <drogon/drogon.h>

#define GLBLSOURCE
#include "websrv.h"




epicsThreadPrivateId webserverCurrentClient = 0;

static std::string webserver_report_body(unsigned level);

int webserver_client_initiating_current_thread ( char * pBuf, size_t bufSize )
{

}

/*
 * webserver_init ()
 */

static void webserver_init(void)
{
    printf("Pre webserver initialization!\n");

    drogon::app()
        .addListener("127.0.0.1", 8080)
        .registerHandler("/init",[](const drogon::HttpRequestPtr& req,std::function<void(const drogon::HttpResponsePtr&)>&& callback)
            {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK); //status code telling client request was successfully processed
                resp->setBody("Web server is now running\n");
                callback(resp);
            },
            {drogon::Get}
        )


        .registerHandler("/report", [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
            {

            unsigned level = 1;
            auto levelParam = req->getParameter("level");
            if (!levelParam.empty()) {
                level = std::stoul(levelParam);
            }

            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody(webserver_report_body(level));
            callback(resp);
            }
        )



        .registerHandler("/stats", [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
            {
                unsigned chanCount = 0;
                unsigned circuitCount = 0;

                webserver_stats(&chanCount, &circuitCount);

                Json::Value json;
                json["channelCount"] = chanCount;
                json["circuitCount"] = circuitCount;

                auto resp = drogon::HttpResponse::newHttpJsonResponse(json);
                callback(resp);
            },
            {drogon::Get}
        );

    drogon::app().run(); //runs the actual server 
}

static
void webserver_run (void)
{
    websrvTCP_ctl = ctlRun;
 
}

static
void webserver_pause (void)
{
    //beacon_ctl = ctlPause;
    websrvTCP_ctl = ctlPause;
}




static const char *websrvAddress = "127.0.0.1";
static const unsigned websrvPort = 8080;

static std::string webserver_report_body(unsigned level)
{
    std::ostringstream body;

    
    if (level == 0) {
        body << "HTTP Web Server\n";
        body << "Framework: Drogon\n";
    }
   

    
    if (level == 1) {
        body << "Listening address: " << websrvAddress << "\n";
        body << "Listening port: " << websrvPort << "\n";

    }

    if (level >= 2) {
        body << "Routes:\n";
        body << "  GET /report\n";
    }

    return body.str();
}


/*
 *  webserver_report()
 */
void webserver_report (unsigned level)
{   

    printf("%s", webserver_report_body(level).c_str());



}










void webserver_stats ( unsigned *pChanCount, unsigned *pCircuitCount )
{
    // circuit count --> active http connection count
    unsigned int circuitCount = static_cast<unsigned>(drogon::app().getConnectionCount());
    if (circuitCount < 0) {
        *pCircuitCount = 0;
    }
    else {
        *pCircuitCount = (unsigned) circuitCount;
    }

    printf("Active HTTP connections: %u\n", circuitCount);



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
