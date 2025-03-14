/*
 * Copyright (C) 1996-2024 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 13    High Level Memory Pool Management */

#include "squid.h"
#include "base/PackableStream.h"
#include "ClientInfo.h"
#include "dlink.h"
#include "event.h"
#include "fs_io.h"
#include "icmp/net_db.h"
#include "md5.h"
#include "mem/Allocator.h"
#include "mem/Pool.h"
#include "mem/Stats.h"
#include "MemBuf.h"
#include "mgr/Registration.h"
#include "SquidConfig.h"
#include "Store.h"

#include <iomanip>

/* forward declarations */
static void memFree2K(void *);
static void memFree4K(void *);
static void memFree8K(void *);
static void memFree16K(void *);
static void memFree32K(void *);
static void memFree64K(void *);

/* local prototypes */
static void memStringStats(std::ostream &);

/* module locals */
static double xm_time = 0;
static double xm_deltat = 0;

/* string pools */
#define mem_str_pool_count 6

struct PoolMeta {
    const char *name;
    size_t obj_size;
};

static Mem::Meter StrCountMeter;
static Mem::Meter StrVolumeMeter;

static Mem::Meter HugeBufCountMeter;
static Mem::Meter HugeBufVolumeMeter;

/* local routines */

// XXX: refactor objects using these pools to use MEMPROXY classes instead
// then remove this function entirely
static Mem::Allocator *&
GetPool(size_t type)
{
    static Mem::Allocator *pools[MEM_MAX];
    static bool initialized = false;

    if (!initialized) {
        memset(pools, '\0', sizeof(pools));
        initialized = true;
        // Mem::Init() makes use of GetPool(type) to initialize
        // the actual pools. So must come after the flag is true
        Mem::Init();
    }

    return pools[type];
}

static Mem::Allocator &
GetStrPool(size_t type)
{
    static Mem::Allocator *strPools[mem_str_pool_count];
    static bool initialized = false;

    static const PoolMeta PoolAttrs[mem_str_pool_count] = {
        {"Short Strings", Mem::Allocator::RoundedSize(36)}, /* to fit rfc1123 and similar */
        {"Medium Strings", Mem::Allocator::RoundedSize(128)}, /* to fit most urls */
        {"Long Strings", Mem::Allocator::RoundedSize(512)},
        {"1KB Strings", Mem::Allocator::RoundedSize(1024)},
        {"4KB Strings", Mem::Allocator::RoundedSize(4*1024)},
        {"16KB Strings", Mem::Allocator::RoundedSize(16*1024)}
    };

    if (!initialized) {
        memset(strPools, '\0', sizeof(strPools));

        /** Lastly init the string pools. */
        for (int i = 0; i < mem_str_pool_count; ++i) {
            strPools[i] = memPoolCreate(PoolAttrs[i].name, PoolAttrs[i].obj_size);
            strPools[i]->zeroBlocks(false);

            if (strPools[i]->objectSize != PoolAttrs[i].obj_size)
                debugs(13, DBG_IMPORTANT, "WARNING: " << PoolAttrs[i].name <<
                       " is " << strPools[i]->objectSize <<
                       " bytes instead of requested " <<
                       PoolAttrs[i].obj_size << " bytes");
        }

        initialized = true;
    }

    return *strPools[type];
}

/// \returns the best-fit string pool or nil
static Mem::Allocator *
memFindStringPool(size_t net_size, bool fuzzy)
{
    for (unsigned int i = 0; i < mem_str_pool_count; ++i) {
        auto &pool = GetStrPool(i);
        if (fuzzy && net_size < pool.objectSize)
            return &pool;
        if (net_size == pool.objectSize)
            return &pool;
    }
    return nullptr;
}

static void
memStringStats(std::ostream &stream)
{
    int i;
    int pooled_count = 0;
    size_t pooled_volume = 0;
    /* heading */
    stream << "String Pool\t Impact\t\t\n \t (%strings)\t (%volume)\n";
    /* table body */

    for (i = 0; i < mem_str_pool_count; ++i) {
        const auto &pool = GetStrPool(i);
        const auto plevel = pool.meter.inuse.currentLevel();
        stream << std::setw(20) << std::left << pool.label;
        stream << std::right << "\t " << xpercentInt(plevel, StrCountMeter.currentLevel());
        stream << "\t " << xpercentInt(plevel * pool.objectSize, StrVolumeMeter.currentLevel()) << "\n";
        pooled_count += plevel;
        pooled_volume += plevel * pool.objectSize;
    }

    /* malloc strings */
    stream << std::setw(20) << std::left << "Other Strings";
    stream << std::right << "\t ";
    stream << xpercentInt(StrCountMeter.currentLevel() - pooled_count, StrCountMeter.currentLevel()) << "\t ";
    stream << xpercentInt(StrVolumeMeter.currentLevel() - pooled_volume, StrVolumeMeter.currentLevel()) << "\n\n";
}

