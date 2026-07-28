#pragma once

#include "SquidConfig.h"
#include "PeerSelectState.h"
#include "CachePeer.h"
#include "salsa2parent.h"
#include <map>
#include <set>

using namespace std;

/// @brief This class represents on object that creates only in proxy cahce, not parent
/// and its created for each reuquest and implements the salsa2 algorithm of parents peers choosing  
class Salsa2Selector{

    private:

        // This 3D array holds all the exclusion probabilities of all parents
        static map<const CachePeer*, ProbabilityMatrix> exclusionProbabilities;
        
        // This point to next peer to choose in the round robin mechanism
        static CachePeer* currentPeer;

        PeerSelector* selector;
        FwdServer*& servers;
        FwdServer* tail;
        HttpRequest* request;
        set<CachePeer*> digestsHits;
        multimap<double, CachePeer*> missProbabilities;
        

        /// @brief Start salsa2 peers selection
        void peerSelection();

        /// @brief Checks for each peer if its digest indicate that it have this request
        void checkDigestsHits();

        /// @brief Check all selection option, and choose ther best
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

        /// @brief Send requsts to selected peers
        void dispatch();

        /// @brief Get probabilities matrix. create it if not exist
        /// @return Probabilities matrix
        static map<const CachePeer*, ProbabilityMatrix>& getProbabilities();

    public:
        Salsa2Selector(PeerSelector* peerSelector, FwdServer*& fwdServers);

        /// @brief Update peer's probabilities matrix when new response arrived
        /// @param reply The reply that came from peer, this give us the new probability
        /// @param request The request for this reply, this tell us witch probability to update
        /// @param peer Peer name that send this responed
        static void updateProbabilty
            (const HttpReply* reply, 
            const HttpRequestPointer request, 
            const CachePeer* peer);
};