/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "LOOLWSD.hpp"
#include "config.h"

/* Default host used in the start test URI */
#define LOOLWSD_TEST_HOST "localhost"

/* Default loleaflet UI used in the admin console URI */
#define LOOLWSD_TEST_ADMIN_CONSOLE "/loleaflet/dist/admin/admin.html"

/* Default loleaflet UI used in the start test URI */
#define LOOLWSD_TEST_LOLEAFLET_UI "/loleaflet/" LOOLWSD_VERSION_HASH "/loleaflet.html"

/* Default document used in the start test URI */
#define LOOLWSD_TEST_DOCUMENT_RELATIVE_PATH "test/data/hello-world.odt"

// This is the main source for the loolwsd program. LOOL uses several loolwsd processes: one main
// parent process that listens on the TCP port and accepts connections from LOOL clients, and a
// number of child processes, each which handles a viewing (editing) session for one document.

#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cassert>
#include <cerrno>
#include <clocale>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <Poco/DOM/AutoPtr.h>
#include <Poco/DOM/DOMParser.h>
#include <Poco/DOM/DOMWriter.h>
#include <Poco/DOM/Document.h>
#include <Poco/DOM/Element.h>
#include <Poco/DOM/NodeList.h>
#include <Poco/Environment.h>
#include <Poco/Exception.h>
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/ConsoleCertificateHandler.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/IPAddress.h>
#include <Poco/Net/InvalidCertificateHandler.h>
#include <Poco/Net/KeyConsoleHandler.h>
#include <Poco/Net/MessageHeader.h>
#include <Poco/Net/NameValueCollection.h>
#include <Poco/Net/Net.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/PartHandler.h>
#include <Poco/Net/PrivateKeyPassphraseHandler.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/SecureServerSocket.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Path.h>
#include <Poco/Pipe.h>
#include <Poco/Process.h>
#include <Poco/SAX/InputSource.h>
#include <Poco/StreamCopier.h>
#include <Poco/StringTokenizer.h>
#include <Poco/TemporaryFile.h>
#include <Poco/ThreadPool.h>
#include <Poco/URI.h>
#include <Poco/Util/HelpFormatter.h>
#include <Poco/Util/MapConfiguration.h>
#include <Poco/Util/Option.h>
#include <Poco/Util/OptionException.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Util/ServerApplication.h>

#include "Admin.hpp"
#include "Auth.hpp"
#include "ClientSession.hpp"
#include "Common.hpp"
#include "DocumentBroker.hpp"
#include "Exceptions.hpp"
#include "FileServer.hpp"
#include "IoUtil.hpp"
#include "Log.hpp"
#include "Protocol.hpp"
#include "ServerSocket.hpp"
#include "Session.hpp"
#include "Storage.hpp"
#include "TraceFile.hpp"
#include "Unit.hpp"
#include "UnitHTTP.hpp"
#include "UserMessages.hpp"
#include "Util.hpp"
#include "FileUtil.hpp"
#include "LOOLWebSocket.hpp"

#ifdef KIT_IN_PROCESS
#include <Kit.hpp>
#endif

#ifdef FUZZER
#include <tools/Replay.hpp>
#endif

#include "common/SigUtil.hpp"

using namespace LOOLProtocol;

using Poco::Environment;
using Poco::Exception;
using Poco::File;
using Poco::Net::HTMLForm;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPRequestHandlerFactory;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServer;
using Poco::Net::HTTPServerParams;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServerResponse;
using Poco::Net::MessageHeader;
using Poco::Net::NameValueCollection;
using Poco::Net::PartHandler;
using Poco::Net::SecureServerSocket;
using Poco::Net::SocketAddress;
using Poco::Net::WebSocket;
using Poco::Path;
#ifndef KIT_IN_PROCESS
using Poco::Pipe;
#endif
using Poco::Process;
#ifndef KIT_IN_PROCESS
using Poco::ProcessHandle;
#endif
using Poco::StreamCopier;
using Poco::StringTokenizer;
using Poco::TemporaryFile;
#if FUZZER
using Poco::Thread;
#endif
using Poco::ThreadPool;
using Poco::URI;
using Poco::Util::Application;
using Poco::Util::HelpFormatter;
using Poco::Util::IncompatibleOptionsException;
using Poco::Util::MissingOptionException;
using Poco::Util::Option;
using Poco::Util::OptionSet;
using Poco::Util::ServerApplication;
using Poco::XML::AutoPtr;
using Poco::XML::DOMParser;
using Poco::XML::DOMWriter;
using Poco::XML::Element;
using Poco::XML::InputSource;
using Poco::XML::NodeList;
using Poco::XML::Node;

int ClientPortNumber = DEFAULT_CLIENT_PORT_NUMBER;
int MasterPortNumber = DEFAULT_MASTER_PORT_NUMBER;

/// New LOK child processes ready to host documents.
//TODO: Move to a more sensible namespace.
static bool DisplayVersion = false;
static std::vector<std::shared_ptr<ChildProcess> > NewChildren;
static std::mutex NewChildrenMutex;
static std::condition_variable NewChildrenCV;
static std::chrono::steady_clock::time_point LastForkRequestTime = std::chrono::steady_clock::now();
static std::atomic<int> OutstandingForks(0);
static std::map<std::string, std::shared_ptr<DocumentBroker> > DocBrokers;
static std::mutex DocBrokersMutex;

/// Used when shutting down to notify them all that the server is recycling.
static std::vector<std::shared_ptr<LOOLWebSocket> > ClientWebSockets;
static std::mutex ClientWebSocketsMutex;

#if ENABLE_DEBUG
static int careerSpanSeconds = 0;
#endif

namespace
{

inline void shutdownLimitReached(LOOLWebSocket& ws)
{
    const std::string error = Poco::format(PAYLOAD_UNAVAILABLE_LIMIT_REACHED, MAX_DOCUMENTS, MAX_CONNECTIONS);
    LOG_INF("Sending client limit-reached message: " << error);

    /* loleaflet sends loolclient, load and partrectangles message immediately
       after web socket handshake, so closing web socket fails loading page in
       some sensible browsers. Ignore handshake messages and gracefully
       close in order to send error messages.
    */
    try
    {
        int flags = 0;
        int retries = 7;
        std::vector<char> buffer(READ_BUFFER_SIZE * 100);

        const Poco::Timespan waitTime(POLL_TIMEOUT_MS * 1000 / retries);
        do
        {
            if (ws.poll(Poco::Timespan(0), Poco::Net::Socket::SelectMode::SELECT_ERROR))
            {
                // Already disconnected, can't send 'close' frame.
                ws.close();
                return;
            }

            // Let the client know we are shutting down.
            ws.sendFrame(error.data(), error.size());

            // Ignore incoming messages.
            if (ws.poll(waitTime, Poco::Net::Socket::SELECT_READ))
            {
                ws.receiveFrame(buffer.data(), buffer.capacity(), flags);
            }

            // Shutdown.
            ws.shutdown(WebSocket::WS_POLICY_VIOLATION);
        }
        while (--retries > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
    }
    catch (const std::exception& ex)
    {
        LOG_ERR("Error while shuting down socket on reaching limit: " << ex.what());
        try
        {
            // Persist, in case it was unrelated error.
            ws.shutdown(WebSocket::WS_POLICY_VIOLATION);
        }
        catch (const std::exception&)
        {
            // Nothing to do.
        }
    }
}

/// Internal implementation to alert all clients
/// connected to any document.
void alertAllUsersInternal(const std::string& msg)
{
    Util::assertIsLocked(DocBrokersMutex);

    LOG_INF("Alerting all users: [" << msg << "]");

    for (auto& brokerIt : DocBrokers)
    {
        auto lock = brokerIt.second->getLock();
        brokerIt.second->alertAllUsers(msg);
    }
}
}

/// Remove dead and idle DocBrokers.
/// The client of idle document should've greyed-out long ago.
/// Returns true if at least one is removed.
bool cleanupDocBrokers()
{
    Util::assertIsLocked(DocBrokersMutex);

    const auto count = DocBrokers.size();
    for (auto it = DocBrokers.begin(); it != DocBrokers.end(); )
    {
        auto docBroker = it->second;
        auto lock = docBroker->getDeferredLock();
        if (!lock.try_lock())
        {
            // Document busy at the moment, cleanup later.
            ++it;
            continue;
        }

        // Remove idle documents after 1 hour.
        const bool idle = (docBroker->getIdleTimeSecs() >= 3600);

        // Cleanup used and dead entries.
        if (docBroker->isLoaded() &&
            (docBroker->getSessionsCount() == 0 || !docBroker->isAlive() || idle))
        {
            LOG_INF("Removing " << (idle ? "idle" : "dead") <<
                    " DocumentBroker for docKey [" << it->first << "].");
            it = DocBrokers.erase(it);
            docBroker->terminateChild(lock, idle ? "idle" : "");
        }
        else
        {
            ++it;
        }
    }

    if (count != DocBrokers.size())
    {
        LOG_TRC("Have " << DocBrokers.size() << " DocBrokers after cleanup.");
        for (auto& pair : DocBrokers)
        {
            LOG_TRC("DocumentBroker [" << pair.first << "].");
        }

        return true;
    }

    return false;
}

/// Forks as many children as requested.
/// Returns the number of children requested to spawn,
/// -1 for error.
static bool forkChildren(const int number)
{
    Util::assertIsLocked(DocBrokersMutex);
    Util::assertIsLocked(NewChildrenMutex);

    if (number > 0)
    {
        const std::string fs = FileUtil::checkDiskSpaceOnRegisteredFileSystems(false);
        if (!fs.empty())
        {
            LOG_WRN("File system of " << fs << " dangerously low on disk space");
            alertAllUsersInternal("error: cmd=internal kind=diskfull");
        }

#ifdef KIT_IN_PROCESS
        forkLibreOfficeKit(LOOLWSD::ChildRoot, LOOLWSD::SysTemplate, LOOLWSD::LoTemplate, LO_JAIL_SUBPATH, number);
#else
        const std::string aMessage = "spawn " + std::to_string(number) + "\n";
        LOG_DBG("MasterToForKit: " << aMessage.substr(0, aMessage.length() - 1));
        if (IoUtil::writeToPipe(LOOLWSD::ForKitWritePipe, aMessage) > 0)
#endif
        {
            OutstandingForks += number;
            LastForkRequestTime = std::chrono::steady_clock::now();
            return number;
        }

        LOG_ERR("No forkit pipe while rebalancing children.");
        return -1; // Fail.
    }

    return 0;
}

/// Cleans up dead children.
/// Returns true if removed at least one.
static bool cleanupChildren()
{
    Util::assertIsLocked(NewChildrenMutex);

    bool removed = false;
    for (int i = NewChildren.size() - 1; i >= 0; --i)
    {
        if (!NewChildren[i]->isAlive())
        {
            LOG_WRN("Removing dead spare child [" << NewChildren[i]->getPid() << "].");
            NewChildren.erase(NewChildren.begin() + i);
            removed = true;
        }
    }

    return removed;
}

/// Decides how many children need spawning and spanws.
/// When force is true, no check of elapsed time since last request is done.
/// Returns the number of children requested to spawn,
/// -1 for error.
static int rebalanceChildren(int balance)
{
    Util::assertIsLocked(DocBrokersMutex);
    Util::assertIsLocked(NewChildrenMutex);

    // Do the cleanup first.
    const bool rebalance = cleanupChildren();

    const auto duration = (std::chrono::steady_clock::now() - LastForkRequestTime);
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    if (OutstandingForks > 0 && durationMs >= CHILD_TIMEOUT_MS)
    {
        // Children taking too long to spawn.
        // Forget we had requested any, and request anew.
        LOG_WRN("ForKit not responsive for " << durationMs << " ms forking " <<
                OutstandingForks << " children. Resetting.");
        OutstandingForks = 0;
    }

    const auto available = NewChildren.size();
    balance -= available;
    balance -= OutstandingForks;

    if (balance > 0 && (rebalance || OutstandingForks == 0))
    {
        LOG_DBG("prespawnChildren: Have " << available << " spare " <<
                (available == 1 ? "child" : "children") << ", and " <<
                OutstandingForks << " outstanding, forking " << balance << " more.");
        return forkChildren(balance);
    }

    return 0;
}

#ifndef KIT_IN_PROCESS
/// Called on startup only.
static void preForkChildren(std::unique_lock<std::mutex>& lock)
{
    Util::assertIsLocked(DocBrokersMutex);
    Util::assertIsLocked(lock);

    int numPreSpawn = LOOLWSD::NumPreSpawnedChildren;
    UnitWSD::get().preSpawnCount(numPreSpawn);

    // Wait until we have at least one child.
    // With valgrind we need extended time to spawn kits.
#ifdef KIT_IN_PROCESS
    const auto timeoutMs = CHILD_TIMEOUT_MS * 3;
#else
    const auto timeoutMs = CHILD_TIMEOUT_MS * (LOOLWSD::NoCapsForKit ? 150 : 3);
#endif
    const auto timeout = std::chrono::milliseconds(timeoutMs);
    LOG_TRC("Waiting for a new child for a max of " << timeoutMs << " ms.");
    NewChildrenCV.wait_for(lock, timeout, []() { return !NewChildren.empty(); });

    // Now spawn more, as needed.
    rebalanceChildren(numPreSpawn);

    // Make sure we have at least one before moving forward.
    LOG_TRC("Waiting for a new child for a max of " << timeoutMs << " ms.");
    if (!NewChildrenCV.wait_for(lock, timeout, []() { return !NewChildren.empty(); }))
    {
        const auto msg = "Failed to fork child processes.";
        LOG_FTL(msg);
        throw std::runtime_error(msg);
    }
}
#endif

/// Proactively spawn children processes
/// to load documents with alacrity.
/// Returns true only if at least one child was requested to spawn.
static bool prespawnChildren()
{
    // First remove dead DocBrokers, if possible.
    std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex, std::defer_lock);
    if (!docBrokersLock.try_lock())
    {
        // Busy, try again later.
        return false;
    }