static void
memBufStats(std::ostream & stream)
{
    stream << "Large buffers: " <<
           HugeBufCountMeter.currentLevel() << " (" <<
           HugeBufVolumeMeter.currentLevel() / 1024 << " KB)\n";
}

void
Mem::Stats(StoreEntry * sentry)
{
    PackableStream stream(*sentry);
    Report(stream);
    memStringStats(stream);
    memBufStats(stream);
#if WITH_VALGRIND
    if (RUNNING_ON_VALGRIND) {
        long int leaked = 0, dubious = 0, reachable = 0, suppressed = 0;
        stream << "Valgrind Report:\n";
        stream << "Type\tAmount\n";
        debugs(13, DBG_IMPORTANT, "Asking valgrind for memleaks");
        VALGRIND_DO_LEAK_CHECK;
        debugs(13, DBG_IMPORTANT, "Getting valgrind statistics");
        VALGRIND_COUNT_LEAKS(leaked, dubious, reachable, suppressed);
        stream << "Leaked\t" << leaked << "\n";
        stream << "Dubious\t" << dubious << "\n";
        stream << "Reachable\t" << reachable << "\n";
        stream << "Suppressed\t" << suppressed << "\n";
    }
#endif
    stream.flush();
}

/*
 * public routines
 */

/*
 * we have a limit on _total_ amount of idle memory so we ignore max_pages for now.
 * Will ignore repeated calls for the same pool type.
 *
 * Relies on Mem::Init() having been called beforehand.
 */
void
memDataInit(mem_type type, const char *name, size_t size, int, bool doZero)
{
    assert(name && size);

    if (GetPool(type) != nullptr)
        return;

    GetPool(type) = memPoolCreate(name, size);
    GetPool(type)->zeroBlocks(doZero);
}

/* find appropriate pool and use it (pools always init buffer with 0s) */
void *
memAllocate(mem_type type)
{
    assert(GetPool(type));
    return GetPool(type)->alloc();
}

/* give memory back to the pool */
void
memFree(void *p, int type)
{
    assert(GetPool(type));
    GetPool(type)->freeOne(p);
}

/* allocate a variable size buffer using best-fit string pool */
void *
memAllocString(size_t net_size, size_t * gross_size)
{
    assert(gross_size);

    if (const auto pool = memFindStringPool(net_size, true)) {
        *gross_size = pool->objectSize;
        assert(*gross_size >= net_size);
        ++StrCountMeter;
        StrVolumeMeter += *gross_size;
        return pool->alloc();
    }

    *gross_size = net_size;
    ++StrCountMeter;
    StrVolumeMeter += *gross_size;
    return xcalloc(1, net_size);
}

void *
memAllocRigid(size_t net_size)
{
    // TODO: Use memAllocString() instead (after it stops zeroing memory).

    if (const auto pool = memFindStringPool(net_size, true)) {
        ++StrCountMeter;
        StrVolumeMeter += pool->objectSize;
        return pool->alloc();
    }

    ++StrCountMeter;
    StrVolumeMeter += net_size;
    return xmalloc(net_size);
}

size_t
memStringCount()
{
    size_t result = 0;

    for (int counter = 0; counter < mem_str_pool_count; ++counter)
        result += GetStrPool(counter).getInUseCount();

    return result;
}

/* free buffer allocated with memAllocString() */
void
memFreeString(size_t size, void *buf)
{
    assert(buf);

    if (const auto pool = memFindStringPool(size, false))
        pool->freeOne(buf);
    else
        xfree(buf);

    --StrCountMeter;
    StrVolumeMeter -= size;
}

void
memFreeRigid(void *buf, size_t net_size)
{
    // TODO: Use memFreeString() instead (after removing fuzzy=false pool search).

    if (const auto pool = memFindStringPool(net_size, true)) {
        pool->freeOne(buf);
        StrVolumeMeter -= pool->objectSize;
        --StrCountMeter;
        return;
    }

    xfree(buf);
    StrVolumeMeter -= net_size;
    --StrCountMeter;
}

