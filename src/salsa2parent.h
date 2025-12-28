#include <cstddef>
#include "squid.h"
#include "SquidConfig.h"
#include "HttpRequest.h"
#include "CachePeer.h"

#define DELTA_V (0.5)
#define DELTA_PI (0.25)

#ifdef SALSA_DEBUG
    #define MIN_UPDATE_INTERVAL (30)
#else
    #define MIN_UPDATE_INTERVAL (1)
#endif

typedef std::array<double*, 2> ProbabilityMatrix;

/// @brief This is singltone class that contain for this cache 
/// all the statistic data needed for salsa2
class Salsa2Parent{
    private:
        const double delta[2]{DELTA_V, DELTA_PI};
        const size_t nCaches;
        size_t updateInterval = MIN_UPDATE_INTERVAL;
        bool deltaMode = false;

        // All statistics matrices have 2 rows, first for spectular requests
        // (this cahce have negative indication)
        // and seconed for regular requests (this cahce have possitive indication)
        
        size_t** reqCounter = new size_t*[2]
            {new size_t[nCaches + 1]{0}, new size_t[nCaches + 1]{0}};

        size_t* clampingCounter = new size_t[nCaches + 1]{0};

        size_t** missCounter = new size_t*[2] 
            {new size_t[nCaches + 1]{0}, new size_t[nCaches + 1]{0}};

        // No need to init first array because it will init inside ctor
        ProbabilityMatrix exclusionProbability = 
            {new double[nCaches + 1], new double[nCaches + 1]{0.0}};
    
        Salsa2Parent(size_t caches);

        ~Salsa2Parent();

        /// @brief Get window size of re estimate exclusion probability
        /// @return The window size of re estimate exclusion probability
        size_t getReEstimateWindowSize() const 
            {return (max((this->updateInterval / 10), (size_t)1));}

        /// @brief Get window size of when to clamp v[i] to V_INIT
        /// for ensure its not make and stay to high, and then it will not get requests
        /// any more and this will damgage our statistics
        /// @return The window size of when to clamp v[i]
        size_t getVClampInterval() const {return (this->updateInterval * 10);}
        
        /// @brief Updating exclusionProbability according to given request details
        /// @param request This request tells salsa2 which probalitiy we need to estimate
        /// @param request Response header to put on the estimation
        void reEstimateExclusionProb(HttpRequest* request, HttpHeader* responseHeader);

        /// @brief Minimize v[i] to v_init
        /// @param posIndications Number of posIndications to clamp its v[i]
        void VClamping(size_t posIndications);

        /// @brief Checks if given request is handling by salsa2
        /// @param request 
        /// @return True - if handle by salsa2, False - otherwise
        static bool isSalsa(HttpRequest::Pointer request)
        {return (Config.salsa2 &&
                 !Config.npeers && 
                 request->header.getByName("salsa2").size());}

        static Salsa2Parent* instance;
        
    public:
        /// @brief Notify salsa for getting new request.
        /// note that this method is static because in the first request that arrived
        /// the instance not set already, because we dont no the number of peers
        /// until we get it from proxy
        /// @param request New request that arrived
        static void newReq(HttpRequest::Pointer request);

        /// @brief Called when this cache miss request.
        /// Updates the number of misses
        /// @param request The requset that missed
        void newMiss(HttpRequest::Pointer request) const;

        /// @brief Updating exclusionProbability according to given request details
        /// @param request This request tells salsa2 which probalitiy we need to estimate
        /// @param request Response header to put on the estimation
        void reEstimateProbabilities(HttpRequest* request, HttpHeader* responseHeader);

        static Salsa2Parent& getInstance(size_t caches = 0);                

        /// @brief Called when this server shutdown 
        static void free(){if (instance) delete instance;}

        /// @brief Fiddler workaround: Restore HTTPS scheme for originally-HTTPS requests
        /// @param request The request to potentially restore
        static void restoreHttpsIfNeeded(HttpRequest* request);

        /// @brief Parse from request header, data for salsa2
        /// @param request The request to get the data from
        /// @param peer Name of peer that get this request
        /// @param isPositive Return parameter that tell if this regular or specular
        /// @param posIndications The number of possitive indications for this request
        /// @return The number of peers in the cluster 
        static size_t parse
            (const HttpRequest::Pointer request,
             const std::string peer,
             bool &isPositive,
             size_t &posIndications);
};

#ifdef SALSA_DEBUG

/// @brief For debug, print request data
/// @param stream Stream to write into in
/// @param request The request to write
/// @return Given stream
std::ostream& operator<<(std::ostream& stream, HttpRequest &request);

#endif