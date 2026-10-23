#include "platform/rollback_snapshot.h"

#if defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

// No std containers: in the game module the global operator new is the
// original nlMalloc, and the snapshots must not allocate in the arenas they save.
namespace mscharged::platform
{
namespace
{
constexpr int kMaxRegions = 256;
constexpr int kHotIdleSaves = 16; // saves a hot page stays unchanged before it turns cold

struct Span
{
    std::uintptr_t start;
    std::size_t pages;
    std::size_t first; // index of its first page
};
// The bytes of a page that belong to the regions.
struct Segment
{
    std::uint32_t offset, bytes;
};
// The undo of one save: the history slots [first, end), counted from the start.
struct SaveRecord
{
    std::uint64_t first, end;
};

struct Tracker
{
    RollbackRegion regions[kMaxRegions];
    int regionCount;
    int depth;
    std::size_t historyPages;

    std::size_t pageSize;
    Span spans[kMaxRegions];
    int spanCount;
    std::size_t pages;
    std::uint32_t* segmentFirst; // per page, into segments
    std::uint8_t* segmentCount;  // per page
    Segment* segments;

    std::uint8_t* faulted; // per page, set by the fault handler (padded to 8 bytes)
    std::uint8_t* hot;
    std::uint8_t* idle;
    std::uint32_t* hotList;
    std::size_t hotCount;
    std::uint8_t* shadow; // the regions' bytes as of the last save, page by page

    std::uint8_t* history; // historyPages slots of undo, a ring
    std::uint32_t* historyPage;
    std::uint64_t head, tail; // slots in use: [tail, head)
    SaveRecord* saves;        // depth + 1, by id
    std::uint64_t oldest, latest;
    bool overflow; // the save under way did not fit in the history

    int active;
    RollbackSnapshotStats stats;
};
Tracker* sTracker;

std::uint64_t NowNs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uint8_t* PageAddress(const Tracker& t, std::size_t page)
{
    for (int i = 0; i < t.spanCount; ++i)
        if (page - t.spans[i].first < t.spans[i].pages)
            return reinterpret_cast<std::uint8_t*>(t.spans[i].start + (page - t.spans[i].first) * t.pageSize);
    return nullptr;
}

void Protect(void* address, std::size_t bytes, bool writable);

// A write fault at address: when it is a tracked page's, the page is marked
// and made writable, and the write can run again. Async-signal-safe: atomics
// on the tracker's own arrays and the protection change.
bool OnWriteFault(std::uintptr_t address)
{
    Tracker* t = sTracker;
    if (!t || !__atomic_load_n(&t->active, __ATOMIC_ACQUIRE)) return false;
    for (int i = 0; i < t->spanCount; ++i)
    {
        const Span& span = t->spans[i];
        if (address < span.start || address - span.start >= span.pages * t->pageSize) continue;
        const std::size_t page = (address - span.start) / t->pageSize;
        if (!__atomic_exchange_n(&t->faulted[span.first + page], 1, __ATOMIC_ACQ_REL))
            __atomic_add_fetch(&t->stats.faults, 1, __ATOMIC_RELAXED);
        Protect(reinterpret_cast<void*>(span.start + page * t->pageSize), t->pageSize, true);
        return true;
    }
    return false;
}

#if defined(__linux__) || defined(__APPLE__)
struct sigaction sPreviousSegv;
#if defined(__APPLE__)
struct sigaction sPreviousBus; // protection faults arrive as SIGBUS on some macOS versions
#endif
bool sHandlerInstalled;

std::size_t SystemPageSize()
{
    return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
}

void Protect(void* address, std::size_t bytes, bool writable)
{
    mprotect(address, bytes, writable ? PROT_READ | PROT_WRITE : PROT_READ);
}

void Forward(const struct sigaction& previous, int signal, siginfo_t* info, void* context)
{
    if (previous.sa_flags & SA_SIGINFO)
    {
        if (previous.sa_sigaction) previous.sa_sigaction(signal, info, context);
        return;
    }
    if (previous.sa_handler == SIG_IGN) return;
    if (previous.sa_handler == SIG_DFL || !previous.sa_handler)
    {
        ::signal(signal, SIG_DFL); // the faulting instruction runs again and ends the process
        return;
    }
    previous.sa_handler(signal);
}

void Fault(int signal, siginfo_t* info, void* context)
{
    if (info && OnWriteFault(reinterpret_cast<std::uintptr_t>(info->si_addr))) return;
#if defined(__APPLE__)
    if (signal == SIGBUS)
    {
        Forward(sPreviousBus, signal, info, context);
        return;
    }
#endif
    Forward(sPreviousSegv, signal, info, context);
}

bool InstallHandler()
{
    if (sHandlerInstalled) return true;
    struct sigaction action{};
    action.sa_sigaction = Fault;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, &sPreviousSegv) != 0) return false;
#if defined(__APPLE__)
    if (sigaction(SIGBUS, &action, &sPreviousBus) != 0) return false;
#endif
    sHandlerInstalled = true;
    return true;
}
#elif defined(_WIN32)
PVOID sHandler;

