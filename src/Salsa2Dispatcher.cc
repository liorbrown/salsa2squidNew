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
        peers.reserve(paths.size());
        
        for (PeerConnectionPointer path : paths)
        {
            Attempt<Salsa2Dispatcher> *attempt = new Attempt<Salsa2Dispatcher>(
                &Salsa2Dispatcher::noteConnectDone, "Salsa2Dispatcher::noteConnectDone");
            attempt->path = path;

            peers.emplace_back(attempt);
        }
    }

Salsa2Dispatcher::~Salsa2Dispatcher()
{
    for(auto peer : this->peers) delete(peer);
}

void Salsa2Dispatcher::start()
{
    for(auto peer : this->peers)
        this->startConnecting<Salsa2Dispatcher>(*peer, peer->path);
}

void Salsa2Dispatcher::noteConnectDone(const CommConnectCbParams &params)
{
    (void)params;
}

