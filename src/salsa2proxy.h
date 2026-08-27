#pragma once

#include "SquidConfig.h"
#include "PeerSelectState.h"
#include "CachePeer.h"
#include "salsa2parent.h"
#include "icp_opcode.h"
#include "typedefs.h"
#include "event.h"
#include <map>
#include <set>

using namespace std;

/// @brief This class represents on object that creates only in proxy cahce, not parent
/// and its created for each reuquest and implements the salsa2 algorithm of parents peers choosing  
class Salsa2Proxy{

    private:

        // This 3D array holds all the exclusion probabilities of all parents
        static map<const CachePeer*, ProbabilityMatrix> exclusionProbabilities;
        
        // This point to next peer to choose in the round robin mechanism
        static CachePeer* currentPeer;

        // Points to the peer that ICP send order should start from next
        // (rotates independently of currentPeer, so no single peer is
        // always first to reply on an all-MISS tie)
        static CachePeer* icpRotationCursor;

        PeerSelector* selector;
        FwdServer*& servers;
        FwdServer* tail;
        HttpRequest* request;
        set<CachePeer*> digestsHits;
        multimap<double, CachePeer*> missProbabilities;

        // Peers chosen by selectPeers() for this request, pending ICP replies
        set<CachePeer*> selectedPeers;

        // Peer we'll forward the GET to: the first HIT, or (once all ICP
        // replies are in with no HIT) the first peer that replied MISS
        CachePeer* winner;

        int nSent;
        int nRecv;

        // Whether the SALSA2_ICP_TIMEOUT_SEC event is still scheduled;
        // guards against a redundant/late eventDelete() (mirrors how
        // PeerSelectorPingMonitor::forget() checks monitorRegistration)
        bool icpTimeoutPending;

        /// @brief Checks for each peer if its digest indicate that it have this request
        void checkDigestsHits();

        /// @brief Check all selection oprion, and choose ther best
        /// @return Peers that select. store as bits in this given integer
        /// 0 - Not choose, 1 - Choose
        size_t exponentialSelection() const;

        /// @brief Calculate cost of specific peers selection
        /// @param peersSelection Peers that select. store as bits in this given integer
        /// 0 - Not choose, 1 - Choose
        /// @return Expectation cost of given choose
        double getCost(size_t peersSelection) const;

        /// @brief Select the peers to ask according to digests result
        void selectPeers();

        /// @brief If no digest give positive indication, select peer with round robin
        void addRoundRobin();

        /// @brief Add peer to peers list of forword them the requsts
        /// @param peer Peer to add
        /// @param code Code that tell why we choose this peer
        void addPeer(CachePeer* peer, hier_code code);

        /// @brief Get probabilities matrix. create it if not exist
        /// @return Probabilities matrix
        static map<const CachePeer*, ProbabilityMatrix>& getProbabilities();

        /// @brief Send an ICP request to every peer in selectedPeers, without
        /// waiting for any response before sending the next one
        void sendIcpRequests();

        /// @brief Registered as the entry's ICP reply callback; forwards to the
        /// handleIcpReply() of the Salsa2Proxy owned by the given PeerSelector
        static IRCB HandlePingReply;

        /// @brief Handle a single ICP response from one of selectedPeers
        void handleIcpReply(CachePeer* peer, icp_opcode op);

        /// @brief Add the winner to the servers list and resume peer selection
        void forward();

        /// @brief Fired if the ICP timeout elapses before we have a winner
        static EVH IcpTimeout;

        /// @brief Fall back to round robin because the ICP timeout elapsed
        void icpTimeout();

    public:
        Salsa2Proxy(PeerSelector* peerSelector, FwdServer*& fwdServers);
        ~Salsa2Proxy();

        /// @brief Start salsa2 peers selection
        void peerSelection();

        /// @brief Update peer's probabilities matrix when new response arrived
        /// @param reply The reply that came from peer, this give us the new probability
        /// @param request The request for this reply, this tell us witch probability to update
        /// @param peer Peer name that send this responed
        static void updateProbabilty
            (const HttpReply* reply, 
            const HttpRequestPointer request, 
            const CachePeer* peer);
};