    cleanupDocBrokers();

    std::unique_lock<std::mutex> lock(NewChildrenMutex, std::defer_lock);
    if (!lock.try_lock())
    {
        // We are forking already? Try later.
        return false;
    }

    const int numPreSpawn = LOOLWSD::NumPreSpawnedChildren;
    return (rebalanceChildren(numPreSpawn) > 0);
}

static size_t addNewChild(const std::shared_ptr<ChildProcess>& child)
{
    std::unique_lock<std::mutex> lock(NewChildrenMutex);

    --OutstandingForks;
    NewChildren.emplace_back(child);
    const auto count = NewChildren.size();
    LOG_INF("Have " << count << " spare " <<
            (count == 1 ? "child" : "children") << " after adding [" << child->getPid() << "].");
    lock.unlock();

    NewChildrenCV.notify_one();
    return count;
}

static std::shared_ptr<ChildProcess> getNewChild()
{
    Util::assertIsLocked(DocBrokersMutex);
    std::unique_lock<std::mutex> lock(NewChildrenMutex);

    namespace chrono = std::chrono;
    const auto startTime = chrono::steady_clock::now();
    do
    {
        LOG_DBG("getNewChild: Rebalancing children.");
        int numPreSpawn = LOOLWSD::NumPreSpawnedChildren;
        ++numPreSpawn; // Replace the one we'll dispatch just now.
        if (rebalanceChildren(numPreSpawn) < 0)
        {
            // Fatal. Let's fail and retry at a higher level.
            LOG_DBG("getNewChild: rebalancing of children failed.");
            return nullptr;
        }

        // With valgrind we need extended time to spawn kits.
#ifdef KIT_IN_PROCESS
        const auto timeoutMs = CHILD_TIMEOUT_MS;
#else
        const auto timeoutMs = CHILD_TIMEOUT_MS * (LOOLWSD::NoCapsForKit ? 100 : 1);
#endif
        LOG_TRC("Waiting for a new child for a max of " << timeoutMs << " ms.");
        const auto timeout = chrono::milliseconds(timeoutMs);
        if (NewChildrenCV.wait_for(lock, timeout, []() { return !NewChildren.empty(); }))
        {
            auto child = NewChildren.back();
            NewChildren.pop_back();
            const auto available = NewChildren.size();

            // Validate before returning.
            if (child && child->isAlive())
            {
                LOG_DBG("getNewChild: Have " << available << " spare " <<
                        (available == 1 ? "child" : "children") <<
                        " after poping [" << child->getPid() << "] to return.");
                return child;
            }

            LOG_WRN("getNewChild: popped dead child, need to find another.");
        }
        else
        {
            LOG_WRN("getNewChild: No available child. Sending spawn request to forkit and failing.");
        }
    }
    while (chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - startTime).count() <
           CHILD_TIMEOUT_MS * 4);

    LOG_DBG("getNewChild: Timed out while waiting for new child.");
    return nullptr;
}

/// Handles the filename part of the convert-to POST request payload.
class ConvertToPartHandler : public PartHandler
{
    std::string& _filename;
public:
    ConvertToPartHandler(std::string& filename)
        : _filename(filename)
    {
    }

    virtual void handlePart(const MessageHeader& header, std::istream& stream) override
    {
        // Extract filename and put it to a temporary directory.
        std::string disp;
        NameValueCollection params;
        if (header.has("Content-Disposition"))
        {
            std::string cd = header.get("Content-Disposition");
            MessageHeader::splitParameters(cd, disp, params);
        }

        if (!params.has("filename"))
            return;

        Path tempPath = Path::forDirectory(Poco::TemporaryFile::tempName() + "/");
        File(tempPath).createDirectories();
        // Prevent user inputting anything funny here.
        // A "filename" should always be a filename, not a path
        const Path filenameParam(params.get("filename"));
        tempPath.setFileName(filenameParam.getFileName());
        _filename = tempPath.toString();

        // Copy the stream to _filename.
        std::ofstream fileStream;
        fileStream.open(_filename);
        StreamCopier::copyStream(stream, fileStream);
        fileStream.close();
    }
};

#if 0 // loolnb
/// Handle a public connection from a client.
class ClientRequestHandler : public HTTPRequestHandler
{
private:
    static std::string getContentType(const std::string& fileName)
    {
        const std::string nodePath = Poco::format("//[@ext='%s']", Poco::Path(fileName).getExtension());
        std::string discPath = Path(Application::instance().commandPath()).parent().toString() + "discovery.xml";
        if (!File(discPath).exists())
        {
            discPath = LOOLWSD::FileServerRoot + "/discovery.xml";
        }

        InputSource input(discPath);
        DOMParser domParser;
        AutoPtr<Poco::XML::Document> doc = domParser.parse(&input);
        // TODO. discovery.xml missing application/pdf
        Node* node = doc->getNodeByPath(nodePath);
        if (node && (node = node->parentNode()) && node->hasAttributes())
        {
            return dynamic_cast<Element*>(node)->getAttribute("name");
        }

        return "application/octet-stream";
    }

    /// Handle POST requests.
    /// Always throw on error, do not set response status here.
    /// Returns true if a response has been sent.
    static bool handlePostRequest(HTTPServerRequest& request, HTTPServerResponse& response, const std::string& id)
    {
        LOG_INF("Post request: [" << request.getURI() << "]");
        StringTokenizer tokens(request.getURI(), "/?");
        if (tokens.count() >= 3 && tokens[2] == "convert-to")
        {
            std::string fromPath;
            ConvertToPartHandler handler(fromPath);
            HTMLForm form(request, request.stream(), handler);
            const std::string format = (form.has("format") ? form.get("format") : "");

            bool sent = false;
            if (!fromPath.empty())
            {
                if (!format.empty())
                {
                    LOG_INF("Conversion request for URI [" << fromPath << "].");

                    auto uriPublic = DocumentBroker::sanitizeURI(fromPath);
                    const auto docKey = DocumentBroker::getDocKey(uriPublic);

                    // This lock could become a bottleneck.
                    // In that case, we can use a pool and index by publicPath.
                    std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);

                    // Request a kit process for this doc.
                    auto child = getNewChild();
                    if (!child)
                    {
                        // Let the client know we can't serve now.
                        throw std::runtime_error("Failed to spawn lokit child.");
                    }

                    LOG_DBG("New DocumentBroker for docKey [" << docKey << "].");
                    auto docBroker = std::make_shared<DocumentBroker>(fromPath, uriPublic, docKey, LOOLWSD::ChildRoot, child);
                    child->setDocumentBroker(docBroker);

                    cleanupDocBrokers();

                    // FIXME: What if the same document is already open? Need a fake dockey here?
                    LOG_DBG("New DocumentBroker for docKey [" << docKey << "].");
                    DocBrokers.emplace(docKey, docBroker);
                    LOG_TRC("Have " << DocBrokers.size() << " DocBrokers after inserting [" << docKey << "].");

                    // Load the document.
                    std::shared_ptr<LOOLWebSocket> ws;
                    auto session = std::make_shared<ClientSession>(id, ws, docBroker, uriPublic);

                    auto lock = docBroker->getLock();
                    auto sessionsCount = docBroker->addSession(session);
                    lock.unlock();
                    LOG_TRC(docKey << ", ws_sessions++: " << sessionsCount);

                    docBrokersLock.unlock();

                    std::string encodedFrom;
                    URI::encode(docBroker->getPublicUri().getPath(), "", encodedFrom);
                    const std::string load = "load url=" + encodedFrom;
                    session->handleInput(load.data(), load.size());

                    // FIXME: Check for security violations.
                    Path toPath(docBroker->getPublicUri().getPath());
                    toPath.setExtension(format);
                    const std::string toJailURL = "file://" + std::string(JAILED_DOCUMENT_ROOT) + toPath.getFileName();
                    std::string encodedTo;
                    URI::encode(toJailURL, "", encodedTo);

                    // Convert it to the requested format.
                    const auto saveas = "saveas url=" + encodedTo + " format=" + format + " options=";
                    session->handleInput(saveas.data(), saveas.size());

                    // Send it back to the client.
                    try
                    {
                        Poco::URI resultURL(session->getSaveAsUrl(COMMAND_TIMEOUT_MS));
                        LOG_TRC("Save-as URL: " << resultURL.toString());

                        if (!resultURL.getPath().empty())
                        {
                            const std::string mimeType = "application/octet-stream";
                            std::string encodedFilePath;
                            URI::encode(resultURL.getPath(), "", encodedFilePath);
                            LOG_TRC("Sending file: " << encodedFilePath);
                            response.sendFile(encodedFilePath, mimeType);
                            sent = true;
                        }
                    }
                    catch (const std::exception& ex)
                    {
                        LOG_ERR("Failed to get save-as url: " << ex.what());
                    }

                    docBrokersLock.lock();
                    auto docLock = docBroker->getLock();
                    sessionsCount = docBroker->removeSession(id);
                    if (sessionsCount == 0)
                    {
                        // At this point we're done.
                        LOG_DBG("Removing DocumentBroker for docKey [" << docKey << "].");
                        DocBrokers.erase(docKey);
                        docBroker->terminateChild(docLock, "");
                        LOG_TRC("Have " << DocBrokers.size() << " DocBrokers after removing [" << docKey << "].");
                    }
                    else
                    {
                        LOG_ERR("Multiple sessions during conversion. " << sessionsCount << " sessions remain.");
                    }
                }

                // Clean up the temporary directory the HTMLForm ctor created.
                Path tempDirectory(fromPath);
                tempDirectory.setFileName("");
                FileUtil::removeFile(tempDirectory, /*recursive=*/true);
            }

            if (!sent)
            {
                // TODO: We should differentiate between bad request and failed conversion.
                throw BadRequestException("Failed to convert and send file.");
            }

            return true;
        }
        else if (tokens.count() >= 4 && tokens[3] == "insertfile")
        {
            LOG_INF("Insert file request.");
            response.set("Access-Control-Allow-Origin", "*");
            response.set("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.set("Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");

            std::string tmpPath;
            ConvertToPartHandler handler(tmpPath);
            HTMLForm form(request, request.stream(), handler);

            if (form.has("childid") && form.has("name"))
            {
                const std::string formChildid(form.get("childid"));
                const std::string formName(form.get("name"));

                // Validate the docKey
                std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
                std::string decodedUri;
                URI::decode(tokens[2], decodedUri);
                const auto docKey = DocumentBroker::getDocKey(DocumentBroker::sanitizeURI(decodedUri));
                auto docBrokerIt = DocBrokers.find(docKey);

                // Maybe just free the client from sending childid in form ?
                if (docBrokerIt == DocBrokers.end() || docBrokerIt->second->getJailId() != formChildid)
                {
                    throw BadRequestException("DocKey [" + docKey + "] or childid [" + formChildid + "] is invalid.");
                }
                docBrokersLock.unlock();

                // protect against attempts to inject something funny here
                if (formChildid.find('/') == std::string::npos && formName.find('/') == std::string::npos)
                {
                    LOG_INF("Perform insertfile: " << formChildid << ", " << formName);
                    const std::string dirPath = LOOLWSD::ChildRoot + formChildid
                                              + JAILED_DOCUMENT_ROOT + "insertfile";
                    File(dirPath).createDirectories();
                    std::string fileName = dirPath + "/" + form.get("name");
                    File(tmpPath).moveTo(fileName);
                    return false;
                }
            }
        }
        else if (tokens.count() >= 6)
        {
            LOG_INF("File download request.");
            // TODO: Check that the user in question has access to this file!

            // 1. Validate the dockey
            std::string decodedUri;
            URI::decode(tokens[2], decodedUri);
            const auto docKey = DocumentBroker::getDocKey(DocumentBroker::sanitizeURI(decodedUri));
            std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
            auto docBrokerIt = DocBrokers.find(docKey);
            if (docBrokerIt == DocBrokers.end())
            {
                throw BadRequestException("DocKey [" + docKey + "] is invalid.");
            }

            // 2. Cross-check if received child id is correct
            if (docBrokerIt->second->getJailId() != tokens[3])
            {
                throw BadRequestException("ChildId does not correspond to docKey");
            }

            // 3. Don't let user download the file in main doc directory containing
            // the document being edited otherwise we will end up deleting main directory
            // after download finishes
            if (docBrokerIt->second->getJailId() == tokens[4])
            {
                throw BadRequestException("RandomDir cannot be equal to ChildId");
            }
            docBrokersLock.unlock();

            std::string fileName;
            bool responded = false;
            URI::decode(tokens[5], fileName);
            const Path filePath(LOOLWSD::ChildRoot + tokens[3]
                                + JAILED_DOCUMENT_ROOT + tokens[4] + "/" + fileName);
            LOG_INF("HTTP request for: " << filePath.toString());
            if (filePath.isAbsolute() && File(filePath).exists())
            {
                std::string contentType = getContentType(fileName);
                response.set("Access-Control-Allow-Origin", "*");
                if (Poco::Path(fileName).getExtension() == "pdf")
                {
                    contentType = "application/pdf";
                    response.set("Content-Disposition", "attachment; filename=\"" + fileName + "\"");
                }

                try
                {
                    response.sendFile(filePath.toString(), contentType);
                    responded = true;
                }
                catch (const Exception& exc)
                {
                    LOG_ERR("Error sending file to client: " << exc.displayText() <<
                            (exc.nested() ? " (" + exc.nested()->displayText() + ")" : ""));
                }

                FileUtil::removeFile(File(filePath.parent()).path(), true);
            }
            else
            {
                LOG_ERR("Download file [" << filePath.toString() << "] not found.");
            }

            return responded;
        }

        throw BadRequestException("Invalid or unknown request.");
    }

