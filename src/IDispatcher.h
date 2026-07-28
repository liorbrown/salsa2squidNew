#pragma once

#include "HttpRequest.h"

/// @brief An interface for dispatching a request to a server.
/// It encapsulates the state and actions required to forward a request,
/// handle the response, and manage the connection to the server.
class IDispatcher
{
    public:
        HttpRequest *request;
        StoreEntry *entry;
        AccessLogEntryPointer al; ///< info for the future access.log entry

        /// @brief Constructs an IDispatcher object.
        /// @param r The HTTP request to be dispatched.
        /// @param e The store entry associated with the request.
        /// @param alp Information for the access log entry.
        IDispatcher(HttpRequest *r, StoreEntry *e, AccessLogEntryPointer alp):
            request(r), entry(e), al(alp) {}
        
        // Pure virtual interface methods

        /// @brief Handles the unexpected closure of the server connection.
        virtual void handleUnregisteredServerEnd() = 0;

        /// @brief Marks the stored reply as complete.
        /// This is called when the entire response has been received and stored.
        /// @param whyWeAreSure A debug string explaining why we are sure the reply is whole.
        virtual void markStoredReplyAsWhole(const char *whyWeAreSure) = 0;

        /// @brief Called when the request dispatching process is complete.
        virtual void complete(const Comm::ConnectionPointer conn = nullptr) = 0;

        /// @brief Sets a flag to prevent retrying the request.
        /// @param val If true, the request will not be retried on failure.
        virtual void dontRetry(bool val) = 0;

        /// @brief Fails the request dispatching process with an error.
        /// @param err The error state that caused the failure.
        virtual void fail(ErrorState *err) = 0;

        /// @brief Unregisters a connection, indicating it is no longer in use by the dispatcher.
        /// @param conn The connection to unregister.
        virtual void unregister(Comm::ConnectionPointer &conn) = 0;
};