/* Find the best fit MEM_X_BUF type */
static mem_type
memFindBufSizeType(size_t net_size, size_t * gross_size)
{
    mem_type type;
    size_t size;

    if (net_size <= 2 * 1024) {
        type = MEM_2K_BUF;
        size = 2 * 1024;
    } else if (net_size <= 4 * 1024) {
        type = MEM_4K_BUF;
        size = 4 * 1024;
    } else if (net_size <= 8 * 1024) {
        type = MEM_8K_BUF;
        size = 8 * 1024;
    } else if (net_size <= 16 * 1024) {
        type = MEM_16K_BUF;
        size = 16 * 1024;
    } else if (net_size <= 32 * 1024) {
        type = MEM_32K_BUF;
        size = 32 * 1024;
    } else if (net_size <= 64 * 1024) {
        type = MEM_64K_BUF;
        size = 64 * 1024;
    } else {
        type = MEM_NONE;
        size = net_size;
    }

    if (gross_size)
        *gross_size = size;

    return type;
}

/* allocate a variable size buffer using best-fit pool */
void *
memAllocBuf(size_t net_size, size_t * gross_size)
{
    mem_type type = memFindBufSizeType(net_size, gross_size);

    if (type != MEM_NONE)
        return memAllocate(type);
    else {
        ++HugeBufCountMeter;
        HugeBufVolumeMeter += *gross_size;
        return xcalloc(1, net_size);
    }
}

/* resize a variable sized buffer using best-fit pool */
void *
memReallocBuf(void *oldbuf, size_t net_size, size_t * gross_size)
{
    /* XXX This can be optimized on very large buffers to use realloc() */
    /* TODO: if the existing gross size is >= new gross size, do nothing */
    size_t new_gross_size;
    void *newbuf = memAllocBuf(net_size, &new_gross_size);

    if (oldbuf) {
        size_t data_size = *gross_size;

        if (data_size > net_size)
            data_size = net_size;

        memcpy(newbuf, oldbuf, data_size);

        memFreeBuf(*gross_size, oldbuf);
    }

    *gross_size = new_gross_size;
    return newbuf;
}

/* free buffer allocated with memAllocBuf() */
void
memFreeBuf(size_t size, void *buf)
{
    mem_type type = memFindBufSizeType(size, nullptr);

    if (type != MEM_NONE)
        memFree(buf, type);
    else {
        xfree(buf);
        --HugeBufCountMeter;
        HugeBufVolumeMeter -= size;
    }
}

static double clean_interval = 15.0;    /* time to live of idle chunk before release */

void
Mem::CleanIdlePools(void *)
{
    MemPools::GetInstance().clean(static_cast<time_t>(clean_interval));
    eventAdd("memPoolCleanIdlePools", CleanIdlePools, nullptr, clean_interval, 1);
}

void
memConfigure(void)
{
    int64_t new_pool_limit;

    /** Set to configured value first */
    if (!Config.onoff.mem_pools)
        new_pool_limit = 0;
    else if (Config.MemPools.limit > 0)
        new_pool_limit = Config.MemPools.limit;
    else {
        if (Config.MemPools.limit == 0)
            debugs(13, DBG_IMPORTANT, "memory_pools_limit 0 has been changed to memory_pools_limit none. Please update your config");
        new_pool_limit = -1;
    }

    MemPools::GetInstance().setIdleLimit(new_pool_limit);
}

void
Mem::Init(void)
{
    /* all pools are ready to be used */
    static bool MemIsInitialized = false;
    if (MemIsInitialized)
        return;

    /**
     * Then initialize all pools.
     * \par
     * Starting with generic 2kB - 64kB buffr pools, then specific object types.
     * \par
     * It does not hurt much to have a lot of pools since sizeof(MemPool) is
     * small; someday we will figure out what to do with all the entries here
     * that are never used or used only once; perhaps we should simply use
     * malloc() for those? @?@
     */
    memDataInit(MEM_2K_BUF, "2K Buffer", 2048, 10, false);
    memDataInit(MEM_4K_BUF, "4K Buffer", 4096, 10, false);
    memDataInit(MEM_8K_BUF, "8K Buffer", 8192, 10, false);
    memDataInit(MEM_16K_BUF, "16K Buffer", 16384, 10, false);
    memDataInit(MEM_32K_BUF, "32K Buffer", 32768, 10, false);
    memDataInit(MEM_64K_BUF, "64K Buffer", 65536, 10, false);
    memDataInit(MEM_DREAD_CTRL, "dread_ctrl", sizeof(dread_ctrl), 0);
    memDataInit(MEM_DWRITE_Q, "dwrite_q", sizeof(dwrite_q), 0);
    memDataInit(MEM_MD5_DIGEST, "MD5 digest", SQUID_MD5_DIGEST_LENGTH, 0);
    GetPool(MEM_MD5_DIGEST)->setChunkSize(512 * 1024);

    MemIsInitialized = true;

    // finally register with the cache manager
    Mgr::RegisterAction("mem", "Memory Utilization", Mem::Stats, 0, 1);
}