    /// Handle GET requests.
    static void handleGetRequest(const std::string& uri, std::shared_ptr<LOOLWebSocket>& ws, const std::string& id)
    {
        LOG_INF("Starting GET request handler for session [" << id << "] on url [" << uri << "].");
        try
        {
            // First, setup WS options.
            // We need blocking here, because the POCO's
            // non-blocking implementation of websockes is
            // broken; essentially it leads to sending
            // incomplete frames.
            ws->setBlocking(true);
            ws->setSendTimeout(WS_SEND_TIMEOUT_MS * 1000);

            // Indicate to the client that document broker is searching.
            const std::string status("statusindicator: find");
            LOG_TRC("Sending to Client [" << status << "].");
            ws->sendFrame(status.data(), status.size());

            const auto uriPublic = DocumentBroker::sanitizeURI(uri);
            const auto docKey = DocumentBroker::getDocKey(uriPublic);
            LOG_INF("Sanitized URI [" << uri << "] to [" << uriPublic.toString() <<
                    "] and mapped to docKey [" << docKey << "] for session [" << id << "].");

            // Check if readonly session is required
            bool isReadOnly = false;
            for (const auto& param : uriPublic.getQueryParameters())
            {
                LOG_DBG("Query param: " << param.first << ", value: " << param.second);
                if (param.first == "permission" && param.second == "readonly")
                {
                    isReadOnly = true;
                }
            }

            int retry = 3;
            while (retry-- > 0)
            {
                auto docBroker = findOrCreateDocBroker(uri, docKey, ws, id, uriPublic);
                if (docBroker)
                {
                    auto session = createNewClientSession(ws, id, uriPublic, docBroker, isReadOnly);
                    if (session)
                    {
                        // Process the request in an exception-safe way.
                        processGetRequest(ws, id, docBroker, session);
                        break;
                    }
                }

                if (retry > 0)
                {
                    LOG_WRN("Failed to connect DocBroker and Client Session, retrying.");
                    LOOLWSD::checkAndRestoreForKit();
                }
                else
                {
                    const std::string msg = SERVICE_UNAVAILABLE_INTERNAL_ERROR;
                    LOG_ERR("handleGetRequest: Giving up trying to connect client: " << msg);
                    try
                    {
                        ws->sendFrame(msg.data(), msg.size());
                        // abnormal close frame handshake
                        ws->shutdown(WebSocket::WS_ENDPOINT_GOING_AWAY);
                    }
                    catch (const std::exception& exc2)
                    {
                        LOG_ERR("handleGetRequest: exception while sending WS error message [" << msg << "]: " << exc2.what());
                    }

                    break;
                }
            }
        }
        catch (const std::exception& exc)
        {
            LOG_INF("Finished GET request handler for session [" << id << "] on uri [" << uri << "] with exception: " << exc.what());
            throw;
        }

        LOG_INF("Finished GET request handler for session [" << id << "] on uri [" << uri << "].");
    }

    /// Find the DocumentBroker for the given docKey, if one exists.
    /// Otherwise, creates and adds a new one to DocBrokers.
    /// May return null if terminating or MaxDocuments limit is reached.
    /// After returning a valid instance DocBrokers must be cleaned up after exceptions.
    static std::shared_ptr<DocumentBroker> findOrCreateDocBroker(const std::string& uri,
                                                                 const std::string& docKey,
                                                                 std::shared_ptr<LOOLWebSocket>& ws,
                                                                 const std::string& id,
                                                                 const Poco::URI& uriPublic)
    {
        LOG_INF("Find or create DocBroker for docKey [" << docKey <<
                "] for session [" << id << "] on url [" << uriPublic.toString() << "].");

        std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);

        cleanupDocBrokers();

        if (TerminationFlag)
        {
            LOG_ERR("Termination flag set. No loading new session [" << id << "]");
            return nullptr;
        }

        std::shared_ptr<DocumentBroker> docBroker;

