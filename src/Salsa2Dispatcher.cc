#include "squid.h"
#include "base/JobWait.h"
#include "base/AsyncCallbacks.h"
#include "error/forward.h"
#include "Salsa2Dispatcher.h"
#include "fd.h"
#include "ip/QosConfig.h"
#include "Store.h"

CBDATA_CLASS_INIT(Salsa2Dispatcher);

HappyConnOpener *Salsa2Dispatcher::newOpener(
    const ResolvedPeers::Pointer &dest,  
    AsyncCallback<HappyConnOpenerAnswer> callback,
    time_t aFwdStart,
    int tries,
    const AccessLogEntryPointer &anAle,
    bool retriable,
    bool persistent
)
{
    HappyConnOpener *opener = new HappyConnOpener(
            dest, callback, this->request, aFwdStart, tries, anAle);

    opener->setHost(this->request->url.host());
    opener->setRetriable(retriable);
    opener->allowPersistent(persistent);
    dest->notificationPending = true;

    return opener;
}

Salsa2Dispatcher::Salsa2Dispatcher(
    const ResolvedPeers::Pointer &dests,
    time_t aFwdStart,
    int tries,
    bool retriable,
    bool persistent,
    FwdState &forwarder,
    Comm::ConnectionPointer clientConnection
    ):
        AsyncJob("Salsa2Dispacher"),
        IDispatcher(forwarder.request, forwarder.entry, forwarder.al),
        destinations(dests),
        fwd(forwarder),
        clientConn(clientConnection)
{
    // Runs on all paths
    for(ResolvedPeerPath &path : dests->paths_)
    {
        AsyncCallback OpenerCallback = 
            asyncCallback(96, 5, Salsa2Dispatcher::noteConnection, this);

        Comm::ConnectionPointer conn = path.connection;

        // Create new ResolvedPeers with only one path
        ResolvedPeers::Pointer dest = new ResolvedPeers();
        dest->addPath(conn);

        // Creates new HappyConnOpener with single path
        HappyConnOpener *opener = this->newOpener(
            dest,
            OpenerCallback,
            aFwdStart,
            tries,
            this->al,
            retriable,
            persistent);

        this->attempts.emplace_back(conn, opener);
    }
}

void Salsa2Dispatcher::start()
{
    debugs(96, 3, "Starting");

    this->entry->peerWriting = nullptr;

    for (Attempt &attempt : this->attempts)
        AsyncJob::Start(attempt.opener);
}

Salsa2Dispatcher::Attempt *Salsa2Dispatcher::getAttempt
    (const Comm::ConnectionPointer conn)
{
    auto it = std::find_if(
        this->attempts.begin(),
        this->attempts.end(),
        [conn](const Attempt& attempt)
        {return (attempt.conn->getPeer() == conn->getPeer());}
    );

    return (it != this->attempts.end()) ? it.operator->() : nullptr;
}

void Salsa2Dispatcher::notifyComponents(const Comm::ConnectionPointer conn)
{
    fd_note(conn->fd, this->request->url.absolute().c_str());
    fd_table[conn->fd].noteUse();

    CachePeer *const peer = conn->getPeer();
    ++peer->stats.fetches;
}

void Salsa2Dispatcher::setQosParams(const Comm::ConnectionPointer conn)
{
    /* Retrieves remote server TOS or MARK value, and stores it as part of the
     * original client request FD object. It is later used to forward
     * remote server's TOS/MARK in the response to the client in case of a MISS.
     */
    if (Ip::Qos::TheConfig.isHitNfmarkActive()) {
        if (Comm::IsConnOpen(this->clientConn) && Comm::IsConnOpen(conn)) {
            fde * clientFde = &fd_table[this->clientConn->fd]; // XXX: move the fd_table access into Ip::Qos
            /* Get the netfilter CONNMARK */
            clientFde->nfConnmarkFromServer = Ip::Qos::getNfConnmark(conn, Ip::Qos::dirOpened);
        }
    }

#if _SQUID_LINUX_
    if (Ip::Qos::TheConfig.isHitTosActive()) 
    {
        if (Comm::IsConnOpen(this->clientConn)) 
        {
            fde * clientFde = &fd_table[this->clientConn->fd];
            /* Get the TOS value for the packet */
            Ip::Qos::getTosFromServer(conn, clientFde);
        }
    }
#endif
}

