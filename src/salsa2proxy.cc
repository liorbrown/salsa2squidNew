#include "squid.h"
#include "debug/Stream.h"
#include "HttpRequest.h"
#include "HttpReply.h"
#include "salsa2proxy.h"
#include <random>
#include "neighbors.h"
#include "mem/AllocatorProxy.h"
#include "http/RegisteredHeaders.h"
#include "ICP.h"
#include "comm/Connection.h"
#include "Store.h"

#define NOT_FOUND 99999
#define V_INIT (0.85)

// Hardcoded for now; later this should come from squid.conf.
#define SALSA2_ICP_TIMEOUT_SEC 2.0

using namespace std;

// Copy this class from peer_select.cc
// I prefer to copy only this code instead of include all peer_select.cc
// This class represents a forward server, which could be a peer cache or the origin server.
class FwdServer
{
    MEMPROXY_CLASS(FwdServer);

public:
    // Constructor for the FwdServer.
    // Takes a CachePeer pointer (nullptr if it's the origin server) and a hierarchy code.
    FwdServer(CachePeer *p, hier_code c) :
        _peer(p),
        code(c),
        next(nullptr)
    {}

    // Pointer to the CachePeer object. If it's the origin server, this will be NULL.
    CbcPointer<CachePeer> _peer;
    // Hierarchy code indicating the type of forward server (e.g., parent hit, round-robin).
    hier_code code;
    // Pointer to the next FwdServer in a linked list.
    FwdServer *next;
};

#ifdef SALSA_DEBUG

ostream& operator<<(ostream& os, const map<CachePeer*, double> arr);
ostream& operator<<(ostream& os, const map<CachePeer*, double> arr)
{
    for (auto &d : arr)
        os << '\n' << *d.first << " : " << d.second;

    return os;
}

ostream& operator<<(ostream& os, const multimap<double, CachePeer*> arr);
ostream& operator<<(ostream& os, const multimap<double, CachePeer*> arr)
{
    for (auto &d : arr)
        os << '\n' << *d.second << " : " << d.first;

    return os;
}

ostream& operator<<(ostream& os, const ProbabilityMatrix& arr);
ostream& operator<<(ostream& os, const ProbabilityMatrix& arr) 
{
    size_t size = Config.npeers + 1;

    for (size_t i = 0; i < 2; ++i) 
    {
        os << "\n[";

        for (size_t j = 0; j < size; ++j) 
        {
            os << arr[i][j];

            if (j < size - 1) 
            {
                os << ", ";
            }
        }

        os << "]";
    }

    return os;
}

string to_string(const map<const CachePeer*, ProbabilityMatrix>& m);
string to_string(const map<const CachePeer*, ProbabilityMatrix>& m) 
{
    ostringstream os;

    for (const auto& pair : m)
        os << '\n' << *pair.first << pair.second;
    
    return os.str();
}

#endif

// Static member to keep track of the current peer for round-robin selection.
CachePeer* Salsa2Proxy::currentPeer = nullptr;

// Static member to rotate the ICP send order across requests.
CachePeer* Salsa2Proxy::icpRotationCursor = nullptr;

map<const CachePeer*, ProbabilityMatrix> Salsa2Proxy::exclusionProbabilities;

Salsa2Proxy::Salsa2Proxy(PeerSelector *peerSelector, FwdServer *&fwdServers):
    selector(peerSelector),
    servers(fwdServers),
    tail(nullptr),
    request(peerSelector->request),
    digestsHits({}),
    winner(nullptr),
    nSent(0),
    nRecv(0),
    icpTimeoutPending(false)
{
}

Salsa2Proxy::~Salsa2Proxy()
{
    if (this->icpTimeoutPending)
        eventDelete(&Salsa2Proxy::IcpTimeout, this);
}

map<const CachePeer*, ProbabilityMatrix>& Salsa2Proxy::getProbabilities()
{
    // If exclusionProbabilities is empty, init it
    if (Salsa2Proxy::exclusionProbabilities.empty())
    {
        // Runs on all peers in configuration
        for (CachePeer* p = Config.peers; p; p = p->next)
        {
            // Create new matrix for current peer, and init its values
            auto newMatrix = Salsa2Proxy::exclusionProbabilities[p] = 
                {new double[Config.npeers + 1], 
                 new double[Config.npeers + 1]{0.0}};
                

            fill_n(newMatrix[0], Config.npeers + 1, V_INIT);
        }

        debugs(96, 4, "Salsa2: exclusionProbabilities:\n" 
            << to_string(Salsa2Proxy::exclusionProbabilities));
    }

    return Salsa2Proxy::exclusionProbabilities;
}