std::size_t SystemPageSize()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}

// Spans merge regions that meet, which may be separate allocations: one call
// cannot change pages of two, so a failed change goes allocation by allocation.
void Protect(void* address, std::size_t bytes, bool writable)
{
    const DWORD protection = writable ? PAGE_READWRITE : PAGE_READONLY;
    DWORD previous;
    if (VirtualProtect(address, bytes, protection, &previous)) return;
    auto* at = static_cast<std::uint8_t*>(address);
    std::uint8_t* const end = at + bytes;
    while (at < end)
    {
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQuery(at, &region, sizeof region)) return;
        std::uint8_t* const next = std::min(end, static_cast<std::uint8_t*>(region.BaseAddress) + region.RegionSize);
        VirtualProtect(at, static_cast<SIZE_T>(next - at), protection, &previous);
        at = next;
    }
}

LONG CALLBACK Fault(EXCEPTION_POINTERS* exception)
{
    const EXCEPTION_RECORD* record = exception->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2
        && record->ExceptionInformation[0] == 1 // a write
        && OnWriteFault(static_cast<std::uintptr_t>(record->ExceptionInformation[1])))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

bool InstallHandler()
{
    if (!sHandler) sHandler = AddVectoredExceptionHandler(1, Fault);
    return sHandler != nullptr;
}
#else
std::size_t SystemPageSize() { return 4096; }
void Protect(void*, std::size_t, bool) {}
bool InstallHandler() { return false; }
#endif

bool Differs(const Tracker& t, std::size_t page, const std::uint8_t* live)
{
    const std::uint8_t* saved = t.shadow + page * t.pageSize;
    for (std::uint32_t s = 0; s < t.segmentCount[page]; ++s)
    {
        const Segment& segment = t.segments[t.segmentFirst[page] + s];
        if (std::memcmp(live + segment.offset, saved + segment.offset, segment.bytes)) return true;
    }
    return false;
}

void CopySegments(const Tracker& t, std::size_t page, std::uint8_t* to, const std::uint8_t* from)
{
    for (std::uint32_t s = 0; s < t.segmentCount[page]; ++s)
    {
        const Segment& segment = t.segments[t.segmentFirst[page] + s];
        std::memcpy(to + segment.offset, from + segment.offset, segment.bytes);
    }
}

void MakeHot(Tracker& t, std::size_t page)
{
    t.hot[page] = 1;
    t.idle[page] = 0;
    t.hotList[t.hotCount++] = static_cast<std::uint32_t>(page);
}

// Drops the oldest kept save: its successor's undo is no longer needed.
bool DropOldest(Tracker& t, std::uint64_t current)
{
    if (t.oldest + 1 >= current) return false;
    ++t.oldest;
    t.tail = t.saves[(t.oldest + 1) % (t.depth + 1)].first; // current's own record is open but has its first
    return true;
}

