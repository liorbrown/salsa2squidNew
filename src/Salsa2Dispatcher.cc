// Project-wide macros/types first
#include "squid.h"

// Ensure types used by HappyConnOpener are visible
#include "base/JobWait.h"
#include "base/AsyncCallbacks.h"
#include "error/forward.h"
#include "Salsa2Dispatcher.h"

Salsa2Dispatcher::Salsa2Dispatcher(
    const vector<PeerConnectionPointer> &paths, 
    const AsyncCallback<Answer> &callback, 
    const HttpRequestPointer &request,
    time_t aFwdStart,
    int tries,
    const AccessLogEntryPointer &anAle):
    HappyConnOpener(nullptr, callback, request, aFwdStart, tries, anAle)
    {        
        for (PeerConnectionPointer path : paths)
        {
            this->peers[path->getPeer()] = new Attempt<Salsa2Dispatcher>(
                 &Salsa2Dispatcher::noteConnectDone, "Salsa2Dispatcher::noteConnectDone");
            
            this->peers[path->getPeer()]->path = path;
        }
    }

Salsa2Dispatcher::~Salsa2Dispatcher()
{
    for(auto &peer : this->peers) delete(peer.second);
}

void Salsa2Dispatcher::start()
{
    for(auto &peer : this->peers)
    {
        Attempt<Salsa2Dispatcher> *attempt = peer.second;
        PeerConnectionPointer path = attempt->path;
        // Need to set it to null, since startConnecting check must(!attempt->path) 
        attempt->path = nullptr;

        this->startConnecting<Salsa2Dispatcher>(*attempt, path);
    }
}

void Salsa2Dispatcher::noteConnectDone(const CommConnectCbParams &params)
{
    auto attempt = this->peers.find(params.conn->getPeer());
    assert(attempt != this->peers.end());

    handleConnOpenerAnswer(*attempt->second, params, "new peer connection");
}