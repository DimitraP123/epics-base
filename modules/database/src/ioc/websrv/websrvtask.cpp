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

// websocket includes
#include <atomic>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

#include "dbChannel.h"





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
#include <drogon/WebSocketController.h>


#define GLBLSOURCE
#include "websrv.h"

extern DBBASE *pdbbase;

epicsThreadPrivateId webserverCurrentClient = 0;

static std::string webserver_report_body(unsigned level);
static void webserver_thread(void *);

static Json::Value websrv_record_handling(const std::string& recordName);
static Json::Value websrv_field_get(const std::string& recordName, const std::string& fieldName);
static Json::Value websrv_field_put(const std::string& recordName, const std::string& fieldName, const std::string& fieldData);
static void webserver_append_database_routes(std::ostringstream& body, bool includeFields);

static const char *websrvAddress = "127.0.0.1";
static const unsigned websrvPort = 8080;

/*
/ WebSocket Logic Starts  
*/

static dbEventCtx websrvEvents = nullptr;

using namespace drogon;

namespace {

    // A WebSocket subscription structure that holds the state of a WebSocket connection and its EPICS db channel and event subscription
    struct WebSocketSubscription
    {
        std::string recordName;
        std::string fieldName;
        std::string fullName;

        dbChannel *channel{nullptr};

        dbEventSubscription subscription{nullptr};

        WebSocketConnectionPtr connection;

        std::atomic<bool> closing{false};
    };

    // Removes extra whitespace from the WebSocket message 
    static std::string websrv_sock_json(const Json::Value &value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";

        return Json::writeString(builder, value);
    }

    // Parses the WebSocket path to extract the record and field names
    static bool websrv_sock_parse_path(const std::string &path, std::string &recordName, std::string &fieldName)
    {
        if (path.empty() || path[0] != '/') {
            return false;
        }

        const std::string target = path.substr(1);

        const std::string::size_type fieldSeparator = target.find('/');

        if (fieldSeparator == std::string::npos) {
            recordName = target;
            fieldName = "VAL";

            return !recordName.empty();
        }

        recordName = target.substr(0, fieldSeparator);

        fieldName = target.substr(fieldSeparator + 1);

        return !recordName.empty() && !fieldName.empty();
    }

    static void sock_field_updated_event(void *userArgument, dbChannel *channel, int eventsRemaining, db_field_log *fieldLog)
    {
        //(void)eventsRemaining;

        // Callback receives the state
        auto *state = static_cast<WebSocketSubscription *>(userArgument);

        // Checking if the connection is still usable
        if (!state || state->closing.load() || !state->connection || !state->connection->connected()) {
            return;
        }

        char fieldData[MAX_STRING_SIZE] = {};

        long elementCount = 1;

        // The callback reads the field
        const long status = dbChannelGetField(channel, DBR_STRING, fieldData, nullptr, &elementCount, fieldLog);

        // If status is zero, the callback successfully retrieved the field
        if (status) {
            return;
        }

        Json::Value response;

        response["record"] = state->recordName;
        response["field"] = state->fieldName;
        response["field_data"] = fieldData;

        const std::string updated_message_data = websrv_sock_json(response);

        const WebSocketConnectionPtr connection = state->connection;
        
        //The WebSocket connection is sent onto Drogon's event loop
        drogon::app().getLoop()->queueInLoop([connection, updated_message_data]()
        {
            if (connection && connection->connected()) {
                connection->send(updated_message_data);
            }
        }
        );
    }
} 

// This class gives the behavior needed to handle WebSocket connections in a Drogon-based server
class WebSocketStatusController : public drogon::WebSocketController< WebSocketStatusController, false>
{
public:
    void handleNewConnection(const HttpRequestPtr &request, const WebSocketConnectionPtr &connection) override
    {
        std::string recordName;
        std::string fieldName;

        
        // Extracts the target record and field from the WebSocket URL
         
        if (!websrv_sock_parse_path( request->path(), recordName, fieldName)) {

            connection->send(R"({"type":"error","message":"Invalid WebSocket path"})");
            connection->shutdown();
            return;
        }

        // Creates a new shared WebSocket subscription state object for the current connection
        auto state = std::make_shared<WebSocketSubscription>();

        // Copies the parsed path information into the subscription state object
        state->recordName = recordName;
        state->fieldName = fieldName;
        state->fullName = recordName + "." + fieldName;

        state->connection = connection;

        // Opens the EPICS db channel once for this connection
        state->channel = dbChannelCreate(state->fullName.c_str());

        if (!state->channel) {
            connection->send(R"("message":"Record or field not found")");
            connection->shutdown();
            return;
        }

        if (dbChannelOpen(state->channel)) {
            
            dbChannelDelete(state->channel);
            
            state->channel = nullptr;

            connection->send(R"("message":"Record or field not found")");

            connection->shutdown();
            return;
        }

        /*
         * Create the EPICS event subscription.
         *
         * DBE_VALUE sends value-change events.
         * DBE_ALARM also sends alarm-state changes.
         */
        state->subscription = db_add_event(websrvEvents, state->channel, sock_field_updated_event, state.get(), DBE_VALUE | DBE_ALARM);

        if (!state->subscription) {
            dbChannelDelete(state->channel);
            
            state->channel = nullptr;

            connection->send(R"({"type":"error","message":"Unable to create event subscription"})");

            connection->shutdown();
            return;
        }

        // Storing the state in the Drogon connection
        connection->setContext(state);

        db_event_enable(state->subscription);

        //Send the current value immediately
         
        db_post_single_event(state->subscription);

        errlogPrintf("websrv: monitoring %s\n", state->fullName.c_str());
    }

