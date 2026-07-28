#pragma once

#include "HappyConnOpener.h"
#include "http.h"

using namespace std;

/// @brief Class for send all selected peers Asynchronously
class Salsa2Dispatcher: public AsyncJob, public IDispatcher
{
    CBDATA_CHILD(Salsa2Dispatcher);

    private:

        /// @brief Holds the state for a single connection attempt to a peer.
        struct Attempt
        {
            public:
                Comm::ConnectionPointer conn; ///< The connection to the peer.                
                HappyConnOpener::Pointer opener; ///< The HappyConnOpener instance for this attempt.
                HttpStateData *requester; ///< JobWait for the HTTP request.

                /// @brief Constructs an Attempt object.
                /// @param connection The connection to the peer.
                /// @param connOpener The HappyConnOpener instance for this attempt.
                /// @param openerCallback Callback for the connection opener.
                Attempt(
                    Comm::ConnectionPointer connection,
                    HappyConnOpener *connOpener
                ):
                    conn(connection), 
                    opener(connOpener),                    
                    requester(nullptr)
                    {}
                
                /// @brief Destructor for the Attempt object, cleans up resources.
                ~Attempt()
                {
                    if (requester)
                    {                        
                        requester->callEnd();                            
                        delete requester;                
                    }
                }
        };

        ///< A vector of Attempt structs, one for each peer connection attempt.
        std::vector<Attempt> attempts; 
        const ResolvedPeers::Pointer &destinations;
        FwdState &fwd; ///< A reference to the FwdState object.
        Comm::ConnectionPointer clientConn; ///< The connection to the client.
        bool isSuccess = false;
        
        /// @brief Constructs a Salsa2Dispatcher object.
        /// @param dests The destination peers.
        /// @param p_request The HTTP request to be dispatched.
        /// @param aFwdStart The start time of the forwarding process.
        /// @param tries The number of tries for this connection.
        /// @param anAle The access log entry.
        /// @param retriable Whether the request is retriable.
        /// @param persistent Whether to use a persistent connection.
        /// @param forwarder A reference to the FwdState object.
        /// @param clientConnection The connection to the client.
        /// @param e The store entry associated with the request.
        Salsa2Dispatcher(
            const ResolvedPeers::Pointer &dests, 
            time_t aFwdStart,
            int tries,
            bool retriable,
            bool persistent,
            FwdState &forwarder,
            Comm::ConnectionPointer clientConnection
        );
        
        /// @brief Creates a new HappyConnOpener for a destination.
        /// @param dest The destination peer.
        /// @param callback The callback to be called when the connection is established.
        /// @param aFwdStart The start time of the forwarding process.
        /// @param tries The number of tries for this connection.
        /// @param anAle The access log entry.
        /// @param retriable Whether the request is retriable.
        /// @param persistent Whether to use a persistent connection.
        /// @return A new HappyConnOpener instance.
        HappyConnOpener *newOpener(
            const ResolvedPeers::Pointer &dest,
            AsyncCallback<HappyConnOpenerAnswer> callback,
            time_t aFwdStart,
            int tries,
            const AccessLogEntryPointer &anAle,
            bool retriable,
            bool persistent
        );

        /// @brief Notifies other components about the connection.
        /// @param conn The connection to notify about.
        void notifyComponents(const Comm::ConnectionPointer conn);

        /// @brief Sets Quality of Service parameters for the connection.
        /// @param conn The connection to set QoS parameters for.
        void setQosParams(const Comm::ConnectionPointer conn);

        /// @brief Prepares for dispatching the request after a connection is established.
        /// @param answer The answer from the HappyConnOpener.
        /// @return The attempt that connected
        Attempt *prepareDispatching(HappyConnOpener::Answer &answer);
        
        // AsyncCall overriding
        void start() override;

        /// @brief Called by HappyConnOpener when a connection is established or fails.
        /// @param answer The answer from the HappyConnOpener.
        void noteConnection(HappyConnOpener::Answer &answer);

        /// @brief Gets the Attempt object for a given connection.
        /// @param conn The connection to get the Attempt for.
        /// @return The Attempt object, or nullptr if not found.
        Attempt *getAttempt(const Comm::ConnectionPointer conn);

        /// @brief Dispatches the request on the given connection.
        /// @param conn The connection to dispatch the request on.
        void dispatch(Attempt &attempt);

        virtual bool doneAll() const {return false;}

    public:
        ~Salsa2Dispatcher()
        {
            debugs(96,3, "reache here");
        }
        
        /// @brief Constructs a Salsa2Dispatcher object.
        /// @param dests The destination peers.
        /// @param p_request The HTTP request to be dispatched.
        /// @param aFwdStart The start time of the forwarding process.
        /// @param tries The number of tries for this connection.
        /// @param anAle The access log entry.
        /// @param retriable Whether the request is retriable.
        /// @param persistent Whether to use a persistent connection.
        /// @param forwarder A reference to the FwdState object.
        /// @param clientConnection The connection to the client.
        /// @param e The store entry associated with the request.
        static void Salsa2DispatcherStart(
            const ResolvedPeers::Pointer &dests, 
            time_t aFwdStart,
            int tries,
            bool retriable,
            bool persistent,
            FwdState &forwarder,
            Comm::ConnectionPointer clientConnection
        )
        {
            AsyncJob::Start(new Salsa2Dispatcher(
                dests, 
                aFwdStart, 
                tries, 
                retriable,
                persistent,
                forwarder,
                clientConnection
            ));
        }

        // IDispatcher implementation

        void handleUnregisteredServerEnd() override;
        void markStoredReplyAsWhole(const char *whyWeAreSure) override;
        void dontRetry(bool val) override;
        void fail(ErrorState *errorState) override;
        void unregister(Comm::ConnectionPointer &conn) override;
        void complete(const Comm::ConnectionPointer conn = nullptr) override;
};