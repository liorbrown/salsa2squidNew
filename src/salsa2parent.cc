#include "salsa2parent.h"

#define V_INIT (0.85)

Salsa2Parent* Salsa2Parent::instance = nullptr;

Salsa2Parent::Salsa2Parent(size_t caches): nCaches(caches)
{
    for (size_t i = 0; i <= caches; i++)     
        this->exclusionProbability[0][i] = V_INIT;
}

Salsa2Parent::~Salsa2Parent()
{
    if (this->missCounter)
    {
        delete[] this->missCounter[0];
        delete[] this->missCounter[1];
        delete[] this->missCounter;
    }

    if (this->reqCounter)
    {
        delete[] this->reqCounter[0];
        delete[] this->reqCounter[1];
        delete[] this->reqCounter;
    }

    if (!this->exclusionProbability.empty())
    {
        delete[] this->exclusionProbability[0];
        delete[] this->exclusionProbability[1];
    }

    if (this->clampingCounter)
        delete[] this->clampingCounter;
}

void Salsa2Parent::reEstimateExclusionProb(HttpRequest* request, HttpHeader* responseHeader)
{
    // Gets the relevant parameters according to request type (0 - speculative, 1 - regular)
    // and by number of pos indication for current request
    size_t& misses = this->missCounter[request->isPositive][request->posIndications];
    double& exclusionProb = this->exclusionProbability[request->isPositive][request->posIndications];
    double currDelta = this->delta[request->isPositive];

    // ReEstimate exclusion probability using EWMA
    exclusionProb = currDelta * misses / this->getReEstimateWindowSize() + 
        (1 - currDelta) * exclusionProb;
        
    responseHeader->addEntry(new HttpHeaderEntry(Http::OTHER,
                                                 SBuf("salsa2"),
                                                 std::to_string(exclusionProb).c_str()));

    std::stringstream stream;
    
    for (size_t i = 0; i < 2; i++)
    {
        stream << "\nIs possitive: " << i << " - ";
        
        for (size_t j = i; j < this->nCaches + i; j++)
            stream << '[' << j << "] " << this->missCounter[i][j] 
                    << " / " << this->reqCounter[i][j] 
                    << " = " << this->exclusionProbability[i][j] << " | ";
    }

    debugs(96, 4, "Salsa2: " << stream.str());

    misses = 0;
}

void Salsa2Parent::VClamping(size_t posIndications)
{
    debugs(96, 4, "Salsa2: Clamping v[" << posIndications << ']');

    double &trueNegativeProb = this->exclusionProbability[0][posIndications];

    // Minimize v[i] to V_INIT
    trueNegativeProb = min(trueNegativeProb, V_INIT);
}

void Salsa2Parent::newMiss(HttpRequest::Pointer request) const
{
    if (Salsa2Parent::isSalsa(request))
    {
        debugs(96, 4, "Salsa2: new Miss\n" << *request);

        // Sets request to miss
        request->isMiss = true;
    }
}

void Salsa2Parent::reEstimateProbabilities(HttpRequest* request, HttpHeader* responseHeader)
{
    if (Salsa2Parent::isSalsa(request))
    {
        debugs(96, 4, "Salsa2: new ReEstimation\n" << *request);

        // It's crucial to update all the 3 counters here only
        // for prevent situation when some request update only few of the counters
        // and this will create wrongs in the statistics

        size_t &requests = this->reqCounter[request->isPositive][request->posIndications];
        size_t &clampingCount = this->clampingCounter[request->posIndications];
        size_t &missCount = this->missCounter[request->isPositive][request->posIndications];

        // Increase requests counter for current request type
        ++requests;
        // If current request is specular, add clamping count of v[i]
        clampingCount += !request->isPositive;
        // Increase missCounter if request missed
        missCount += request->isMiss;

        // Check if reach clamsing count, so clamp v[i] to V_INIT 
        if (clampingCount == this->getVClampInterval())
        {
            this->VClamping(request->posIndications);
            clampingCount = 0;
        }
            
        // Check if reach window size, so re-estimate exclusion probability
        if (requests == this->getReEstimateWindowSize())
        {
            this->reEstimateExclusionProb(request, responseHeader);
            requests = 0;
        }
    }
}