        // Lookup this document.
        auto it = DocBrokers.find(docKey);
        if (it != DocBrokers.end() && it->second)
        {
            // Get the DocumentBroker from the Cache.
            LOG_DBG("Found DocumentBroker with docKey [" << docKey << "].");
            docBroker = it->second;
            if (docBroker->isMarkedToDestroy())
            {
                // Let the waiting happen in parallel to new requests.
                docBrokersLock.unlock();

                // If this document is going out, wait.
                LOG_DBG("Document [" << docKey << "] is marked to destroy, waiting to reload.");

                bool timedOut = true;
                for (size_t i = 0; i < COMMAND_TIMEOUT_MS / POLL_TIMEOUT_MS; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(POLL_TIMEOUT_MS));

                    docBrokersLock.lock();
                    it = DocBrokers.find(docKey);
                    if (it == DocBrokers.end())
                    {
                        // went away successfully
                        docBroker.reset();
                        docBrokersLock.unlock();
                        timedOut = false;
                        break;
                    }
                    else if (it->second && !it->second->isMarkedToDestroy())
                    {
                        // was actually replaced by a real document
                        docBroker = it->second;
                        docBrokersLock.unlock();
                        timedOut = false;
                        break;
                    }

                    docBrokersLock.unlock();
                    if (TerminationFlag)
                    {
                        LOG_ERR("Termination flag set. Not loading new session [" << id << "]");
                        return nullptr;
                    }
                }

                if (timedOut)
                {
                    // Still here, but marked to destroy. Proceed and hope to recover.
                    LOG_ERR("Timed out while waiting for document to unload before loading.");
                }

                // Retake the lock and recheck if another thread created the DocBroker.
                docBrokersLock.lock();
                it = DocBrokers.find(docKey);
                if (it != DocBrokers.end())
                {
                    // Get the DocumentBroker from the Cache.
                    LOG_DBG("Found DocumentBroker for docKey [" << docKey << "].");
                    docBroker = it->second;
                    assert(docBroker);
                }
            }
        }
        else
        {
            LOG_DBG("No DocumentBroker with docKey [" << docKey << "] found. New Child and Document.");
        }

        Util::assertIsLocked(docBrokersLock);

        if (TerminationFlag)
        {
            LOG_ERR("Termination flag set. No loading new session [" << id << "]");
            return nullptr;
        }

        // Indicate to the client that we're connecting to the docbroker.
        const std::string statusConnect = "statusindicator: connect";
        LOG_TRC("Sending to Client [" << statusConnect << "].");
        ws->sendFrame(statusConnect.data(), statusConnect.size());

        if (!docBroker)
        {
            docBroker = createNewDocBroker(uri, docKey, ws, uriPublic);
        }

        return docBroker;
    }

    static std::shared_ptr<DocumentBroker> createNewDocBroker(const std::string& uri,
                                                              const std::string& docKey,
                                                              std::shared_ptr<LOOLWebSocket>& ws,
                                                              const Poco::URI& uriPublic)
    {
        Util::assertIsLocked(DocBrokersMutex);

        static_assert(MAX_DOCUMENTS > 0, "MAX_DOCUMENTS must be positive");
        if (DocBrokers.size() + 1 > MAX_DOCUMENTS)
        {
            LOG_ERR("Maximum number of open documents reached.");
            shutdownLimitReached(*ws);
            return nullptr;
        }

        // Request a kit process for this doc.
        auto child = getNewChild();
        if (!child)
        {
            // Let the client know we can't serve now.
            LOG_ERR("Failed to get new child.");
            return nullptr;
        }

        // Set the one we just created.
        LOG_DBG("New DocumentBroker for docKey [" << docKey << "].");
        auto docBroker = std::make_shared<DocumentBroker>(uri, uriPublic, docKey, LOOLWSD::ChildRoot, child);
        child->setDocumentBroker(docBroker);
        DocBrokers.emplace(docKey, docBroker);
        LOG_TRC("Have " << DocBrokers.size() << " DocBrokers after inserting [" << docKey << "].");

        return docBroker;
    }

    static std::shared_ptr<ClientSession> createNewClientSession(std::shared_ptr<LOOLWebSocket>& ws,
                                                                 const std::string& id,
                                                                 const Poco::URI& uriPublic,
                                                                 const std::shared_ptr<DocumentBroker>& docBroker,
                                                                 const bool isReadOnly)
    {
        LOG_CHECK_RET(docBroker && "Null docBroker instance", nullptr);
        try
        {
            auto lock = docBroker->getLock();

            // Validate the broker.
            if (!docBroker->isAlive())
            {
                LOG_ERR("DocBroker is invalid or premature termination of child process.");
                lock.unlock();
                removeDocBrokerSession(docBroker);
                return nullptr;
            }

            if (docBroker->isMarkedToDestroy())
            {
                LOG_ERR("DocBroker is marked to destroy, can't add session.");
                lock.unlock();
                removeDocBrokerSession(docBroker);
                return nullptr;
            }

            // Now we have a DocumentBroker and we're ready to process client commands.
            const std::string statusReady = "statusindicator: ready";
            LOG_TRC("Sending to Client [" << statusReady << "].");
            ws->sendFrame(statusReady.data(), statusReady.size());

            // In case of WOPI, if this session is not set as readonly, it might be set so
            // later after making a call to WOPI host which tells us the permission on files
            // (UserCanWrite param).
            auto session = std::make_shared<ClientSession>(id, ws, docBroker, uriPublic, isReadOnly);

            docBroker->addSession(session);

            lock.unlock();

            const std::string fs = FileUtil::checkDiskSpaceOnRegisteredFileSystems();
            if (!fs.empty())
            {
                LOG_WRN("File system of [" << fs << "] is dangerously low on disk space.");
                const std::string diskfullMsg = "error: cmd=internal kind=diskfull";
                // Alert all other existing sessions also
                Util::alertAllUsers(diskfullMsg);
            }

            return session;
        }
        catch (const std::exception& exc)
        {
            LOG_WRN("Exception while preparing session [" << id << "]: " << exc.what());
            removeDocBrokerSession(docBroker, id);
        }

        return nullptr;
    }

    /// Remove DocumentBroker session and instance from DocBrokers.
    static void removeDocBrokerSession(const std::shared_ptr<DocumentBroker>& docBroker, const std::string& id = "")
    {
        LOG_CHECK_RET(docBroker && "Null docBroker instance", );

        const auto docKey = docBroker->getDocKey();
        LOG_DBG("Removing docBroker [" << docKey << "]" << (id.empty() ? "" : (" and session [" + id + "].")));

        std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
        auto lock = docBroker->getLock();

        if (!id.empty())
        {
            docBroker->removeSession(id);
        }

        if (docBroker->getSessionsCount() == 0 || !docBroker->isAlive())
        {
            LOG_INF("Removing unloaded DocumentBroker for docKey [" << docKey << "].");
            DocBrokers.erase(docKey);
            docBroker->terminateChild(lock, "");
        }
    }

    /// Process GET requests.
    static void processGetRequest(std::shared_ptr<LOOLWebSocket>& ws,
                                  const std::string& id,
                                  const std::shared_ptr<DocumentBroker>& docBroker,
                                  const std::shared_ptr<ClientSession>& session)
    {
        LOG_CHECK_RET(docBroker && "Null docBroker instance", );
        const auto docKey = docBroker->getDocKey();
        LOG_CHECK_RET(session && "Null ClientSession instance", );
        try
        {
            // Let messages flow.
            IoUtil::SocketProcessor(ws, "client_ws_" + id,
                [&session](const std::vector<char>& payload)
                {
                    return session->handleInput(payload.data(), payload.size());
                },
                [&session]() { session->closeFrame(); },
                []() { return TerminationFlag || SigUtil::isShuttingDown(); });

            // Connection terminated. Destroy session.
            LOG_DBG("Client session [" << id << "] on docKey [" << docKey << "] terminated. Cleaning up.");

            auto docLock = docBroker->getLock();

            // We issue a force-save when last editable (non-readonly) session is going away
            const bool forceSave = docBroker->startDestroy(id);
            if (forceSave)
            {
                LOG_INF("Shutdown of the last editable (non-readonly) session, saving the document before tearing down.");
            }

            // We need to wait until the save notification reaches us
            // and Storage persists the document.
            if (!docBroker->autoSave(forceSave, COMMAND_TIMEOUT_MS, docLock))
            {
                LOG_ERR("Auto-save before closing failed.");
            }

            const auto sessionsCount = docBroker->removeSession(id);
            docLock.unlock();

            if (sessionsCount == 0)
            {
                // We've supposedly destroyed the last session, now cleanup.
                removeDocBrokerSession(docBroker);
            }

            LOG_INF("Finishing GET request handler for session [" << id << "].");
        }
        catch (const UnauthorizedRequestException& exc)
        {
            LOG_ERR("Error in client request handler: " << exc.toString());
            const std::string status = "error: cmd=internal kind=unauthorized";
            LOG_TRC("Sending to Client [" << status << "].");
            ws->sendFrame(status.data(), status.size());
        }
        catch (const std::exception& exc)
        {
            LOG_ERR("Error in client request handler: " << exc.what());
        }

        try
        {
            if (session->isCloseFrame())
            {
                LOG_TRC("Normal close handshake.");
                // Client initiated close handshake
                // respond close frame
                ws->shutdown();
            }
            else if (!SigUtil::isShuttingDown())
            {
                // something wrong, with internal exceptions
                LOG_TRC("Abnormal close handshake.");
                session->closeFrame();
                ws->shutdown(WebSocket::WS_ENDPOINT_GOING_AWAY);
            }
            else
            {
                std::lock_guard<std::mutex> lock(ClientWebSocketsMutex);
                LOG_TRC("Capturing Client WS for [" << id << "]");
                ClientWebSockets.push_back(ws);
            }
        }
        catch (const std::exception& exc)
        {
            LOG_WRN("Exception while closing socket for session [" << id <<
                    "] of docKey [" << docKey << "]: " << exc.what());
        }
    }

    /// Sends back the WOPI Discovery XML.
    /// The XML needs to be preprocessed to stamp the correct URL etc.
    /// Returns true if a response has been sent.
    static bool handleGetWOPIDiscovery(HTTPServerRequest& request, HTTPServerResponse& response)
    {
        std::string discoveryPath = Path(Application::instance().commandPath()).parent().toString() + "discovery.xml";
        if (!File(discoveryPath).exists())
        {
            discoveryPath = LOOLWSD::FileServerRoot + "/discovery.xml";
        }

        const std::string mediaType = "text/xml";
        const std::string action = "action";
        const std::string urlsrc = "urlsrc";
        const auto& config = Application::instance().config();
        const std::string loleafletHtml = config.getString("loleaflet_html", "loleaflet.html");
        const std::string uriValue = ((LOOLWSD::isSSLEnabled() || LOOLWSD::isSSLTermination()) ? "https://" : "http://")
                                   + (LOOLWSD::ServerName.empty() ? request.getHost() : LOOLWSD::ServerName)
                                   + "/loleaflet/" LOOLWSD_VERSION_HASH "/" + loleafletHtml + '?';

        InputSource inputSrc(discoveryPath);
        DOMParser parser;
        AutoPtr<Poco::XML::Document> docXML = parser.parse(&inputSrc);
        AutoPtr<NodeList> listNodes = docXML->getElementsByTagName(action);

        for (unsigned long it = 0; it < listNodes->length(); ++it)
        {
            static_cast<Element*>(listNodes->item(it))->setAttribute(urlsrc, uriValue);
        }

        std::ostringstream ostrXML;
        DOMWriter writer;
        writer.writeNode(ostrXML, docXML);

        response.set("User-Agent", "LOOLWSD WOPI Agent");
        response.setContentLength(ostrXML.str().length());
        response.setContentType(mediaType);
        response.setChunkedTransferEncoding(false);

        std::ostream& ostr = response.send();
        ostr << ostrXML.str();
        LOG_INF("Sent discovery.xml successfully.");
        return true;
    }

public:

    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override
    {
        if (UnitWSD::get().filterHandleRequest(
                UnitWSD::TestRequest::Client,
                request, response))
            return;

        const auto connectionNum = ++LOOLWSD::NumConnections;
        if (connectionNum > MAX_CONNECTIONS)
        {
            --LOOLWSD::NumConnections;
            LOG_ERR("Limit on maximum number of connections of " << MAX_CONNECTIONS << " reached.");
            // accept handshake
            LOOLWebSocket ws(request, response);
            shutdownLimitReached(ws);
            return;
        }

        try
        {
            const auto id = LOOLWSD::GenSessionId();
            LOG_TRC("Accepted connection #" << connectionNum << " of " <<
                    MAX_CONNECTIONS << " as session [" << id << "].");
            handleClientRequest(request, response, id);
        }
        catch (const std::exception& exc)
        {
            // Nothing to do.
        }

        --LOOLWSD::NumConnections;
    }

    static void handleClientRequest(HTTPServerRequest& request, HTTPServerResponse& response, const std::string& id)
    {
        Util::setThreadName("client_ws_" + id);

        LOG_DBG("Thread started.");

        Poco::URI requestUri(request.getURI());
        LOG_DBG("Handling: " << request.getURI());

        StringTokenizer reqPathTokens(request.getURI(), "/?", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

        bool responded = false;
        try
        {
            if ((request.getMethod() == HTTPRequest::HTTP_GET ||
                 request.getMethod() == HTTPRequest::HTTP_HEAD) &&
                request.getURI() == "/")
            {
                std::string mimeType = "text/plain";
                std::string responseString = "OK";
                response.setContentLength(responseString.length());
                response.setContentType(mimeType);
                response.setChunkedTransferEncoding(false);
                std::ostream& ostr = response.send();
                if (request.getMethod() == HTTPRequest::HTTP_GET)
                {
                    ostr << responseString;
                }
                responded = true;
            }
            else if (request.getMethod() == HTTPRequest::HTTP_GET && request.getURI() == "/favicon.ico")
            {
                std::string mimeType = "image/vnd.microsoft.icon";
                std::string faviconPath = Path(Application::instance().commandPath()).parent().toString() + "favicon.ico";
                if (!File(faviconPath).exists())
                {
                    faviconPath = LOOLWSD::FileServerRoot + "/favicon.ico";
                }
                response.setContentType(mimeType);
                response.sendFile(faviconPath, mimeType);
                responded = true;
            }
            else if (request.getMethod() == HTTPRequest::HTTP_GET && request.getURI() == "/hosting/discovery")
            {
                // http://server/hosting/discovery
                responded = handleGetWOPIDiscovery(request, response);
            }
            else if (!(request.find("Upgrade") != request.end() && Poco::icompare(request["Upgrade"], "websocket") == 0) &&
                     reqPathTokens.count() > 0 && reqPathTokens[0] == "lool")
            {
                // All post requests have url prefix 'lool'.
                responded = handlePostRequest(request, response, id);
            }
            else if (reqPathTokens.count() > 2 && reqPathTokens[0] == "lool" && reqPathTokens[2] == "ws")
            {
                auto ws = std::make_shared<LOOLWebSocket>(request, response);
                responded = true; // After upgrading to WS we should not set HTTP response.
                handleGetRequest(reqPathTokens[1], ws, id);
            }
            else
            {
                LOG_ERR("Unknown resource: " << request.getURI());
                response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
            }
        }
        catch (const Exception& exc)
        {
            LOG_ERR("ClientRequestHandler::handleClientRequest: " << exc.displayText() <<
                    (exc.nested() ? " (" + exc.nested()->displayText() + ")" : ""));
            response.setStatusAndReason(HTTPResponse::HTTP_SERVICE_UNAVAILABLE);
        }
        catch (const UnauthorizedRequestException& exc)
        {
            LOG_ERR("ClientRequestHandler::handleClientRequest: UnauthorizedException: " << exc.toString());
            response.setStatusAndReason(HTTPResponse::HTTP_UNAUTHORIZED);
        }
        catch (const BadRequestException& exc)
        {
            LOG_ERR("ClientRequestHandler::handleClientRequest: BadRequestException: " << exc.toString());
            response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
        }
        catch (const std::exception& exc)
        {
            LOG_ERR("ClientRequestHandler::handleClientRequest: Exception: " << exc.what());
            response.setStatusAndReason(HTTPResponse::HTTP_SERVICE_UNAVAILABLE);
        }

        if (responded)
        {
            LOG_DBG("Already sent response!?");
        }
        else
        {
            // I wonder if this code path has ever been exercised
            LOG_DBG("Attempting to send response");
            response.setContentLength(0);
            std::ostream& os = response.send();
            LOG_DBG("Response stream " << (os.good() ? "*is*" : "is not") << " good after send.");
        }

        LOG_DBG("Thread finished.");
    }
};
#endif