// Keeps what the shadow holds for page as the undo of the save under way.
void PushUndo(Tracker& t, std::size_t page, std::uint64_t current)
{
    if (t.overflow) return;
    while (t.head - t.tail >= t.historyPages)
    {
        if (!DropOldest(t, current))
        {
            t.overflow = true; // this save alone needs more room: no save before it stays restorable
            return;
        }
        ++t.stats.evicted; // before its time: the history was full (a save past the depth goes as planned)
    }
    const std::size_t slot = t.head++ % t.historyPages;
    t.historyPage[slot] = static_cast<std::uint32_t>(page);
    CopySegments(t, page, t.history + slot * t.pageSize, t.shadow + page * t.pageSize);
}

// The cold pages that faulted since the last call, each passed to visit once.
template<typename Visit> void TakeFaulted(Tracker& t, Visit visit)
{
    const std::size_t words = (t.pages + 7) / 8;
    auto* bytes = t.faulted;
    for (std::size_t w = 0; w < words; ++w)
    {
        std::uint64_t word;
        std::memcpy(&word, bytes + w * 8, 8); // a racing fault is taken by the next call
        if (!word) continue;
        for (std::size_t page = w * 8; page < std::min(t.pages, w * 8 + 8); ++page)
            if (__atomic_exchange_n(&bytes[page], 0, __ATOMIC_ACQ_REL)) visit(page);
    }
}

void Record(std::uint64_t value, std::uint64_t& total, std::uint64_t& max)
{
    total += value;
    max = std::max(max, value);
}

void FreeTracker(Tracker* t)
{
    if (!t) return;
    std::free(t->segmentFirst);
    std::free(t->segmentCount);
    std::free(t->segments);
    std::free(t->faulted);
    std::free(t->hot);
    std::free(t->idle);
    std::free(t->hotList);
    std::free(t->shadow);
    std::free(t->history);
    std::free(t->historyPage);
    std::free(t->saves);
    std::free(t);
}

// regions sorted by base.
bool SameConfiguration(const Tracker& t, const RollbackRegion* regions, int count, int depth, std::size_t historyPages)
{
    if (t.regionCount != count || t.depth != depth || t.historyPages != historyPages) return false;
    for (int i = 0; i < count; ++i)
        if (t.regions[i].base != regions[i].base || t.regions[i].bytes != regions[i].bytes) return false;
    return true;
}
}

