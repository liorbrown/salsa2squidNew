#pragma once

#include "HappyConnOpener.h"

using namespace std;

/// @brief Class for send all selected peers Asynchronously
class Salsa2Dispatcher: public HappyConnOpener
{
    private:
        // Holds all peers and their attempts
        map<CachePeer*, HappyConnOpener::Attempt<Salsa2Dispatcher>*> peers;

        // Called when new connection created successfully
        void noteConnectDone(const CommConnectCbParams &params);

        // Starting connections opening
        void start() override;

    public:
        Salsa2Dispatcher(
            const vector<PeerConnectionPointer> &paths, 
            const AsyncCallback<Answer> &callback, 
            const HttpRequestPointer &request,
            time_t aFwdStart,
            int tries,
            const AccessLogEntryPointer &anAle);
        
        ~Salsa2Dispatcher();
};