/// Handler of announcements that a new loolkit process was created.
///
/// loolforkit is creating the loolkit processes.  That happens
/// completely assynchronously, and from the different process (loolforkit),
/// so this handler is listening for annoucements that a new loolkit process
/// has been created.
class PrisonerRequestHandler : public HTTPRequestHandler
{
public:

    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override
    {
        if (UnitWSD::get().filterHandleRequest(
                UnitWSD::TestRequest::Prisoner,
                request, response))
            return;

        handlePrisonerRequest(request, response);
    }

    static void handlePrisonerRequest(HTTPServerRequest& request, HTTPServerResponse& response)
    {
        LOG_TRC("Child connection with URI [" << request.getURI() << "].");
        assert(request.serverAddress().port() == MasterPortNumber);
        if (request.getURI().find(NEW_CHILD_URI) != 0)
        {
            LOG_ERR("Invalid incoming URI.");
            return;
        }

        // New Child is spawned.
        const auto params = Poco::URI(request.getURI()).getQueryParameters();
        Poco::Process::PID pid = -1;
        for (const auto& param : params)
        {
            if (param.first == "pid")
            {
                pid = std::stoi(param.second);
            }
            else if (param.first == "version")
            {
                LOOLWSD::LOKitVersion = param.second;
            }
        }

        if (pid <= 0)
        {
            LOG_ERR("Invalid PID in child URI [" << request.getURI() << "].");
            return;
        }

        LOG_INF("New child [" << pid << "].");
        auto ws = std::make_shared<LOOLWebSocket>(request, response);
        UnitWSD::get().newChild(ws);

        addNewChild(std::make_shared<ChildProcess>(pid, ws));
    }
};

#if 0 // loolnb
/// External (client) connection handler factory.
/// Creates handler objects.
class ClientRequestHandlerFactory : public HTTPRequestHandlerFactory
{
public:
    ClientRequestHandlerFactory()
    {
    }

    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest& request) override
    {
        Util::setThreadName("client_req_hdl");

        auto logger = Log::info();
        logger << "Request from " << request.clientAddress().toString() << ": "
               << request.getMethod() << " " << request.getURI() << " "
               << request.getVersion();

        for (const auto& it : request)
        {
            logger << " / " << it.first << ": " << it.second;
        }

        logger << Log::end;

        // Routing
        Poco::URI requestUri(request.getURI());
        std::vector<std::string> reqPathSegs;
        requestUri.getPathSegments(reqPathSegs);
        HTTPRequestHandler* requestHandler;

        // File server
        if (reqPathSegs.size() >= 1 && reqPathSegs[0] == "loleaflet")
        {
            requestHandler = FileServer::createRequestHandler();
        }
        // Admin LOOLWebSocket Connections
        else if (reqPathSegs.size() >= 2 && reqPathSegs[0] == "lool" && reqPathSegs[1] == "adminws")
        {
            requestHandler = Admin::createRequestHandler();
        }
        // Client post and websocket connections
        else
        {
            requestHandler = new ClientRequestHandler();
        }

        return requestHandler;
    }
};
#endif

/// Internal (prisoner) connection handler factory.
/// Creates handler objects.
class PrisonerRequestHandlerFactory : public HTTPRequestHandlerFactory
{
public:
    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest& request) override
    {
        Util::setThreadName("prsnr_req_hdl");

        auto logger = Log::info();
        logger << "Request from " << request.clientAddress().toString() << ": "
               << request.getMethod() << " " << request.getURI() << " "
               << request.getVersion();

        for (const auto& it : request)
        {
            logger << " / " << it.first << ": " << it.second;
        }

        logger << Log::end;
        return new PrisonerRequestHandler();
    }
};

namespace
{

inline Poco::Net::ServerSocket* getServerSocket(int portNumber, bool reuseDetails)
{
    try
    {
        // reuseDetails being true means we are not doing UnitWSD::isUnitTesting. In that case make
        // sure there isn't another loolwsd already listening on the same port, as the way we create
        // and listen on the socket doesn't prevent it otherwise. Try connecting to the port first,
        // not expecting to suceed. (Yes, there is a race condition here if multiple loolwsd
        // processes come here simultaneously. Live with it. Multiple loolwsd processes is a problem
        // that happens accidentally for developers only anyway, in production systemd takes care of
        // having just one, I hope.)

        // Note: We really should _not_ need to do this at all. The reason we do is that,
        // if we don't, when an instance of loolwsd is already running, a second instance
        // would not fail to listen to the _same_ port, and start stealing connections.
        // This would have the undesirable side-effect of bifurcating documents (meaning,
        // clients loading a document that is already loaded in the first instance of
        // wsd, if their socket is accepted by the second instance of wsd, will not
        // be able to collaborate with their peers, who are in the first instance of wsd).
        // This situation arises because Poco's Socket::bind() (when reuseAddress,
        // the second arg, is true) enables reuse for both address _and_ port.
        // The latter is damaging (and the root of this bug). It's designed (on Linux)
        // to load-balance requests on a given port to all server instances.
        // But this only works if requests are independent (as in web-page serving)
        // and fail miserably for our purposes here (collaborative editing).

        // The correct solution is to explicitly enable only address reuse
        // before bind and pass reuseAddress=false (second arg) to bind.
        // Reusing the address is fine, since that eliminates the wait
        // when we recycle and the socket from the earlier run is still
        // in TIME_WAIT.

        if (reuseDetails)
        {
            try
            {
                LOG_INF("Trying first to connect to an existing loolwsd at the same port " << portNumber);
                Poco::Net::StreamSocket s(SocketAddress("127.0.0.1:" + std::to_string(portNumber)));
                LOG_FTL("Connection succeeded, so we can't continue");
                return nullptr;
            }
            catch (const Exception&)
            {
                LOG_INF("Connection failed, so hopefully we are the only loolwsd on this port");
            }
        }

        Poco::Net::ServerSocket* socket = LOOLWSD::isSSLEnabled() ? new SecureServerSocket() : new Poco::Net::ServerSocket();
        Poco::Net::IPAddress wildcardAddr;
        SocketAddress address(wildcardAddr, portNumber);
        socket->bind(address, reuseDetails);
        // 64 is the default value for the backlog parameter in Poco
        // when creating a Poco::Net::ServerSocket, so use it here, too.
        socket->listen(64);
        return socket;
    }
    catch (const Exception& exc)
    {
        LOG_FTL("Could not create server socket: " << exc.displayText());
        return nullptr;
    }
}

inline Poco::Net::ServerSocket* findFreeServerPort(int& portNumber)
{
    Poco::Net::ServerSocket* socket = nullptr;
    while (!socket)
    {
        socket = getServerSocket(portNumber, false);
        if (!socket)
        {
            portNumber++;
            LOG_INF("client port busy - trying " << portNumber);
        }
    }
    return socket;
}

inline Poco::Net::ServerSocket* getMasterSocket(int portNumber)
{
    try
    {
        SocketAddress addr2("127.0.0.1", portNumber);
        return new Poco::Net::ServerSocket(addr2);
    }
    catch (const Exception& exc)
    {
        LOG_FTL("Could not create master socket: " << exc.displayText());
        return nullptr;
    }
}

inline Poco::Net::ServerSocket* findFreeMasterPort(int &portNumber)
{
    Poco::Net::ServerSocket* socket = nullptr;
    while (!socket)
    {
        socket = getServerSocket(portNumber, false);
        if (!socket)
        {
            portNumber++;
            LOG_INF("master port busy - trying " << portNumber);
        }
    }
    return socket;
}

inline std::string getLaunchBase(const std::string &credentials)
{
    std::ostringstream oss;
    oss << "    ";
    oss << ((LOOLWSD::isSSLEnabled() || LOOLWSD::isSSLTermination()) ? "https://" : "http://");
    oss << credentials;
    oss << LOOLWSD_TEST_HOST ":";
    oss << ClientPortNumber;

    return oss.str();
}

inline std::string getLaunchURI()
{
    const std::string aAbsTopSrcDir = Poco::Path(Application::instance().commandPath()).parent().toString();

    std::ostringstream oss;

    oss << getLaunchBase("");
    oss << LOOLWSD_TEST_LOLEAFLET_UI;
    oss << "?file_path=file://";
    oss << Poco::Path(aAbsTopSrcDir).absolute().toString();
    oss << LOOLWSD_TEST_DOCUMENT_RELATIVE_PATH;

    return oss.str();
}

inline std::string getAdminURI(const Poco::Util::LayeredConfiguration &config)
{
    std::string user = config.getString("admin_console.username", "");
    std::string passwd = config.getString("admin_console.password", "");

    if (user.empty() || passwd.empty())
        return "";

    std::ostringstream oss;

    oss << getLaunchBase(user + ":" + passwd + "@");
    oss << LOOLWSD_TEST_ADMIN_CONSOLE;

    return oss.str();
}

} // anonymous namespace

std::atomic<unsigned> LOOLWSD::NextSessionId;
#ifndef KIT_IN_PROCESS
std::atomic<int> LOOLWSD::ForKitWritePipe(-1);
std::atomic<int> LOOLWSD::ForKitProcId(-1);
bool LOOLWSD::NoCapsForKit = false;
#endif
#ifdef FUZZER
bool LOOLWSD::DummyLOK = false;
std::string LOOLWSD::FuzzFileName;
#endif
std::string LOOLWSD::Cache = LOOLWSD_CACHEDIR;
std::string LOOLWSD::SysTemplate;
std::string LOOLWSD::LoTemplate;
std::string LOOLWSD::ChildRoot;
std::string LOOLWSD::ServerName;
std::string LOOLWSD::FileServerRoot;
std::string LOOLWSD::LOKitVersion;
std::string LOOLWSD::ConfigFile = LOOLWSD_CONFIGDIR "/loolwsd.xml";
Util::RuntimeConstant<bool> LOOLWSD::SSLEnabled;
Util::RuntimeConstant<bool> LOOLWSD::SSLTermination;

static std::string UnitTestLibrary;

unsigned int LOOLWSD::NumPreSpawnedChildren = 0;
std::atomic<unsigned> LOOLWSD::NumConnections;
std::unique_ptr<TraceFileWriter> LOOLWSD::TraceDumper;

/// Helper class to hold default configuration entries.
class AppConfigMap final : public Poco::Util::MapConfiguration
{
public:
    AppConfigMap(const std::map<std::string, std::string>& map)
    {
        for (const auto& pair : map)
        {
            setRaw(pair.first, pair.second);
        }
    }
};