bool ConfigureRollbackSnapshots(const RollbackRegion* regions, int count, int depth, std::size_t historyPages)
{
    if (count <= 0 || count > kMaxRegions || depth < 1 || !historyPages) return false;
    if (sTracker && __atomic_load_n(&sTracker->active, __ATOMIC_ACQUIRE)) return false;
    RollbackRegion sorted[kMaxRegions];
    std::copy(regions, regions + count, sorted);
    std::sort(sorted, sorted + count, [](const RollbackRegion& a, const RollbackRegion& b) { return a.base < b.base; });
    if (sTracker && SameConfiguration(*sTracker, sorted, count, depth, historyPages)) return true;
    if (!InstallHandler()) return false;

    // The fault handler reads the tracker: retire the old one only while inactive.
    Tracker* old = sTracker;
    __atomic_store_n(&sTracker, static_cast<Tracker*>(nullptr), __ATOMIC_RELEASE);
    FreeTracker(old);

    auto* t = static_cast<Tracker*>(std::calloc(1, sizeof(Tracker)));
    if (!t) return false;
    t->regionCount = count;
    std::copy(sorted, sorted + count, t->regions);
    t->depth = depth;
    t->historyPages = historyPages;
    t->pageSize = SystemPageSize();
    const std::uintptr_t mask = t->pageSize - 1;

    // Spans: the regions' pages, merged where they meet.
    for (int r = 0; r < count; ++r)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(t->regions[r].base);
        const std::uintptr_t start = base & ~mask, end = (base + t->regions[r].bytes + mask) & ~mask;
        Span* last = t->spanCount ? &t->spans[t->spanCount - 1] : nullptr;
        if (last && start <= last->start + last->pages * t->pageSize)
            last->pages = std::max(last->pages, (end - last->start) / t->pageSize);
        else
            t->spans[t->spanCount++] = {start, (end - start) / t->pageSize, 0};
    }
    for (int i = 0; i < t->spanCount; ++i)
    {
        t->spans[i].first = t->pages;
        t->pages += t->spans[i].pages;
    }

    const std::size_t maxSegments = t->pages + 2 * static_cast<std::size_t>(count);
    t->segmentFirst = static_cast<std::uint32_t*>(std::calloc(t->pages, sizeof(std::uint32_t)));
    t->segmentCount = static_cast<std::uint8_t*>(std::calloc(t->pages, 1));
    t->segments = static_cast<Segment*>(std::calloc(maxSegments, sizeof(Segment)));
    t->faulted = static_cast<std::uint8_t*>(std::calloc((t->pages + 7) / 8 * 8, 1));
    t->hot = static_cast<std::uint8_t*>(std::calloc(t->pages, 1));
    t->idle = static_cast<std::uint8_t*>(std::calloc(t->pages, 1));
    t->hotList = static_cast<std::uint32_t*>(std::calloc(t->pages, sizeof(std::uint32_t)));
    t->shadow = static_cast<std::uint8_t*>(std::malloc(t->pages * t->pageSize));
    t->history = static_cast<std::uint8_t*>(std::malloc(historyPages * t->pageSize));
    t->historyPage = static_cast<std::uint32_t*>(std::calloc(historyPages, sizeof(std::uint32_t)));
    t->saves = static_cast<SaveRecord*>(std::calloc(static_cast<std::size_t>(depth) + 1, sizeof(SaveRecord)));
    if (!t->segmentFirst || !t->segmentCount || !t->segments || !t->faulted || !t->hot || !t->idle || !t->hotList
        || !t->shadow || !t->history || !t->historyPage || !t->saves)
    {
        FreeTracker(t);
        return false;
    }

    // Segments, page by page: regions are sorted and disjoint.
    std::size_t segmentCount = 0;
    int region = 0;
    for (int i = 0; i < t->spanCount; ++i)
        for (std::size_t p = 0; p < t->spans[i].pages; ++p)
        {
            const std::size_t page = t->spans[i].first + p;
            const std::uintptr_t pageStart = t->spans[i].start + p * t->pageSize, pageEnd = pageStart + t->pageSize;
            t->segmentFirst[page] = static_cast<std::uint32_t>(segmentCount);
            while (region < count
                   && reinterpret_cast<std::uintptr_t>(t->regions[region].base) + t->regions[region].bytes <= pageStart)
                ++region;
            for (int r = region; r < count; ++r)
            {
                const auto base = reinterpret_cast<std::uintptr_t>(t->regions[r].base);
                if (base >= pageEnd) break;
                const std::uintptr_t from = std::max(base, pageStart),
                                     to = std::min(base + t->regions[r].bytes, pageEnd);
                if (from >= to) continue;
                if (t->segmentCount[page] == UINT8_MAX)
                {
                    // More regions in one page than its count holds.
                    FreeTracker(t);
                    return false;
                }
                t->segments[segmentCount++] = {static_cast<std::uint32_t>(from - pageStart),
                                               static_cast<std::uint32_t>(to - from)};
                ++t->segmentCount[page];
            }
        }

    t->stats.pageSize = t->pageSize;
    t->stats.trackedPages = t->pages;
    __atomic_store_n(&sTracker, t, __ATOMIC_RELEASE);
    return true;
}