static mem_type &
operator++(mem_type &aMem)
{
    int tmp = (int)aMem;
    aMem = (mem_type)(++tmp);
    return aMem;
}

/*
 * Test that all entries are initialized
 */
void
memCheckInit(void)
{
    mem_type t = MEM_NONE;

    while (++t < MEM_MAX) {
        /*
         * If you hit this assertion, then you forgot to add a
         * memDataInit() line for type 't'.
         */
        assert(GetPool(t));
    }
}

void
memClean(void)
{
    if (Config.MemPools.limit > 0) // do not reset if disabled or same
        MemPools::GetInstance().setIdleLimit(0);
    MemPools::GetInstance().clean(0);

    Mem::PoolStats stats;
    const auto poolsInUse = Mem::GlobalStats(stats);
    if (stats.items_inuse) {
        debugs(13, 2, stats.items_inuse <<
               " items in " << stats.chunks_inuse << " chunks and " <<
               poolsInUse << " pools are left dirty");
    }
}

int
memInUse(mem_type type)
{
    return GetPool(type)->getInUseCount();
}

/* ick */

void
memFree2K(void *p)
{
    memFree(p, MEM_2K_BUF);
}

void
memFree4K(void *p)
{
    memFree(p, MEM_4K_BUF);
}

void
memFree8K(void *p)
{
    memFree(p, MEM_8K_BUF);
}

void
memFree16K(void *p)
{
    memFree(p, MEM_16K_BUF);
}

void
memFree32K(void *p)
{
    memFree(p, MEM_32K_BUF);
}

void
memFree64K(void *p)
{
    memFree(p, MEM_64K_BUF);
}

static void
cxx_xfree(void * ptr)
{
    xfree(ptr);
}

FREE *
memFreeBufFunc(size_t size)
{
    switch (size) {

    case 2 * 1024:
        return memFree2K;

    case 4 * 1024:
        return memFree4K;

    case 8 * 1024:
        return memFree8K;

    case 16 * 1024:
        return memFree16K;

    case 32 * 1024:
        return memFree32K;

    case 64 * 1024:
        return memFree64K;

    default:
        --HugeBufCountMeter;
        HugeBufVolumeMeter -= size;
        return cxx_xfree;
    }
}

void
Mem::PoolReport(const PoolStats *mp_st, const PoolMeter *AllMeter, std::ostream &stream)
{
    int excess = 0;
    int needed = 0;
    PoolMeter *pm = mp_st->meter;
    const char *delim = "\t ";

    stream.setf(std::ios_base::fixed);
    stream << std::setw(20) << std::left << mp_st->label << delim;
    stream << std::setw(4) << std::right << mp_st->obj_size << delim;

    /* Chunks */
    if (mp_st->chunk_capacity) {
        stream << std::setw(4) << toKB(mp_st->obj_size * mp_st->chunk_capacity) << delim;
        stream << std::setw(4) << mp_st->chunk_capacity << delim;

        needed = mp_st->items_inuse / mp_st->chunk_capacity;

        if (mp_st->items_inuse % mp_st->chunk_capacity)
            ++needed;

        excess = mp_st->chunks_inuse - needed;

        stream << std::setw(4) << mp_st->chunks_alloc << delim;
        stream << std::setw(4) << mp_st->chunks_inuse << delim;
        stream << std::setw(4) << mp_st->chunks_free << delim;
        stream << std::setw(4) << mp_st->chunks_partial << delim;
        stream << std::setprecision(3) << xpercent(excess, needed) << delim;
    } else {
        stream << delim;
        stream << delim;
        stream << delim;
        stream << delim;
        stream << delim;
        stream << delim;
        stream << delim;
    }
    /*
     *  Fragmentation calculation:
     *    needed = inuse.currentLevel() / chunk_capacity
     *    excess = used - needed
     *    fragmentation = excess / needed * 100%
     *
     *    Fragm = (alloced - (inuse / obj_ch) ) / alloced
     */
    /* allocated */
    stream << mp_st->items_alloc << delim;
    stream << toKB(mp_st->obj_size * pm->alloc.currentLevel()) << delim;
    stream << toKB(mp_st->obj_size * pm->alloc.peak()) << delim;
    stream << std::setprecision(2) << ((squid_curtime - pm->alloc.peakTime()) / 3600.) << delim;
    stream << std::setprecision(3) << xpercent(mp_st->obj_size * pm->alloc.currentLevel(), AllMeter->alloc.currentLevel()) << delim;
    /* in use */
    stream << mp_st->items_inuse << delim;
    stream << toKB(mp_st->obj_size * pm->inuse.currentLevel()) << delim;
    stream << toKB(mp_st->obj_size * pm->inuse.peak()) << delim;
    stream << std::setprecision(2) << ((squid_curtime - pm->inuse.peakTime()) / 3600.) << delim;
    stream << std::setprecision(3) << xpercent(pm->inuse.currentLevel(), pm->alloc.currentLevel()) << delim;
    /* idle */
    stream << mp_st->items_idle << delim;
    stream << toKB(mp_st->obj_size * pm->idle.currentLevel()) << delim;
    stream << toKB(mp_st->obj_size * pm->idle.peak()) << delim;
    /* saved */
    stream << (int)pm->gb_saved.count << delim;
    stream << std::setprecision(3) << xpercent(pm->gb_saved.count, AllMeter->gb_allocated.count) << delim;
    stream << std::setprecision(3) << xpercent(pm->gb_saved.bytes, AllMeter->gb_allocated.bytes) << delim;
    stream << std::setprecision(3) << xdiv(pm->gb_allocated.count - pm->gb_oallocated.count, xm_deltat) << "\n";
    pm->gb_oallocated.count = pm->gb_allocated.count;
}