Salsa2Dispatcher::Attempt *Salsa2Dispatcher::prepareDispatching(
    HappyConnOpener::Answer &answer)
{
    PeerConnectionPointer conn = answer.conn;
    if (!conn)
    {
        debugs(96, 3, "Salsa2: Salsa2Dispatcher::noteConnection got empty conn");

        return nullptr;
    }
    
    // Update access log file
    this->fwd.syncHierNote(answer.conn, request->url.host());

    Attempt *attempt = this->getAttempt(conn);

    if (!attempt)
    {
        debugs(96, 3, " conn not in list");

        return nullptr;
    }

    ErrorState *error = answer.error.get();
    if (error)
    {
        debugs(96, 3, err_type_str[error->type] << 
            " \"" << Http::StatusCodeString(error->httpStatus) << "\"\n\t" 
            << this->request->url
            << "\n\t From " << *conn);
        
        return nullptr;
    }
    else if (!Comm::IsConnOpen(conn) || fd_table[conn->fd].closing())
    {        
        this->fwd.closePendingConnection(
            conn, "conn was closed while waiting for noteConnection");
        
        return nullptr;
    }

    attempt->conn = conn;
    return attempt;
}

void Salsa2Dispatcher::dispatch(Salsa2Dispatcher::Attempt &attempt)
{
    debugs(96, 5, 
        "Salsa2: Fetching " << 
        request->method << ' ' << request->url
        << " From " << *attempt.conn);
       
    this->notifyComponents(attempt.conn);
    this->setQosParams(attempt.conn);

    attempt.requester = new HttpStateData(this, attempt.conn);
    AsyncJob::Start(attempt.requester);
}

void Salsa2Dispatcher::noteConnection(HappyConnOpener::Answer &answer)
{
    // Not need to handle all the TLS shit like FwdState does 
    // since salsa2 designs to work only with http requests
    Salsa2Dispatcher::Attempt *attempt = this->prepareDispatching(answer);

    if (attempt) dispatch(*attempt);
}

/////////// IDispatcher implementation ///////////////////

void Salsa2Dispatcher::handleUnregisteredServerEnd()
{ 
    debugs(96, 5, "Salsa2: handleUnregisteredServerEnd"); 
}

void Salsa2Dispatcher::markStoredReplyAsWhole(const char *whyWeAreSure) 
{
    // Gets status code
    const Http::StatusCode status = this->entry->mem().baseReply().sline.status();

    // Check that status OK and entry not aborted
    this->isSuccess = !EBIT_TEST(entry->flags, ENTRY_ABORTED) && status < 400;

    // Forward to FwdState so storedWholeReply_ gets set, preventing completeTruncated()
    if (this->isSuccess)
        this->fwd.markStoredReplyAsWhole(whyWeAreSure);

    debugs(96, 5, "Salsa2: markStoredReplyAsWhole - reason: " << whyWeAreSure);
}

void Salsa2Dispatcher::dontRetry(bool val)
{
    debugs(96, 5, "Salsa2: dontRetry - " << val);
}

void Salsa2Dispatcher::fail(ErrorState *errorState) 
{
    if (this->entry && errorState)
        debugs(96, 5,
            err_type_str[errorState->type] << " \"" 
            << Http::StatusCodeString(errorState->httpStatus) << "\"\n\t" 
            << this->entry->url());
}

void Salsa2Dispatcher::unregister(Comm::ConnectionPointer &conn) 
{
    if (conn)
        debugs(96, 5, "Salsa2: unregister - " << *conn);
}

void Salsa2Dispatcher::complete(const Comm::ConnectionPointer conn)
{
    (void)conn;
    if (this->isSuccess)    
        this->fwd.complete();
}