void Salsa2Proxy::updateProbabilty
    (const HttpReply* reply, 
    const HttpRequestPointer request, 
    const CachePeer* peer)
{
    if (peer)
    {
        String updateProb = reply->header.getByName("salsa2");
        
        // Check if response contains probability update value
        if (updateProb.size())
        {
            debugs(96, 4, "salsa2: Reply header: " << updateProb);

            bool isPos;
            size_t posIndications;

            // Get isPossitive and number of possitive indications from request header
            Salsa2Parent::parse(request, peer->name, isPos, posIndications);

            // Update right cell with new probability
            Salsa2Proxy::getProbabilities()[peer][isPos][posIndications] =
                stod(updateProb.rawBuf());

            debugs(96, 4, "Salsa2: exclusionProbabilities:" 
                << to_string(Salsa2Proxy::exclusionProbabilities));
        }
    }
}

void Salsa2Proxy::peerSelection()
{
    debugs(96,4,"Salsa2: Starting salsa2 peer selection for URL: " 
        << this->request->storeId());

    // Initialize the list of selected forward servers to empty
    this->servers = nullptr;

    // Add header salsa2 with number of caches
    this->request->header.addEntry(new HttpHeaderEntry(Http::OTHER, 
                                    SBuf("salsa2"), 
                                    to_string(Config.npeers).c_str()));

    // Check for digest hits among the available peers
    this->checkDigestsHits();

    // Apply the Salsa2 peer selection logic
    this->selectPeers();

    if (this->selectedPeers.empty())
    {
        // No peer selected by Salsa2: route to some parent using round robin
        debugs(96,DBG_CRITICAL,"Salsa2: No peers selected, falling back to round robin");
        this->addRoundRobin();
        if (this->selector->entry)
            this->selector->entry->ping_status = PING_DONE;
        return;
    }

    if (!this->selector->entry ||
        Config.Port.icp <= 0 ||
        !Comm::IsConnOpen(icpOutgoingConn))
    {
        // Can't ICP (no StoreEntry to track replies on, or ICP disabled):
        // fall back to round robin exactly like the no-selection case
        debugs(96,DBG_CRITICAL,"Salsa2: ICP unavailable, falling back to round robin");
        this->addRoundRobin();
        if (this->selector->entry)
            this->selector->entry->ping_status = PING_DONE;
        return;
    }

    this->sendIcpRequests();
}

// This function checks the digests of each peer to see if they have the requested content.
void Salsa2Proxy::checkDigestsHits()
{
    #ifdef REQ_UPDATE
    // Index to keep track of the current cache in the cachesData array.
    int cacheIndex = -1;
    #endif

    // Reset the number of choices and ideal choices for hierarchical selection.
    request->hier.n_choices = request->hier.n_ichoices = 0;

    // Generate a public store key for the requested object.
    storeKeyPublicByRequest(this->request);

    // Iterate through the configured peers.
    for (CachePeer* peer = Config.peers; peer; peer = peer->next)
    {
        #ifdef REQ_UPDATE
        // Store the name of the current peer in the cachesData array.
        this->cachesData[++cacheIndex].name = peer->name;
        #endif

        // Perform a digest lookup for the current peer.
        lookup_t lookup = peerDigestLookup(peer, this->selector);

        debugs(96,4,"Salsa2: Checking digest of " << *peer << " Result: " << lookup
                << " Digest URL is: " << peer->digest_url);

        // If the digest lookup was not negative (i.e., the peer has some information about the object).
        if (peerHTTPOkay(peer, this->selector) && lookup != LOOKUP_NONE)
        {
            // Increment the total number of choices for hierarchical selection.
            this->request->hier.n_choices++;

            // If the digest lookup was a hit and the peer is HTTP okay (available and healthy).
            if (lookup == LOOKUP_HIT)
            {                
                this->digestsHits.insert(peer);

                // Increment the number of ideal choices (peers with a digest hit).
                this->request->hier.n_ichoices++;

                // Add this peer to the list of forward servers with the code indicating a parent hit.
                //this->addPeer(peer, CD_PARENT_HIT);          

                // Add peer name to request header
                this->request->header.addEntry(new HttpHeaderEntry(Http::OTHER,
                                                SBuf("salsa2"),
                                                peer->name));
            }                        
        }
    }
}

void Salsa2Proxy::addPeer(CachePeer* peer, hier_code code)
{
    // Create a new FwdServer object.
    FwdServer* newTail = new FwdServer(peer, code);

    // If the list of servers is currently empty.
    if (!this->tail)
        // Set the head and tail of the list to the new server.
        this->servers = this->tail = newTail;
    else
        // Append the new server to the end of the list and update the tail.
        this->tail = this->tail->next = newTail;
}