std::uint64_t BeginRollbackSnapshots()
{
    Tracker* t = sTracker;
    if (!t || __atomic_load_n(&t->active, __ATOMIC_ACQUIRE)) return 0;
    for (std::size_t page = 0; page < t->pages; ++page)
        CopySegments(*t, page, t->shadow + page * t->pageSize, PageAddress(*t, page));
    std::memset(t->faulted, 0, (t->pages + 7) / 8 * 8);
    const std::uint64_t id = t->latest + 1;
    t->oldest = t->latest = id;
    t->head = t->tail = 0;
    t->saves[id % (t->depth + 1)] = {0, 0};

    // Every page read-only but the hot ones, one call per run of cold pages.
    __atomic_store_n(&t->active, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < t->spanCount; ++i)
    {
        const Span& span = t->spans[i];
        std::size_t p = 0;
        while (p < span.pages)
        {
            if (t->hot[span.first + p])
            {
                ++p;
                continue;
            }
            const std::size_t run = p;
            while (p < span.pages && !t->hot[span.first + p]) ++p;
            Protect(reinterpret_cast<void*>(span.start + run * t->pageSize), (p - run) * t->pageSize, false);
        }
    }
    return id;
}

std::uint64_t SaveRollbackSnapshot()
{
    Tracker* t = sTracker;
    if (!t || !__atomic_load_n(&t->active, __ATOMIC_ACQUIRE)) return 0;
    const std::uint64_t start = NowNs();
    const std::uint64_t id = t->latest + 1;
    while (id - t->oldest > static_cast<std::uint64_t>(t->depth)) DropOldest(*t, id);
    t->saves[id % (t->depth + 1)].first = t->head;
    t->overflow = false;
    std::uint64_t saved = 0;

    // Hot pages: save the changed ones, cool down the idle ones.
    std::size_t kept = 0;
    for (std::size_t h = 0; h < t->hotCount; ++h)
    {
        const std::uint32_t page = t->hotList[h];
        std::uint8_t* live = PageAddress(*t, page);
        if (Differs(*t, page, live))
        {
            PushUndo(*t, page, id);
            CopySegments(*t, page, t->shadow + page * t->pageSize, live);
            t->idle[page] = 0;
            ++saved;
        }
        else if (++t->idle[page] >= kHotIdleSaves)
        {
            Protect(live, t->pageSize, false);
            t->hot[page] = 0;
            ++t->stats.demoted;
            continue;
        }
        t->hotList[kept++] = page;
    }
    t->hotCount = kept;

    // Cold pages written since the last save: already writable, now hot.
    TakeFaulted(*t, [&](std::size_t page) {
        std::uint8_t* live = PageAddress(*t, page);
        if (Differs(*t, page, live))
        {
            PushUndo(*t, page, id);
            CopySegments(*t, page, t->shadow + page * t->pageSize, live);
            ++saved;
        }
        if (!t->hot[page]) MakeHot(*t, page);
        ++t->stats.promoted;
    });

    t->saves[id % (t->depth + 1)].end = t->head;
    t->latest = id;
    if (t->overflow)
    {
        t->oldest = id;
        t->tail = t->head;
    }
    auto& st = t->stats;
    ++st.saves;
    Record(NowNs() - start, st.saveNs, st.maxSaveNs);
    Record(saved, st.savedPages, st.maxSavedPages);
    st.hotPages = t->hotCount;
    return id;
}

bool RestoreRollbackSnapshot(std::uint64_t id)
{
    Tracker* t = sTracker;
    if (!t || !__atomic_load_n(&t->active, __ATOMIC_ACQUIRE) || id < t->oldest || id > t->latest) return false;
    const std::uint64_t start = NowNs();
    std::uint64_t restored = 0;

    // Back to the last save: what changed since is in the hot or faulted pages.
    for (std::size_t h = 0; h < t->hotCount; ++h)
    {
        const std::uint32_t page = t->hotList[h];
        std::uint8_t* live = PageAddress(*t, page);
        if (!Differs(*t, page, live)) continue;
        CopySegments(*t, page, live, t->shadow + page * t->pageSize);
        ++restored;
    }
    TakeFaulted(*t, [&](std::size_t page) {
        CopySegments(*t, page, PageAddress(*t, page), t->shadow + page * t->pageSize);
        if (!t->hot[page]) MakeHot(*t, page);
        ++t->stats.promoted;
        ++restored;
    });

    // Then each later save undone, newest first.
    for (std::uint64_t save = t->latest; save > id; --save)
    {
        const SaveRecord& record = t->saves[save % (t->depth + 1)];
        for (std::uint64_t slot = record.end; slot-- > record.first;)
        {
            const std::size_t index = slot % t->historyPages;
            const std::uint32_t page = t->historyPage[index];
            std::uint8_t* live = PageAddress(*t, page);
            if (!t->hot[page])
            {
                Protect(live, t->pageSize, true);
                MakeHot(*t, page);
            }
            const std::uint8_t* before = t->history + index * t->pageSize;
            CopySegments(*t, page, live, before);
            CopySegments(*t, page, t->shadow + page * t->pageSize, before);
            ++restored;
        }
    }
    if (id < t->latest) t->head = t->saves[(id + 1) % (t->depth + 1)].first;
    t->latest = id;

    auto& st = t->stats;
    ++st.restores;
    Record(NowNs() - start, st.restoreNs, st.maxRestoreNs);
    Record(restored, st.restoredPages, st.maxRestoredPages);
    st.hotPages = t->hotCount;
    return true;
}