Salsa2Parent &Salsa2Parent::getInstance(size_t caches)
{
    // Creates instance only for parent caches, not proxy
    // Check this is parent by check if have no peers in its conf file
    // because proxy know its parents, but parents dont know proxy
    if (caches && Config.salsa2 && !Config.npeers && !instance)
    {
        debugs(96, 4, "Salsa2: Creates salsa2parent instance, with " << caches);
        instance = new Salsa2Parent(caches);
    }

    return *instance;
}

size_t Salsa2Parent::parse
    (const HttpRequest::Pointer request, 
    const std::string peer, 
    bool &isPositive, 
    size_t &posIndications)
{    
    String salsaHeader = request->header.getByName("salsa2");

    debugs(96, 4, "salsa2: header: [" << salsaHeader << ']');

    posIndications = 0;
    isPositive = false;

    std::string segment;
    std::istringstream tokenizer(salsaHeader.rawBuf());

    // Gets caches number from header
    getline(tokenizer, segment, ',');
    size_t nCaches = stoul(segment);

    // Run on all peer's names and count them
    while (std::getline(tokenizer, segment, ','))
    {
        ++posIndications;

        // This is regular requests Iff given peer is in the peers list of this requests
        if (segment.substr(1) == peer)
            isPositive = true;
    }

    return nCaches;
}

void Salsa2Parent::newReq(HttpRequest::Pointer request)
{
    if (Salsa2Parent::isSalsa(request))
    {          
        debugs(96, 4, "Salsa2: new req\n" << *request);

        bool isPos;
        size_t posIndications;
        char hostname[100];

        // The indication if this request is spectular or regular, is if this hostname
        // is in the list of squid hosts that give possitive indication.
        // Note that squid proxy can't send implicity to parent if its spectular or regular
        // because squid mechanism send the same request with same data to all parents
        gethostname(hostname, 100); 

        size_t nCaches = Salsa2Parent::parse(request, hostname, isPos, posIndications);

         // Update request with the request salsa2 data 
        request->posIndications = posIndications;
        request->isPositive = isPos;

        // Init Salsa2Parent singleton if not set already
        Salsa2Parent::getInstance(nCaches);
    }
}

bool
Salsa2Parent::restoreHttpsIfNeeded(HttpRequest* request, char** uri)
{
    // Fiddler workaround: Restore HTTPS scheme for originally-HTTPS requests
    // Only restore if this Squid has NO peers (i.e., it goes DIRECT to origin)
    debugs(96, 4, "salsa2: restoreHttpsIfNeeded for " << request->url 
        << " originally_https=" << request->flags.originally_https 
        << " scheme=" << request->url.getScheme() 
        << " npeers=" << Config.npeers);
    
    if (request->flags.originally_https && request->url.getScheme() == AnyP::PROTO_HTTP) 
    {
        if (!Config.npeers) {
            // This Squid has no peers - going DIRECT to origin, restore HTTPS now
            request->url.setScheme(AnyP::PROTO_HTTPS, "https");
            if (request->url.port() == 80) {
                request->url.port(443);
            }
            debugs(96, 4, "salsa2: Restored HTTPS for DIRECT connection: " << request->url);

            
            // Update uri string to match the restored HTTPS URL
            if (uri && *uri) 
            {
                xfree(*uri);
                *uri = SBufToCstring(request->effectiveRequestUri());

                return true;
            }
        } else {
            // This Squid has peers - keep as HTTP, peers will restore HTTPS
            debugs(96, 4, "salsa2: Keeping HTTP (forwarding to peers): " << request->url);
        }
    }

    return false;
}

#ifdef SALSA_DEBUG

std::ostream& operator<<(std::ostream& stream, HttpRequest &request)
{
    stream << "Req address = " << &request
           << "\nStore ID = " << request.storeId()
           << "\nclient address = " << request.client_addr
           << "\nisPossitive = " << std::boolalpha << request.isPositive
           << "\nposIndications = " << request.posIndications
           << "\nHeader =";

    for(auto entry : request.header.entries)
        stream << '\n' << entry->name.c_str() << ": " << entry->value;

    return stream;
}

#endif