LOOLWSD::LOOLWSD()
{
}

LOOLWSD::~LOOLWSD()
{
}

void LOOLWSD::initialize(Application& self)
{
    if (geteuid() == 0)
    {
        throw std::runtime_error("Do not run as root. Please run as lool user.");
    }

    if (!UnitWSD::init(UnitWSD::UnitType::Wsd, UnitTestLibrary))
    {
        throw std::runtime_error("Failed to load wsd unit test library.");
    }

    auto& conf = config();

    // Add default values of new entries here.
    static const std::map<std::string, std::string> DefAppConfig
        = { { "tile_cache_path", LOOLWSD_CACHEDIR },
            { "sys_template_path", "systemplate" },
            { "lo_template_path", "/opt/collaboraoffice5.1" },
            { "child_root_path", "jails" },
            { "lo_jail_subpath", "lo" },
            { "server_name", "" },
            { "file_server_root_path", "loleaflet/.." },
            { "num_prespawn_children", "1" },
            { "per_document.max_concurrency", "4" },
            { "loleaflet_html", "loleaflet.html" },
            { "logging.color", "true" },
            { "logging.level", "trace" },
            { "loleaflet_logging", "false" },
            { "ssl.enable", "true" },
            { "ssl.termination", "true" },
            { "ssl.cert_file_path", LOOLWSD_CONFIGDIR "/cert.pem" },
            { "ssl.key_file_path", LOOLWSD_CONFIGDIR "/key.pem" },
            { "ssl.ca_file_path", LOOLWSD_CONFIGDIR "/ca-chain.cert.pem" },
            { "storage.filesystem[@allow]", "false" },
            { "storage.wopi[@allow]", "true" },
            { "storage.wopi.host[0][@allow]", "true" },
            { "storage.wopi.host[0]", "localhost" },
            { "storage.wopi.max_file_size", "0" },
            { "storage.webdav[@allow]", "false" },
            { "logging.file[@enable]", "false" },
            { "logging.file.property[0][@name]", "path" },
            { "logging.file.property[0]", "loolwsd.log" },
            { "logging.file.property[1][@name]", "rotation" },
            { "logging.file.property[1]", "never" },
            { "logging.file.property[2][@name]", "compress" },
            { "logging.file.property[2]", "true" },
            { "logging.file.property[3][@name]", "flush" },
            { "logging.file.property[3]", "false" },
            { "trace[@enable]", "false" } };

    // Set default values, in case they are missing from the config file.
    AutoPtr<AppConfigMap> defConfig(new AppConfigMap(DefAppConfig));
    conf.addWriteable(defConfig, PRIO_SYSTEM); // Lowest priority

    // Load default configuration files, if present.
    if (loadConfiguration(PRIO_DEFAULT) == 0)
    {
        // Fallback to the LOOLWSD_CONFIGDIR or --config-file path.
        loadConfiguration(ConfigFile, PRIO_DEFAULT);
    }

    // Override any settings passed on the command-line.
    AutoPtr<AppConfigMap> overrideConfig(new AppConfigMap(_overrideSettings));
    conf.addWriteable(overrideConfig, PRIO_APPLICATION); // Highest priority

    // Allow UT to manipulate before using configuration values.
    UnitWSD::get().configure(config());

    const auto logLevel = getConfigValue<std::string>(conf, "logging.level", "trace");
    setenv("LOOL_LOGLEVEL", logLevel.c_str(), true);
    const auto withColor = getConfigValue<bool>(conf, "logging.color", true) && isatty(fileno(stderr));
    if (withColor)
    {
        setenv("LOOL_LOGCOLOR", "1", true);
    }

    const auto logToFile = getConfigValue<bool>(conf, "logging.file[@enable]", false);
    std::map<std::string, std::string> logProperties;
    for (size_t i = 0; ; ++i)
    {
        const std::string confPath = "logging.file.property[" + std::to_string(i) + "]";
        const auto confName = config().getString(confPath + "[@name]", "");
        if (!confName.empty())
        {
            const auto value = config().getString(confPath, "");
            logProperties.emplace(confName, value);
        }
        else if (!config().has(confPath))
        {
            break;
        }
    }

    // Setup the logfile envar for the kit processes.
    if (logToFile)
    {
        setenv("LOOL_LOGFILE", "1", true);
        const auto it = logProperties.find("path");
        if (it != logProperties.end())
        {
            setenv("LOOL_LOGFILENAME", it->second.c_str(), true);
#if ENABLE_DEBUG
            std::cerr << "\nFull log is available in: " << it->second.c_str() << std::endl;
#endif
        }
    }

    Log::initialize("wsd", logLevel, withColor, logToFile, logProperties);

#if ENABLE_SSL
    LOOLWSD::SSLEnabled.set(getConfigValue<bool>(conf, "ssl.enable", true));
#else
    LOOLWSD::SSLEnabled.set(false);
#endif

    if (LOOLWSD::isSSLEnabled())
    {
        LOG_INF("SSL support: SSL is enabled.");
    }
    else
    {
        LOG_WRN("SSL support: SSL is disabled.");
    }

#if ENABLE_SSL
    LOOLWSD::SSLTermination.set(getConfigValue<bool>(conf, "ssl.termination", true));
#else
    LOOLWSD::SSLTermination.set(false);
#endif

    Cache = getPathFromConfig("tile_cache_path");
    SysTemplate = getPathFromConfig("sys_template_path");
    LoTemplate = getPathFromConfig("lo_template_path");
    ChildRoot = getPathFromConfig("child_root_path");
    ServerName = config().getString("server_name");

    FileServerRoot = getPathFromConfig("file_server_root_path");
    NumPreSpawnedChildren = getConfigValue<int>(conf, "num_prespawn_children", 1);
    if (NumPreSpawnedChildren < 1)
    {
        LOG_WRN("Invalid num_prespawn_children in config (" << NumPreSpawnedChildren << "). Resetting to 1.");
        NumPreSpawnedChildren = 1;
    }

    const auto maxConcurrency = getConfigValue<int>(conf, "per_document.max_concurrency", 4);
    if (maxConcurrency > 0)
    {
        setenv("MAX_CONCURRENCY", std::to_string(maxConcurrency).c_str(), 1);
    }

    // Otherwise we profile the soft-device at jail creation time.
    setenv("SAL_DISABLE_OPENCL", "true", 1);

    // Log the connection and document limits.
    static_assert(MAX_DOCUMENTS > 0 && MAX_DOCUMENTS <= MAX_CONNECTIONS, "MAX_DOCUMENTS must be positive and no more than MAX_CONNECTIONS");
    LOG_INF("Maximum concurrent open Documents limit: " << MAX_DOCUMENTS);
    LOG_INF("Maximum concurrent client Connections limit: " << MAX_CONNECTIONS);

    LOOLWSD::NumConnections = 0;

    // Command Tracing.
    if (getConfigValue<bool>(conf, "trace[@enable]", false))
    {
        const auto& path = getConfigValue<std::string>(conf, "trace.path", "");
        const auto recordOutgoing = getConfigValue<bool>(conf, "trace.outgoing.record", false);
        std::vector<std::string> filters;
        for (size_t i = 0; ; ++i)
        {
            const std::string confPath = "trace.filter.message[" + std::to_string(i) + "]";
            const auto regex = config().getString(confPath, "");
            if (!regex.empty())
            {
                filters.push_back(regex);
            }
            else if (!config().has(confPath))
            {
                break;
            }
        }

        const auto compress = getConfigValue<bool>(conf, "trace.path[@compress]", false);
        const auto takeSnapshot = getConfigValue<bool>(conf, "trace.path[@snapshot]", false);
        TraceDumper.reset(new TraceFileWriter(path, recordOutgoing, compress, takeSnapshot, filters));
        LOG_INF("Command trace dumping enabled to file: " << path);
    }

    StorageBase::initialize();

    ServerApplication::initialize(self);

#if ENABLE_DEBUG
    std::cerr << "\nLaunch this in your browser:\n\n"
              << getLaunchURI() << '\n' << std::endl;

    std::string adminURI = getAdminURI(config());
    if (!adminURI.empty())
        std::cerr << "\nOr for the Admin Console:\n\n"
                  << adminURI << '\n' << std::endl;
#endif
}

void LOOLWSD::initializeSSL()
{
    if (!LOOLWSD::isSSLEnabled())
    {
        return;
    }

    const auto ssl_cert_file_path = getPathFromConfig("ssl.cert_file_path");
    LOG_INF("SSL Cert file: " << ssl_cert_file_path);

    const auto ssl_key_file_path = getPathFromConfig("ssl.key_file_path");
    LOG_INF("SSL Key file: " << ssl_key_file_path);

    const auto ssl_ca_file_path = getPathFromConfig("ssl.ca_file_path");
    LOG_INF("SSL CA file: " << ssl_ca_file_path);

    Poco::Crypto::initializeCrypto();

    Poco::Net::initializeSSL();
    Poco::Net::Context::Params sslParams;
    sslParams.certificateFile = ssl_cert_file_path;
    sslParams.privateKeyFile = ssl_key_file_path;
    sslParams.caLocation = ssl_ca_file_path;
    // Don't ask clients for certificate
    sslParams.verificationMode = Poco::Net::Context::VERIFY_NONE;

    // FIXME: ConsoleCertificateHandler will block on stdin upon error!
    Poco::SharedPtr<Poco::Net::PrivateKeyPassphraseHandler> consoleHandler = new Poco::Net::KeyConsoleHandler(true);
    Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> invalidCertHandler = new Poco::Net::ConsoleCertificateHandler(true);

    Poco::Net::Context::Ptr sslContext = new Poco::Net::Context(Poco::Net::Context::SERVER_USE, sslParams);
    Poco::Net::SSLManager::instance().initializeServer(consoleHandler, invalidCertHandler, sslContext);

    // Init client
    Poco::Net::Context::Params sslClientParams;
    // TODO: Be more strict and setup SSL key/certs for owncloud server and us
    sslClientParams.verificationMode = Poco::Net::Context::VERIFY_NONE;

    Poco::SharedPtr<Poco::Net::PrivateKeyPassphraseHandler> consoleClientHandler = new Poco::Net::KeyConsoleHandler(false);
    Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> invalidClientCertHandler = new Poco::Net::AcceptCertificateHandler(false);

    Poco::Net::Context::Ptr sslClientContext = new Poco::Net::Context(Poco::Net::Context::CLIENT_USE, sslClientParams);
    Poco::Net::SSLManager::instance().initializeClient(consoleClientHandler, invalidClientCertHandler, sslClientContext);
}

void LOOLWSD::dumpNewSessionTrace(const std::string& id, const std::string& sessionId, const std::string& uri, const std::string& path)
{
    if (TraceDumper)
    {
        try
        {
            TraceDumper->newSession(id, sessionId, uri, path);
        }
        catch (const std::exception& exc)
        {
            LOG_WRN("Exception in tracer newSession: " << exc.what());
        }
    }
}

void LOOLWSD::dumpEndSessionTrace(const std::string& id, const std::string& sessionId, const std::string& uri)
{
    if (TraceDumper)
    {
        try
        {
            TraceDumper->endSession(id, sessionId, uri);
        }
        catch (const std::exception& exc)
        {
            LOG_WRN("Exception in tracer newSession: " << exc.what());
        }
    }
}

void LOOLWSD::dumpEventTrace(const std::string& id, const std::string& sessionId, const std::string& data)
{
    if (TraceDumper)
    {
        TraceDumper->writeEvent(id, sessionId, data);
    }
}

void LOOLWSD::dumpIncomingTrace(const std::string& id, const std::string& sessionId, const std::string& data)
{
    if (TraceDumper)
    {
        TraceDumper->writeIncoming(id, sessionId, data);
    }
}