size_t Salsa2Proxy::exponentialSelection() const
{
    // If expextation greater that miss penalty, so we prefer to not choose any peer
    double minExpectaion = Config.missPenalty;
    size_t result = 0;
    size_t combinations = pow(2, Config.npeers);

    // Runs on all peers select options
    // Any select option is represent by corresponding bit
    for (size_t i = 1; i < combinations; i++)
    {
        double expectaion = this->getCost(i);
        
        // If its best expectation for now, save this index
        if (expectaion < minExpectaion)
        {
            minExpectaion = expectaion;
            result = i;
        }
    }

    debugs(96, 4, "Selected peers are: " << result);

    return result;
}

double Salsa2Proxy::getCost(size_t peersSelection) const
{
    double missProbability = 1;
    double expectaion = 0;

    // Run on all peers, until currIndex is 0
    // because its says there are no more peers to select
    // for this selection
    for 
    (
        auto currPeer = begin(this->missProbabilities); 
        peersSelection;
        ++currPeer
    )
    {
        // Check if current peer bit is 1, then choose it
        if (peersSelection % 2)
        {
            // Add its cost to expectation
            expectaion += currPeer->second->access_cost;

            // Every peer we check, decrease the probability of missing
            missProbability *= currPeer->first;
        }

        // Move to next bit
        peersSelection /= 2;
    }

    // The cost expectation is added by multilpy miss penalty with its probability
    expectaion += missProbability * Config.missPenalty;

    return expectaion;
}

void Salsa2Proxy::selectPeers()
{
    // Runs on all peers, and calculate their miss probability for current request
    for (CachePeer* peer = Config.peers; peer; peer = peer->next)
        this->missProbabilities.insert({Salsa2Proxy::getProbabilities()
            [peer]
            [this->digestsHits.find(peer) != end(this->digestsHits)]
            [this->request->hier.n_ichoices],
            peer});
    
    debugs(96, 4, "salsa2: missProbabilities:" << this->missProbabilities);

    // Gets best peers selection by naive selection
    size_t selection = this->exponentialSelection();

    // Run on all peers and add to selection the selected peers.
    // if selection = 0 it says that the best option is to leave peers and go directly
    for 
    (
        auto currPeer = begin(this->missProbabilities); 
        selection;
        ++currPeer
    )
    {
        // Check if current peer is in selection,
        // by checking if its bit is 1
        if (selection % 2)
            this->selectedPeers.insert(currPeer->second);

        // Move to next bit/peer
        selection /= 2;
    }
}

void Salsa2Proxy::addRoundRobin()
{
    // If no servers have been selected yet by salsa2
    if (!this->servers)
    {
        debugs(96,4,"Salsa2: No digest match for URL: " << this->request->storeId());

        // Iterate through all configured peers.
        for (int i = 0; i < Config.npeers; i++)
        {
            // If the currentPeer pointer is null, initialize it to the first peer in the list.
            if (!currentPeer)
                currentPeer = Config.peers;

            // Ensure that currentPeer is not null.
            assert(currentPeer);

            // If the current peer is HTTP okay (available and healthy).
            if (peerHTTPOkay(currentPeer, this->selector))
            {
                // Add this peer to the list of forward servers with the round-robin code.
                this->addPeer(currentPeer,ROUNDROBIN_PARENT);
                debugs(96,4,"Salsa2: Add " << *currentPeer << " as round robin");

                #ifdef REQ_UPDATE
                // Mark this peer as accessed in the cachesData array.
                size_t index = this->getPeerIndex(currentPeer->name);
                if (index != NOT_FOUND)
                    this->cachesData[index].accessed = 1;
                #endif

                // Move to the next peer in the list for the next round-robin selection.
                currentPeer = currentPeer->next;

                // Return after adding the first available peer in round-robin.
                return;
            }

            // Move to the next peer even if the current one is not HTTP okay.
            currentPeer = currentPeer->next;
        }

        // If the loop finishes without finding any online parent.
        debugs(96,4,"Salsa2: Not found any online parent");
    }
}

