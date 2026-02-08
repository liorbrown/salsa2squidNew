#pragma once

#include "HappyConnOpener.h"

using namespace std;

class Salsa2Dispatcher: public HappyConnOpener
{
    private:
        //vector<unique_ptr<HappyConnOpener::Attempt<Salsa2Dispatcher>>> peers;
        vector<HappyConnOpener::Attempt<Salsa2Dispatcher>*> peers;

        void noteConnectDone(const CommConnectCbParams &params);

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