void LOOLWSD::dumpOutgoingTrace(const std::string& id, const std::string& sessionId, const std::string& data)
{
    if (TraceDumper)
    {
        TraceDumper->writeOutgoing(id, sessionId, data);
    }
}

void LOOLWSD::defineOptions(OptionSet& optionSet)
{
    ServerApplication::defineOptions(optionSet);

    optionSet.addOption(Option("help", "", "Display help information on command line arguments.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("version", "", "Display version information.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("port", "", "Port number to listen to (default: " +
                               std::to_string(DEFAULT_CLIENT_PORT_NUMBER) + "),"
                               " must not be " + std::to_string(MasterPortNumber) + ".")
                        .required(false)
                        .repeatable(false)
                        .argument("port_number"));

    optionSet.addOption(Option("disable-ssl", "", "Disable SSL security layer.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("override", "o", "Override any setting by providing fullxmlpath=value.")
                        .required(false)
                        .repeatable(true)
                        .argument("xmlpath"));

    optionSet.addOption(Option("config-file", "", "Override configuration file path.")
                        .required(false)
                        .repeatable(false)
                        .argument("path"));

#if ENABLE_DEBUG
    optionSet.addOption(Option("unitlib", "", "Unit testing library path.")
                        .required(false)
                        .repeatable(false)
                        .argument("unitlib"));

    optionSet.addOption(Option("nocaps", "", "Use a non-privileged forkit for valgrinding.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("careerspan", "", "How many seconds to run.")
                        .required(false)
                        .repeatable(false)
                        .argument("seconds"));
#endif

#ifdef FUZZER
    optionSet.addOption(Option("dummy-lok", "", "Use empty (dummy) LibreOfficeKit implementation instead a real LibreOffice.")
                        .required(false)
                        .repeatable(false));
    optionSet.addOption(Option("fuzz", "", "Read input from the specified file for fuzzing.")
                        .required(false)
                        .repeatable(false)
                        .argument("trace_file_name"));
#endif
}

void LOOLWSD::handleOption(const std::string& optionName,
                           const std::string& value)
{
    ServerApplication::handleOption(optionName, value);

    if (optionName == "help")
    {
        displayHelp();
        std::exit(Application::EXIT_OK);
    }
    else if (optionName == "version")
        DisplayVersion = true;
    else if (optionName == "port")
        ClientPortNumber = std::stoi(value);
    else if (optionName == "disable-ssl")
        _overrideSettings["ssl.enable"] = "false";
    else if (optionName == "override")
    {
        std::string optName;
        std::string optValue;
        LOOLProtocol::parseNameValuePair(value, optName, optValue);
        _overrideSettings[optName] = optValue;
    }
    else if (optionName == "config-file")
        ConfigFile = value;
#if ENABLE_DEBUG
    else if (optionName == "unitlib")
        UnitTestLibrary = value;
#ifndef KIT_IN_PROCESS
    else if (optionName == "nocaps")
        NoCapsForKit = true;
#endif
    else if (optionName == "careerspan")
        careerSpanSeconds = std::stoi(value);

    static const char* clientPort = std::getenv("LOOL_TEST_CLIENT_PORT");
    if (clientPort)
        ClientPortNumber = std::stoi(clientPort);

    static const char* masterPort = std::getenv("LOOL_TEST_MASTER_PORT");
    if (masterPort)
        MasterPortNumber = std::stoi(masterPort);
#endif

#ifdef FUZZER
    if (optionName == "dummy-lok")
        DummyLOK = true;
    else if (optionName == "fuzz")
        FuzzFileName = value;
#endif
}

void LOOLWSD::displayHelp()
{
    HelpFormatter helpFormatter(options());
    helpFormatter.setCommand(commandName());
    helpFormatter.setUsage("OPTIONS");
    helpFormatter.setHeader("LibreOffice Online WebSocket server.");
    helpFormatter.format(std::cout);
}

bool LOOLWSD::checkAndRestoreForKit()
{
#ifdef KIT_IN_PROCESS
    return false;
#else
    int status;
    const pid_t pid = waitpid(ForKitProcId, &status, WUNTRACED | WNOHANG);
    if (pid > 0)
    {
        if (pid == ForKitProcId)
        {
            if (WIFEXITED(status) || WIFSIGNALED(status))
            {
                if (WIFEXITED(status))
                {
                    LOG_INF("Forkit process [" << pid << "] exited with code: " <<
                            WEXITSTATUS(status) << ".");
                }
                else
                {
                    LOG_ERR("Forkit process [" << pid << "] " <<
                            (WCOREDUMP(status) ? "core-dumped" : "died") <<
                            " with " << SigUtil::signalName(WTERMSIG(status)));
                }

                // Spawn a new forkit and try to dust it off and resume.
                if (!createForKit())
                {
                    LOG_FTL("Failed to spawn forkit instance. Shutting down.");
                    SigUtil::requestShutdown();
                }
            }
            else if (WIFSTOPPED(status))
            {
                LOG_INF("Forkit process [" << pid << "] stopped with " <<
                        SigUtil::signalName(WSTOPSIG(status)));
            }
            else if (WIFCONTINUED(status))
            {
                LOG_INF("Forkit process [" << pid << "] resumed with SIGCONT.");
            }
            else
            {
                LOG_WRN("Unknown status returned by waitpid: " << std::hex << status << ".");
            }

            return true;
        }
        else
        {
            LOG_ERR("An unknown child process [" << pid << "] died.");
        }
    }
    else if (pid < 0)
    {
        LOG_SYS("Forkit waitpid failed.");
        if (errno == ECHILD)
        {
            // No child processes.
            // Spawn a new forkit and try to dust it off and resume.
            if (!createForKit())
            {
                LOG_FTL("Failed to spawn forkit instance. Shutting down.");
                SigUtil::requestShutdown();
            }
        }

        return true;
    }

    return false;
#endif
}

bool LOOLWSD::createForKit()
{
#ifdef KIT_IN_PROCESS
    return true;
#else
    LOG_INF("Creating new forkit process.");

    Process::Args args;
    args.push_back("--losubpath=" + std::string(LO_JAIL_SUBPATH));
    args.push_back("--systemplate=" + SysTemplate);
    args.push_back("--lotemplate=" + LoTemplate);
    args.push_back("--childroot=" + ChildRoot);
    args.push_back("--clientport=" + std::to_string(ClientPortNumber));
    args.push_back("--masterport=" + std::to_string(MasterPortNumber));
    if (UnitWSD::get().hasKitHooks())
    {
        args.push_back("--unitlib=" + UnitTestLibrary);
    }

    if (DisplayVersion)
    {
        args.push_back("--version");
    }

    std::string forKitPath = Path(Application::instance().commandPath()).parent().toString() + "loolforkit";
    if (NoCapsForKit)
    {
        forKitPath = forKitPath + std::string("-nocaps");
        args.push_back("--nocaps");
    }

    // If we're recovering forkit, don't allow processing new requests.
    std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
    std::unique_lock<std::mutex> newChildrenLock(NewChildrenMutex);

    // Always reap first, in case we haven't done so yet.
    int status;
    waitpid(ForKitProcId, &status, WUNTRACED | WNOHANG);
    ForKitProcId = -1;
    Admin::instance().setForKitPid(ForKitProcId);

    const int oldForKitWritePipe = ForKitWritePipe;
    ForKitWritePipe = -1;
    close(oldForKitWritePipe);

    // ForKit always spawns one.
    ++OutstandingForks;

    LOG_INF("Launching forkit process: " << forKitPath << ' ' <<
            Poco::cat(std::string(" "), args.begin(), args.end()));

    LastForkRequestTime = std::chrono::steady_clock::now();
    Pipe inPipe;
    ProcessHandle child = Process::launch(forKitPath, args, &inPipe, nullptr, nullptr);

    // The Pipe dtor closes the fd, so dup it.
    ForKitWritePipe = dup(inPipe.writeHandle());

    ForKitProcId = child.id();

    LOG_INF("Forkit process launched: " << ForKitProcId);

    // Init the Admin manager
    Admin::instance().setForKitPid(ForKitProcId);

    // Spawn some children, if necessary.
    preForkChildren(newChildrenLock);

    return (ForKitProcId != -1);
#endif
}

#ifdef FUZZER
std::mutex Connection::Mutex;
#endif

// TODO loolnb FIXME
static const std::string HARDCODED_PATH("file:///tmp/hello-world.odt");

class PlainSocketFactory : public SocketFactory
{
    std::shared_ptr<Socket> create(const int fd) override
    {
        // TODO FIXME loolnb - avoid the copy/paste between PlainSocketFactory
        // and SslSocketFactory
        // Request a kit process for this doc.
        std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
        auto child = getNewChild();
        if (!child)
        {
            // Let the client know we can't serve now.
            throw std::runtime_error("Failed to spawn lokit child.");
        }

        Poco::URI uri(HARDCODED_PATH);
        std::shared_ptr<DocumentBroker> docBroker = std::make_shared<DocumentBroker>(HARDCODED_PATH, uri, HARDCODED_PATH, LOOLWSD::ChildRoot, child);
        return std::make_shared<StreamSocket>(fd, new ClientSession("hardcoded", docBroker, uri));
    }
};

class SslSocketFactory : public SocketFactory
{
    std::shared_ptr<Socket> create(const int fd) override
    {
        // TODO FIXME loolnb - avoid the copy/paste between PlainSocketFactory
        // and SslSocketFactory
        // Request a kit process for this doc.
        std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
        auto child = getNewChild();
        if (!child)
        {
            // Let the client know we can't serve now.
            throw std::runtime_error("Failed to spawn lokit child.");
        }

        Poco::URI uri(HARDCODED_PATH);
        std::shared_ptr<DocumentBroker> docBroker = std::make_shared<DocumentBroker>(HARDCODED_PATH, uri, HARDCODED_PATH, LOOLWSD::ChildRoot, child);
        return std::make_shared<StreamSocket>(fd, new ClientSession("hardcoded", docBroker, uri));
    }
};

/// The main server thread.
///
/// Waits for the connections from the loleaflets, and creates the
/// websockethandlers accordingly.
class LOOLWSDServer
{
    LOOLWSDServer(LOOLWSDServer&& other) = delete;
    const LOOLWSDServer& operator=(LOOLWSDServer&& other) = delete;

public:
    LOOLWSDServer()
        : _stop(false)
    {
    }

    ~LOOLWSDServer()
    {
        stop();
        if (_serverThread.joinable())
            _serverThread.join();
    }

    void start(const Poco::Net::SocketAddress& addr)
    {
        std::shared_ptr<ServerSocket> serverSocket = std::make_shared<ServerSocket>(_documentPoll,
                LOOLWSD::isSSLEnabled()? std::unique_ptr<SocketFactory>{new SslSocketFactory()}:
                                         std::unique_ptr<SocketFactory>{new PlainSocketFactory()});

        if (!serverSocket->bind(addr))
        {
            const std::string msg = "Failed to bind. (errno: ";
            throw std::runtime_error(msg + std::strerror(errno) + ")");
        }

        if (!serverSocket->listen())
        {
            const std::string msg = "Failed to listen. (errno: ";
            throw std::runtime_error(msg + std::strerror(errno) + ")");
        }

        _serverPoll.insertNewSocket(serverSocket);

        _serverThread = std::thread(runServer, std::ref(_stop), std::ref(_serverPoll));

        // TODO loolnb - we need a documentThread per document
        _documentThread = std::thread(runDocument, std::ref(_stop), std::ref(_documentPoll));
    }

    void stop()
    {
        _stop = true;
    }

private:
    std::atomic<bool> _stop;

    SocketPoll _serverPoll;
    std::thread _serverThread;

    // TODO loolnb - we need a documentThread per document
    SocketPoll _documentPoll;
    std::thread _documentThread;

    static void runServer(std::atomic<bool>& stop, SocketPoll& serverPoll) {
        LOG_INF("Starting master server thread.");
        while (!stop && !TerminationFlag && !SigUtil::isShuttingDown())
        {
            serverPoll.poll(30000);
        }
    }

    static void runDocument(std::atomic<bool>& stop, SocketPoll& documentPoll) {
        LOG_INF("Starting document thread.");
        while (!stop && !TerminationFlag && !SigUtil::isShuttingDown())
        {
            documentPoll.poll(5000);
        }
    }
};

int LOOLWSD::main(const std::vector<std::string>& /*args*/)
{
#ifndef FUZZER
    SigUtil::setFatalSignals();
    SigUtil::setTerminationSignals();
#endif

    // down-pay all the forkit linking cost once & early.
    Environment::set("LD_BIND_NOW", "1");

    if (DisplayVersion)
    {
        std::string version, hash;
        Util::getVersionInfo(version, hash);
        LOG_INF("Loolwsd version details: " << version << " - " << hash);
    }

    initializeSSL();

    char* locale = setlocale(LC_ALL, nullptr);
    if (locale == nullptr || std::strcmp(locale, "C") == 0)
    {
        setlocale(LC_ALL, "en_US.utf8");
    }

    if (access(Cache.c_str(), R_OK | W_OK | X_OK) != 0)
    {
        LOG_SFL("Unable to access cache [" << Cache <<
                "] please make sure it exists, and has write permission for this user.");
        return Application::EXIT_SOFTWARE;
    }

    // We use the same option set for both parent and child loolwsd,
    // so must check options required in the parent (but not in the
    // child) separately now. Also check for options that are
    // meaningless for the parent.
    if (SysTemplate.empty())
    {
        LOG_FTL("Missing --systemplate option");
        throw MissingOptionException("systemplate");
    }
    if (LoTemplate.empty())
    {
        LOG_FTL("Missing --lotemplate option");
        throw MissingOptionException("lotemplate");
    }
    if (ChildRoot.empty())
    {
        LOG_FTL("Missing --childroot option");
        throw MissingOptionException("childroot");
    }
    else if (ChildRoot[ChildRoot.size() - 1] != '/')
        ChildRoot += '/';

    FileUtil::registerFileSystemForDiskSpaceChecks(ChildRoot);
    FileUtil::registerFileSystemForDiskSpaceChecks(Cache + "/.");

    if (FileServerRoot.empty())
        FileServerRoot = Poco::Path(Application::instance().commandPath()).parent().toString();
    FileServerRoot = Poco::Path(FileServerRoot).absolute().toString();
    LOG_DBG("FileServerRoot: " << FileServerRoot);

    if (ClientPortNumber == MasterPortNumber)
        throw IncompatibleOptionsException("port");

    // Configure the Server.
    // Note: TCPServer internally uses a ThreadPool to dispatch connections
    // (the default if not given). The capacity of the ThreadPool is increased
    // here in proportion to MAX_CONNECTIONS. Each client requests ~10
    // resources (.js, .css, etc) beyond the main one, which are transient.
    // The pool must have sufficient available threads to dispatch a new
    // connection, otherwise will deadlock. So we need to have sufficient
    // threads to serve new clients while those transients are served.
    // We provision up to half the limit to connect simultaneously
    // without loss of performance. This cap is to avoid flooding the server.
    static_assert(MAX_CONNECTIONS >= 3, "MAX_CONNECTIONS must be at least 3");
    const auto maxThreadCount = MAX_CONNECTIONS * 5;

#if 0 // loolnb
    auto params1 = new HTTPServerParams();
    params1->setMaxThreads(maxThreadCount);
#endif
    auto params2 = new HTTPServerParams();
    params2->setMaxThreads(maxThreadCount);

    // Twice as many min and max since we share this pool
    // between both internal and external connections.
    const auto minThreadCount = std::max<int>(NumPreSpawnedChildren * 3, 3);
    const auto idleTimeSeconds = 90;
    const auto stackSizeBytes = 256 * 1024;
    ThreadPool threadPool(minThreadCount * 2,
                          maxThreadCount * 2,
                          idleTimeSeconds,
                          stackSizeBytes);

    // Start internal server for child processes.
    SocketAddress addr2("127.0.0.1", MasterPortNumber);
    std::unique_ptr<Poco::Net::ServerSocket> psvs2(
        UnitWSD::isUnitTesting() ?
            findFreeMasterPort(MasterPortNumber) :
            getMasterSocket(MasterPortNumber));
    if (!psvs2)
    {
        LOG_FTL("Failed to listen on master port (" <<
                MasterPortNumber << ") or find a free port. Exiting.");
        return Application::EXIT_SOFTWARE;
    }

    HTTPServer srv2(new PrisonerRequestHandlerFactory(), threadPool, *psvs2, params2);
    LOG_INF("Starting prisoner server listening on " << MasterPortNumber);
    srv2.start();

    // Fire the ForKit process; we are ready to get child connections.
    if (!createForKit())
    {
        LOG_FTL("Failed to spawn loolforkit.");
        return Application::EXIT_SOFTWARE;
    }

#if 0 // loolnb
    // Now we can serve clients; Start listening on the public port.
    std::unique_ptr<Poco::Net::ServerSocket> psvs(
        UnitWSD::isUnitTesting() ?
            findFreeServerPort(ClientPortNumber) :
            getServerSocket(ClientPortNumber, true));
    if (!psvs)
    {
        LOG_FTL("Failed to listen on client port (" <<
                ClientPortNumber << ") or find a free port. Exiting.");
        return Application::EXIT_SOFTWARE;
    }

    HTTPServer srv(new ClientRequestHandlerFactory(), threadPool, *psvs, params1);
    LOG_INF("Starting master server listening on " << ClientPortNumber);
    srv.start();
#endif

    LOOLWSDServer srv;
    // TODO loolnb
    SocketAddress addr("127.0.0.1", ClientPortNumber);
    srv.start(addr);

#if ENABLE_DEBUG
    time_t startTimeSpan = time(nullptr);
#endif

    auto last30SecCheckTime = std::chrono::steady_clock::now();
    while (!TerminationFlag && !SigUtil::isShuttingDown())
    {
        UnitWSD::get().invokeTest();
        if (TerminationFlag || SigUtil::handleShutdownRequest())
        {
            break;
        }

        if (!checkAndRestoreForKit())
        {
            // No children have died.
            // Make sure we have sufficient reserves.
            if (prespawnChildren())
            {
                // Nothing more to do this round, unless we are fuzzing
#if FUZZER
                if (!FuzzFileName.empty())
                {
                    std::unique_ptr<Replay> replay(new Replay(
#if ENABLE_SSL
                            "https://127.0.0.1:" + std::to_string(ClientPortNumber),
#else
                            "http://127.0.0.1:" + std::to_string(ClientPortNumber),
#endif
                            FuzzFileName));

                    std::unique_ptr<Thread> replayThread(new Thread());
                    replayThread->start(*replay);

                    // block until the replay finishes
                    replayThread->join();

                    TerminationFlag = true;
                }
#endif
            }
            else if (!std::getenv("LOOL_NO_AUTOSAVE") &&
                     std::chrono::duration_cast<std::chrono::seconds>
                        (std::chrono::steady_clock::now() - last30SecCheckTime).count() >= 30)
            {
                try
                {
#if 0
                    std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
                    cleanupDocBrokers();
                    for (auto& pair : DocBrokers)
                    {
                        auto docLock = pair.second->getDeferredLock();
                        if (doclock.try_lock())
                        {
                            pair.second->autosave(false, 0, doclock);
                        }
                    }
#endif
                }
                catch (const std::exception& exc)
                {
                    LOG_ERR("Exception: " << exc.what());
                }

                last30SecCheckTime = std::chrono::steady_clock::now();
            }
            else
            {
                // Wait if we had done no work.
                std::this_thread::sleep_for(std::chrono::milliseconds(CHILD_REBALANCE_INTERVAL_MS));
            }
        }

#if ENABLE_DEBUG
        if (careerSpanSeconds > 0 && time(nullptr) > startTimeSpan + careerSpanSeconds)
        {
            LOG_INF((time(nullptr) - startTimeSpan) << " seconds gone, finishing as requested.");
            break;
        }
#endif
    }

    // Stop the listening to new connections
    // and wait until sockets close.
    LOG_INF("Stopping server socket listening. ShutdownFlag: " <<
            SigUtil::isShuttingDown() << ", TerminationFlag: " << TerminationFlag);

    // Wait until documents are saved and sessions closed.
    srv.stop();
    srv2.stop();
    threadPool.joinAll();

    // atexit handlers tend to free Admin before Documents
    LOG_INF("Cleaning up lingering documents.");
    DocBrokers.clear();

#ifndef KIT_IN_PROCESS
    // Terminate child processes
    LOG_INF("Requesting forkit process " << ForKitProcId << " to terminate.");
    SigUtil::killChild(ForKitProcId);
#endif

    // Terminate child processes
    LOG_INF("Requesting child processes to terminate.");
    for (auto& child : NewChildren)
    {
        child->close(true);
    }

#ifndef KIT_IN_PROCESS
    // Wait for forkit process finish.
    int status = 0;
    waitpid(ForKitProcId, &status, WUNTRACED);
    close(ForKitWritePipe);
#endif

    // In case forkit didn't cleanup properly, don't leave jails behind.
    LOG_INF("Cleaning up childroot directory [" << ChildRoot << "].");
    std::vector<std::string> jails;
    File(ChildRoot).list(jails);
    for (auto& jail : jails)
    {
        const auto path = ChildRoot + jail;
        LOG_INF("Removing jail [" << path << "].");
        FileUtil::removeFile(path, true);
    }

    if (SigUtil::isShuttingDown())
    {
        // At this point there should be no other thread, but...
        std::lock_guard<std::mutex> lock(ClientWebSocketsMutex);

        LOG_INF("Notifying clients that we are recycling.");
        static const std::string msg("close: recycling");
        for (auto& ws : ClientWebSockets)
        {
            try
            {
                ws->sendFrame(msg.data(), msg.size());
                ws->shutdown(WebSocket::WS_ENDPOINT_GOING_AWAY);
            }
            catch (const std::exception& ex)
            {
                LOG_ERR("Error while notifying client of recycle: " << ex.what());
            }
        }
    }

    // Finally, we no longer need SSL.
    if (LOOLWSD::isSSLEnabled())
    {
        Poco::Net::uninitializeSSL();
        Poco::Crypto::uninitializeCrypto();
    }

    int returnValue = Application::EXIT_OK;
    UnitWSD::get().returnValue(returnValue);

    LOG_INF("Process [loolwsd] finished.");
    return returnValue;
}

void UnitWSD::testHandleRequest(TestRequest type, UnitHTTPServerRequest& request, UnitHTTPServerResponse& response)
{
    switch (type)
    {
    case TestRequest::Client:
#if 0 // loolnb
        ClientRequestHandler::handleClientRequest(request, response, LOOLWSD::GenSessionId());
#endif
        break;
    case TestRequest::Prisoner:
        PrisonerRequestHandler::handlePrisonerRequest(request, response);
        break;
    default:
        assert(false);
        break;
    }
}

#if !defined(BUILDING_TESTS) && !defined(KIT_IN_PROCESS)
namespace Util
{

void alertAllUsers(const std::string& cmd, const std::string& kind)
{
    alertAllUsers("error: cmd=" + cmd + " kind=" + kind);
}

void alertAllUsers(const std::string& msg)
{
    std::lock_guard<std::mutex> docBrokersLock(DocBrokersMutex);

    alertAllUsersInternal(msg);
}

}
#endif

POCO_SERVER_MAIN(LOOLWSD)

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