    void handleNewMessage(const WebSocketConnectionPtr &connection, std::string &&message, const WebSocketMessageType &messageType) override
    {
        (void)connection;
        (void)message;
        (void)messageType;
    }

    void handleConnectionClosed(const WebSocketConnectionPtr &connection) override
    {
        auto state = connection->getContext<WebSocketSubscription>();

        if (!state) {
            return;
        }

        state->closing.store(true);

        // Cancels the EPICS event before deleting the channel.
        if (state->subscription) {
            db_cancel_event(state->subscription);
            state->subscription = nullptr;
        }

        if (state->channel) {
            dbChannelDelete(state->channel);
            state->channel = nullptr;
        }

        state->connection.reset();

        errlogPrintf("websrv: WebSocket monitor disconnected\n");
    }

    WS_PATH_LIST_BEGIN

    //One generic WebSocket route handles every EPICS record and field
    WS_ADD_PATH_VIA_REGEX(R"(^/[^/]+/[^/]+$)", Get);

    WS_PATH_LIST_END
};


// Keeps the registered controller alive for the lifetime of the IOC
static std::shared_ptr<WebSocketStatusController> webserverWebSocketController;

/*
/ WebSocket Logic Ends 
*/

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

        // Report handling route when no level is specified (defaults to level 0)
        .registerHandler("/report", [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
                {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                    resp->setStatusCode(drogon::k200OK);
                    resp->setBody(webserver_report_body(0));
                    callback(resp);
                },
                {drogon::Get}
            )
 
        // Report handling route when a level is specified
        .registerHandler("/report/{1}", [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& levelParam)
            {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);

                try {
                    unsigned level = static_cast<unsigned>(std::stoul(levelParam));

                    resp->setStatusCode(drogon::k200OK);
                    resp->setBody(webserver_report_body(level));
                }
                catch (const std::exception&) {
                    resp->setStatusCode(drogon::k400BadRequest);
                    resp->setBody("Invalid report level!\n");
                }

                callback(resp);
            },
            {drogon::Get}
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
        // WebSocket initialization starts 
        */
        websrvEvents = db_init_events();

        if (!websrvEvents) {
            errlogPrintf(
                "websrv: unable to initialize database events\n");
            return;
        }

        const int eventStatus = db_start_events(websrvEvents, "websrvEvents", nullptr, nullptr, epicsThreadPriorityMedium);

        if (eventStatus != DB_EVENT_OK) {
            errlogPrintf("websrv: unable to start database event thread\n");

            db_close_events(websrvEvents);
            websrvEvents = nullptr;
            return;
        }
        
        webserverWebSocketController = std::make_shared<WebSocketStatusController>();

        drogon::app().registerController(webserverWebSocketController);
    /*
    // WebSocket initialization ends 
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
   
    else if (level == 1) {
        body << "Listening address: " << websrvAddress << "\n";
        body << "Listening port: " << websrvPort << "\n";
    }

    else if (level == 2) {
        body << "Routes:\n";
        body << "/init\n";
        body << "/stats\n";
        body << "/report\n";
        body << "/{record}\n";
        body << "/{record}/{field}\n";
    }

    else if (level == 3) {
        body << "-----PV RECORD LIST-----\n";
        webserver_append_database_routes(body, false);
    }

    else if (level >= 4) {
        body << "-----PV FIELDS LIST-----\n";
        webserver_append_database_routes(body, true);
    }

    return body.str();
}

void webserver_report (unsigned level)
{   
    printf("%s", webserver_report_body(level).c_str());
}

static void webserver_append_database_routes(std::ostringstream& body, bool includeFields)
{
    DBENTRY *pdbentry = dbAllocEntry(pdbbase);

    if (!pdbentry) {
        body << "Unable to allocate DBENTRY\n";
        return;
    }

    long recordTypeStatus = dbFirstRecordType(pdbentry);

    while (!recordTypeStatus) {
        long recordStatus = dbFirstRecord(pdbentry);

        while (!recordStatus) {
            const char *recordName = dbGetRecordName(pdbentry);

            if (includeFields) {
                long fieldStatus = dbFirstField(pdbentry, TRUE);

                while (!fieldStatus) {
                    const char *fieldName = dbGetFieldName(pdbentry);

                    if (fieldName) {
                        body << recordName << "/" << fieldName << "\n";
                    }

                    fieldStatus = dbNextField(pdbentry, TRUE);
                }
            }
            
            else if (recordName) {
                body << recordName << "\n";
            
            }
            recordStatus = dbNextRecord(pdbentry);
        }

        recordTypeStatus = dbNextRecordType(pdbentry);
    }

    dbFreeEntry(pdbentry);
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