void
Mem::Report(std::ostream &stream)
{
    static char buf[64];
    int not_used = 0;

    /* caption */
    stream << "Current memory usage:\n";
    /* heading */
    stream << "Pool\t Obj Size\t"
           "Chunks\t\t\t\t\t\t\t"
           "Allocated\t\t\t\t\t"
           "In Use\t\t\t\t\t"
           "Idle\t\t\t"
           "Allocations Saved\t\t\t"
           "Rate\t"
           "\n"
           " \t (bytes)\t"
           "KB/ch\t obj/ch\t"
           "(#)\t used\t free\t part\t %Frag\t "
           "(#)\t (KB)\t high (KB)\t high (hrs)\t %Tot\t"
           "(#)\t (KB)\t high (KB)\t high (hrs)\t %alloc\t"
           "(#)\t (KB)\t high (KB)\t"
           "(#)\t %cnt\t %vol\t"
           "(#)/sec\t"
           "\n";
    xm_deltat = current_dtime - xm_time;
    xm_time = current_dtime;

    /* Get stats for Totals report line */
    PoolStats mp_total;
    const auto poolsInUse = GlobalStats(mp_total);

    std::vector<PoolStats> usedPools;
    usedPools.reserve(poolsInUse);

    /* main table */
    for (const auto pool : MemPools::GetInstance().pools) {
        PoolStats mp_stats;
        pool->getStats(mp_stats);

        if (mp_stats.pool->meter.gb_allocated.count > 0)
            usedPools.emplace_back(mp_stats);
        else
            ++not_used;
    }

    // sort on %Total Allocated (largest first)
    std::sort(usedPools.begin(), usedPools.end(), [](const PoolStats &a, const PoolStats &b) {
        return (double(a.obj_size) * a.meter->alloc.currentLevel()) > (double(b.obj_size) * b.meter->alloc.currentLevel());
    });

    for (const auto &pool: usedPools) {
        PoolReport(&pool, mp_total.meter, stream);
    }

    PoolReport(&mp_total, mp_total.meter, stream);

    /* Cumulative */
    stream << "Cumulative allocated volume: "<< double_to_str(buf, 64, mp_total.meter->gb_allocated.bytes) << "\n";
    /* overhead */
    stream << "Current overhead: " << mp_total.overhead << " bytes (" <<
           std::setprecision(3) << xpercent(mp_total.overhead, mp_total.meter->inuse.currentLevel()) << "%)\n";
    /* limits */
    if (MemPools::GetInstance().idleLimit() >= 0)
        stream << "Idle pool limit: " << std::setprecision(2) << toMB(MemPools::GetInstance().idleLimit()) << " MB\n";
    /* limits */
    auto poolCount = MemPools::GetInstance().pools.size();
    stream << "Total Pools created: " << poolCount << "\n";
    stream << "Pools ever used:     " << poolCount - not_used << " (shown above)\n";
    stream << "Currently in use:    " << poolsInUse << "\n";
}