void Salsa2Proxy::sendIcpRequests()
{
    StoreEntry* entry = this->selector->entry;
    const char* url = entry->url();
    int reqnum = icpSetCacheKey((const cache_key*)entry->key);

    if (this->selectedPeers.empty())
    {
        debugs(96,DBG_CRITICAL,"Salsa2: No peers selected, falling back to round robin");
        this->addRoundRobin();
        entry->ping_status = PING_DONE;
        return;
    }

    debugs(96,DBG_CRITICAL,"Salsa2: Sending ICP requests for URL: " << url);

    // Rotate the ICP send order across requests instead of always sending
    // in the same fixed order (selectedPeers is a set<CachePeer*>, ordered
    // by pointer value, which would otherwise make the same peer first
    // every time for the whole process lifetime). Walk the full configured
    // peer list starting from the rotation cursor, sending only to peers
    // that are actually selected, then advance the cursor by one position
    // for next time - e.g. with 4 peers: 1,2,3,4 then 2,3,4,1 then 3,4,1,2...
    if (!icpRotationCursor)
        icpRotationCursor = Config.peers;

    CachePeer* p = icpRotationCursor;
    do
    {
        if (this->selectedPeers.count(p))
        {
            // No ICP_FLAG_SRC_RTT: Salsa2 doesn't use RTT for peer selection,
            // so it doesn't need Config.onoff.query_icmp set to work.
            icpCreateAndSend(ICP_QUERY, 0, url, reqnum, 0,
                             icpOutgoingConn->fd, p->in_addr, nullptr);

            debugs(96,DBG_CRITICAL,"Salsa2: Sent ICP request to " << *p);
        }

        p = p->next ? p->next : Config.peers;
    } while (p != icpRotationCursor);

    icpRotationCursor = icpRotationCursor->next ? icpRotationCursor->next : Config.peers;

    this->nSent = static_cast<int>(this->selectedPeers.size());

    entry->mem_obj->ping_reply_callback = &Salsa2Proxy::HandlePingReply;
    entry->mem_obj->ircb_data = this->selector;
    entry->ping_status = PING_WAITING;

    eventAdd("Salsa2Proxy::IcpTimeout", &Salsa2Proxy::IcpTimeout, this,
             SALSA2_ICP_TIMEOUT_SEC, 0, false);
    this->icpTimeoutPending = true;
}

void Salsa2Proxy::HandlePingReply(CachePeer* peer, peer_t, AnyP::ProtocolType proto, void* pingdata, void* data)
{
    if (proto != AnyP::PROTO_ICP)
    {
        debugs(96,DBG_CRITICAL,"Salsa2: ERROR: ignoring a non-ICP reply with protocol " << proto);
        return;
    }

    PeerSelector* selector = static_cast<PeerSelector*>(data);

    if (!selector->salsa2)
    {
        debugs(96,DBG_CRITICAL,"Salsa2: ERROR: ICP reply with no live Salsa2Proxy");
        return;
    }

    selector->salsa2->handleIcpReply(peer, static_cast<icp_common_t*>(pingdata)->getOpCode());
}

void Salsa2Proxy::handleIcpReply(CachePeer* peer, icp_opcode op)
{
    ++this->nRecv;

    if (op == ICP_HIT)
    {
        debugs(96,DBG_CRITICAL,"Salsa2: ICP HIT from " << *peer);
        this->winner = peer;
        this->forward();
        return;
    }

    debugs(96,DBG_CRITICAL,"Salsa2: ICP MISS from " << *peer);

    // Remember only the first peer that replied MISS
    if (!this->winner)
        this->winner = peer;

    if (this->nRecv < this->nSent)
        return; // still waiting for other replies

    // All replies are in and none of them was a HIT: forward to the first MISS
    this->forward();
}

void Salsa2Proxy::forward()
{
    // Cancel our own ICP timeout: we already have a winner
    if (this->icpTimeoutPending)
    {
        eventDelete(&Salsa2Proxy::IcpTimeout, this);
        this->icpTimeoutPending = false;
    }

    debugs(96,DBG_CRITICAL,"Salsa2: Forwarding to winner " << *(this->winner));

    // Prevents any further (late/duplicate) ICP reply for this entry from
    // being delivered to us, and guards against double forwarding
    this->selector->entry->ping_status = PING_DONE;

    this->addPeer(this->winner, SALSA2);

    PeerSelector* sel = this->selector;
    sel->selectMore();
}

void Salsa2Proxy::IcpTimeout(void* data)
{
    static_cast<Salsa2Proxy*>(data)->icpTimeout();
}

void Salsa2Proxy::icpTimeout()
{
    StoreEntry* entry = this->selector->entry;

    // The event that fired us has already been dequeued by the scheduler
    // itself; nothing left for us to cancel.
    this->icpTimeoutPending = false;

    // Do nothing if we already got a winner while this timeout was queued
    if (!entry || entry->ping_status != PING_WAITING)
        return;

    debugs(96,DBG_CRITICAL,"Salsa2: ICP timeout before we got a winner, falling back to round robin");

    entry->ping_status = PING_DONE;

    this->addRoundRobin();

    PeerSelector* sel = this->selector;
    sel->selectMore();
}