std::uint64_t OldestRollbackSnapshot()
{
    return sTracker ? sTracker->oldest : 0;
}

void EndRollbackSnapshots()
{
    Tracker* t = sTracker;
    if (!t || !__atomic_load_n(&t->active, __ATOMIC_ACQUIRE)) return;
    // Writable first: a fault still on its way finds the tracker active.
    for (int i = 0; i < t->spanCount; ++i)
        Protect(reinterpret_cast<void*>(t->spans[i].start), t->spans[i].pages * t->pageSize, true);
    __atomic_store_n(&t->active, 0, __ATOMIC_RELEASE);
    TakeFaulted(*t, [&](std::size_t page) {
        if (!t->hot[page]) MakeHot(*t, page);
    });
}

std::size_t FindUntrackedRollbackWrites(std::uintptr_t* first)
{
    const Tracker* t = sTracker;
    if (!t || !__atomic_load_n(&t->active, __ATOMIC_ACQUIRE)) return 0;
    std::size_t found = 0;
    for (std::size_t page = 0; page < t->pages; ++page)
    {
        if (t->hot[page] || __atomic_load_n(&t->faulted[page], __ATOMIC_ACQUIRE)) continue;
        std::uint8_t* live = PageAddress(*t, page);
        if (!Differs(*t, page, live)) continue;
        if (!found && first) *first = reinterpret_cast<std::uintptr_t>(live);
        ++found;
    }
    return found;
}

void VisitRollbackChanges(void (*visit)(std::uintptr_t address, std::size_t bytes, void* context), void* context)
{
    const Tracker* t = sTracker;
    if (!t || !__atomic_load_n(&t->active, __ATOMIC_ACQUIRE)) return;
    auto scan = [&](std::size_t page) {
        const std::uint8_t* live = PageAddress(*t, page);
        const std::uint8_t* saved = t->shadow + page * t->pageSize;
        for (std::uint32_t s = 0; s < t->segmentCount[page]; ++s)
        {
            const Segment& segment = t->segments[t->segmentFirst[page] + s];
            const std::uint32_t end = segment.offset + segment.bytes;
            std::uint32_t i = segment.offset;
            while (i < end)
            {
                if (live[i] == saved[i])
                {
                    ++i;
                    continue;
                }
                const std::uint32_t run = i;
                while (i < end && live[i] != saved[i]) ++i;
                visit(reinterpret_cast<std::uintptr_t>(live + run), i - run, context);
            }
        }
    };
    for (std::size_t h = 0; h < t->hotCount; ++h)
        if (Differs(*t, t->hotList[h], PageAddress(*t, t->hotList[h]))) scan(t->hotList[h]);
    for (std::size_t page = 0; page < t->pages; ++page)
        if (!t->hot[page] && __atomic_load_n(&t->faulted[page], __ATOMIC_ACQUIRE)) scan(page);
}

const RollbackSnapshotStats& RollbackSnapshotStatistics()
{
    static const RollbackSnapshotStats none{};
    return sTracker ? sTracker->stats : none;
}
}
