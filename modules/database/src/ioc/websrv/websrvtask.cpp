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
#include "epicsThread.h"
#include "epicsExport.h"

#define epicsExportSharedSymbols
#include "dbChannel.h"
#include "dbCommon.h"
#include "dbEvent.h"
#include "db_field_log.h"
#include "dbServer.h"

#include <dbAccess.h>
#include <dbDefs.h>
#include <dbStaticLib.h>

#include <drogon/drogon.h>

#define GLBLSOURCE
#include "websrv.h"

extern DBBASE *pdbbase;

epicsThreadPrivateId webserverCurrentClient = 0;

static std::string webserver_report_body(unsigned level);
static void webserver_thread(void *);

static Json::Value websrv_record_handling(const std::string& recordName);
static Json::Value websrv_field_get(const std::string& recordName, const std::string& fieldName);
static Json::Value websrv_field_put(const std::string& recordName, const std::string& fieldName, const std::string& fieldData);

static const char *websrvAddress = "127.0.0.1";
static const unsigned websrvPort = 8080;

int webserver_client_initiating_current_thread ( char * pBuf, size_t bufSize )
{

}

static void webserver_init(void)
{
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

        // Record handling route
        .registerHandler("/{1}", [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& recordName)
            {
                Json::Value result = websrv_record_handling(recordName);

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                resp->setStatusCode(drogon::k200OK);
                resp->setBody(result["field"].asString() + ": " + result["field_data"].asString() + "\n");
                callback(resp);
            },
            {drogon::Get}
        )

        // Field handling route (gets and puts field data)
        .registerHandler("/{1}/{2}", [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& recordName, const std::string& fieldName)
            {
                Json::Value result;

                // If a field name is not specified, returns a 404 Not Found
                if (fieldName.empty()) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k404NotFound);
                    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                    resp->setBody("404 Not Found: field name is missing\n");
                    callback(resp);
                    return;
                }
                // If the request method is GET
                if (req->method() == drogon::Get) {
                    result = websrv_field_get( recordName, fieldName);
                }

                // If the request method is PUT
                else if (req->method() == drogon::Put) {const std::string fieldData = req->getParameter("field_data");

                    if (fieldData.empty()) 
                    {
                        result["record"] = recordName;
                        result["field"] = fieldName;
                        result["error"] = "No field_data specified!";
                    }
                    else {
                        result = websrv_field_put(recordName, fieldName, fieldData);
                    }
                }
                else {
                    result["error"] = "Error! - Unsupported HTTP method - Please use GET or PUT\n";
                }

                auto resp = drogon::HttpResponse::newHttpResponse();

                if (result.isMember("error")) 
                {
                    resp->setStatusCode(drogon::k400BadRequest);
                }
                else 
                {
                    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                    resp->setStatusCode(drogon::k200OK);
                    resp->setBody(result["field"].asString() + ": " + result["field_data"].asString() + "\n");
                }

                callback(resp);
            },
            {drogon::Get, drogon::Put}
        );

        /*
        // Background Thread
        // Runs the Drogon web server in a separate thread to avoid blocking the main EPICS thread
        */

        printf("Creating webserver background run thread!\n");

        epicsThreadId threadId = epicsThreadCreate("webserver", epicsThreadPriorityMedium, epicsThreadGetStackSize(epicsThreadStackMedium), webserver_thread, nullptr);

        // If thread could not be created, print error message
        if (!threadId) {
            errlogPrintf("Drogon web server thread has failed to be created\n");
            return;
        }

    printf("Background thread running! Webserver successfully initialized!\n");
}

static void webserver_thread(void *)
{
    drogon::app().run();
}

static Json::Value websrv_record_handling(const std::string& recordName)
{
    return websrv_field_get(recordName, "VAL");
}

static Json::Value websrv_field_get(const std::string& recordName, const std::string& fieldName)
{
    Json::Value result;

    result["record"] = recordName;
    result["field"] = fieldName;

    const std::string fullName = recordName + "." + fieldName;

    DBADDR addr;

    long status = dbNameToAddr(fullName.c_str(), &addr);

    if (status) {
        result["error"] = "Record or field not found";
        return result;
    }

    char fieldData[MAX_STRING_SIZE] = {};

    status = dbGetField( &addr, DBR_STRING, fieldData, nullptr, nullptr, nullptr);

    if (status) {
        result["error"] = "Unable to read field";
        return result;
    }

    result["field_data"] = fieldData;

    return result;
}

static Json::Value websrv_field_put(const std::string& recordName, const std::string& fieldName, const std::string& fieldData)
{
    Json::Value result;

    result["record"] = recordName;
    result["field"] = fieldName;

    const std::string fullName = recordName + "." + fieldName;

    DBADDR addr;

    long status = dbNameToAddr(fullName.c_str(), &addr);

    if (status) {
        result["error"] = "Record or field not found";
        return result;
    }

    status = dbPutField(&addr, DBR_STRING, fieldData.c_str(), 1);

    if (status) {
        result["error"] = "Unable to write field";
        return result;
    }

    result["field_data"] = fieldData;

    return result;
}

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
        body << "/init\n";
        body << "/stats\n";
        body << "/report\n";
        body << "/{record}\n";
        body << "/{record}/{field}\n";
    }

    return body.str();
}

void webserver_report (unsigned level)
{   
    printf("%s", webserver_report_body(level).c_str());
}

void webserver_stats ( unsigned *pChanCount, unsigned *pCircuitCount )
{
    unsigned int circuitCount = static_cast<unsigned>(drogon::app().getConnectionCount());
    
    if (circuitCount < 0) {
        *pCircuitCount = 0;
    }
    else {
        *pCircuitCount = (unsigned) circuitCount;
    }

    printf("Active HTTP connections: %u\n", circuitCount);
}

static
void webserver_run (void)
{
    websrvTCP_ctl = ctlRun;
}

static
void webserver_pause (void)
{
    websrvTCP_ctl = ctlPause;
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