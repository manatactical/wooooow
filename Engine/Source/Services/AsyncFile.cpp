#include <cstddef>
#include <cstdint>
#include "Engine/Source/Services/AsyncFile.h"
#include "Engine/Source/Base/Prop.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Event/PerfLog.h"
#if defined(WOWDECOMP_MODERN_ASSETS)
#include "Engine/Source/ModernAssets/ModernAssetsLog.h"
#endif
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

// STATIC: 0x00AC3438
static const char* s_asyncQueueNames[3] = { "Disk Queue", "Net Geometry Queue", "Net Texture Queue" };

// STATIC: 0x00AC3444
static uint32_t s_handlerTimeout = 100;

// NOT IN BINARY. Wall-clock budget for the poll-handler phase of AsyncFileReadPollHandler. Left at 0
// (uncapped, original behaviour) after measuring: bounding the drain only moved the cost into repeated
// passes and did not remove it.
static uint32_t s_pollHandlerBudgetMs = 0;

// NOT IN BINARY. Budget for AsyncFileReadWaitAll. That function spins until every outstanding read has
// landed, which is correct under a loading screen and wrong during play: a run measured 4 waits totalling
// 2301 ms with 374 reads queued. With a budget it drains for that long and returns, and the caller's own
// retry (map load runs it repeatedly) finishes the rest. 0 restores the original block-until-empty.
//
// Left at 0 (uncapped / original) after measuring: capping it moved the cost into repeated calls and made
// the worst case worse (751 -> 1024 ms), so the budget machinery stays but is not exercised.
static uint32_t s_waitAllBudgetMs = 0;

uint32_t AsyncFileReadGetWaitAllBudget() {
    return s_waitAllBudgetMs;
}

// STATIC: 0x00AC3448
static TSList<CAsyncQueue, TSGetLink<CAsyncQueue> > s_asyncQueueList;

// STATIC: 0x00AC3454
static TSList<CAsyncThread, TSGetLink<CAsyncThread> > s_asyncThreadList;

namespace AsyncFileRead {
    // STATIC: 0x00AC3460
    TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> s_asyncFileReadFreeList;
}

// STATIC: 0x00AC346C
static TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> s_asyncFileReadPostList;

// STATIC: 0x00B4A1EC
static int32_t s_queueLocked;

// STATIC: 0x00B4A1F0
static void (*s_ingameStartCallback)();

// STATIC: 0x00B4A1F4
static void (*s_ingameProgressCallback)(float, int32_t);

// STATIC: 0x00B4A1F8
static int32_t s_progressCount;

// STATIC: 0x00B4A1FC
static void* s_asyncProgressCallbackParams;

// STATIC: 0x00B4A200
static void (*s_asyncProgressCallback)(float, void*);

// STATIC: 0x00B4A204
static CAsyncObject* s_asyncWaitObject;

// STATIC: 0x00B4A208
static HPROPCONTEXT s_propContext;

// STATIC: 0x00B4A20C
//
// NOT IN BINARY size. The original is a fixed 3-slot array and only ever fills slot 0 outside streaming
// mode, so every async read on a non-trial client is served by exactly one worker thread. That is a 2010
// spinning-disk assumption: one queue saturates an SSD about as well as one thread saturates 16 cores.
// The disk queue (slot 0) is now a ring of kMaxDiskQueues workers and the last two slots stay reserved for
// the net queues the streaming path uses. The array size is derived from the ring size rather than being a
// second, independently maintained number: slots [0, kMaxDiskQueues) are disk, then two net slots.
constexpr int32_t kMaxDiskQueues = 8;
static CAsyncQueue* s_asyncQueues[kMaxDiskQueues + 2];

// NOT IN BINARY. How many worker threads serve the disk queue. 0 keeps the original single worker.
static uint32_t s_diskQueueCount = 0;
static uint32_t s_diskQueueNext;

// NOT IN BINARY. See AsyncFile.h.
void AsyncFileReadSetDiskQueueCount(uint32_t count) {
    if (count > kMaxDiskQueues) {
        count = kMaxDiskQueues;
    }

    s_diskQueueCount = count;
}

uint32_t AsyncFileReadGetDiskQueueCount() {
    return s_diskQueueCount;
}

// NOT IN BINARY. See AsyncFile.h.
uint32_t AsyncFileReadGetMaxDiskQueueCount() {
    return static_cast<uint32_t>(kMaxDiskQueues);
}

// STATIC: 0x00B4A218
static uint32_t s_threadSleep;

namespace AsyncFileRead {
    // STATIC: 0x00B4A21C
    SEvent s_shutdownEvent(1, 0);
}

// STATIC: 0x00B4A220
static TSGrowableArray<void (*)()> s_asyncPollHandlers;

// STATIC: 0x00B4A230
static TSGrowableArray<int32_t (*)()> s_asyncStatusHandlers;

// STATIC: 0x00B4A240
SCritSect g_queueLock;

// VERIFY: 5a8ab5a9 auto
// OFFSET: 0x004B50A0
int32_t CAsyncObject::IsPending() {
    return !this->m_isCurrent && !this->m_isRead && !this->m_isProcessed;
}

// VERIFY: 6305e7e3 auto
// OFFSET: 0x004B9910
void AsyncFileReadSetProgressCallback(void (*callback)(float, void*), void* param) {
    s_asyncProgressCallback = callback;
    s_asyncProgressCallbackParams = param;
}

// VERIFY: fe99c992 auto
// OFFSET: 0x004B9930
void AsyncFileReadIngameProgressCallback(void (*startCallback)(), void (*progressCallback)(float, int32_t)) {
    s_ingameStartCallback = startCallback;
    s_ingameProgressCallback = progressCallback;
}

// VERIFY: 08e4e19e auto
// OFFSET: 0x004B9950
void AsyncFile::EnterQueueLock() {
    s_queueLocked = 1;
    g_queueLock.Enter();
}

// VERIFY: ee2b2778 auto
// OFFSET: 0x004B9970
void AsyncFile::LeaveQueueLock() {
    g_queueLock.Leave();
    s_queueLocked = 0;
}

// VERIFY: ca9687a9 manual
// OFFSET: 0x004B9AB0
void AsyncFileReadUnloadPostList() {
    CAsyncObject* object;

    while ((object = s_asyncFileReadPostList.Head()) != nullptr) {
        object->m_link.Unlink();

        if (object->m_userPostUnloadCallback) {
            reinterpret_cast<void (*)(void*)>(object->m_userPostUnloadCallback)(object->m_userArg);
        }
    }
}

// VERIFY: acd49bbd manual
// OFFSET: 0x004B9B20
int32_t AsyncFileReadPollHandler(const void* data, void* param) {
    uint32_t start = OsGetAsyncTimeMs();

    // NOT IN BINARY. Sum of the postload callbacks this pass ran, and the pass's own wall time. The two
    // are logged together at the end: if the pass took much longer than the callbacks it ran, the cost is
    // in the loop (lock contention, unlinking) rather than in anything a callback touched. Earlier attempts
    // to attribute this from inside a single callback produced numbers that could not be reconciled.
    uint32_t callbackTotalMs = 0;
    uint32_t callbacksRun = 0;

    while (true) {
        g_queueLock.Enter();

        CAsyncObject* object = s_asyncFileReadPostList.Head();

        if (!object) {
            g_queueLock.Leave();
            break;
        }

        object->m_link.Unlink();

        if (object == s_asyncWaitObject) {
            s_asyncWaitObject = nullptr;
        }

        object->m_isProcessed = 1;
        g_queueLock.Leave();

        // NOT IN BINARY. The postload callback is where the real cost sits: it uploads a texture, attaches a
        // model's skin, or builds a WMO/ADT group, and a bulk load runs hundreds of them back to back. The
        // earlier attempt to bound the drain only moved that cost around; timing it per call says which
        // callback the milliseconds actually belong to. m_size names the size, the file names the asset.
        const uint32_t callbackStart = OsGetAsyncTimeMs();

        reinterpret_cast<void (*)(void*)>(object->m_userPostloadCallback)(object->m_userArg);

        const uint32_t callbackMs = OsGetAsyncTimeMs() - callbackStart;

        callbackTotalMs += callbackMs;
        callbacksRun++;

        if (callbackMs >= 2) {
            char label[96];
            char name[256];

            name[0] = '\0';

            if (object->m_file) {
                SFile::GetActualFileName(object->m_file, name, sizeof(name));
            }

            const char* leaf = name;

            for (const char* c = name; *c; c++) {
                if (*c == '\\' || *c == '/') {
                    leaf = c + 1;
                }
            }

            SStrPrintf(label, sizeof(label), "%s %uK", leaf[0] ? leaf : "?", object->m_size >> 10);

            // The callback is often the one that releases the file, so the name is frequently already gone
            // by now. The size still separates the classes: a few-KB blob is a texture or a skin, a
            // multi-MB one is a model body.
            PerfLogSpanDetail("postload", callbackMs, label);
        }

        PerfLogCount("postload-calls", 1);
        s_progressCount--;

        if (OsGetAsyncTimeMs() - start > s_handlerTimeout) {
            break;
        }
    }

    for (uint32_t i = 0; i < s_asyncPollHandlers.Count(); i++) {
        const uint32_t handlerStart = OsGetAsyncTimeMs();

        s_asyncPollHandlers[i]();

        // NOT IN BINARY. A poll handler can be the whole hitch on its own -- the texture drain reported
        // 1007 ms in one tick. Reporting all of them together, not just the ones with a known name, is
        // what tells us whether the next suspect is even worth naming.
        const uint32_t handlerMs = OsGetAsyncTimeMs() - handlerStart;

        if (handlerMs >= 2) {
            char label[32];
            SStrPrintf(label, sizeof(label), "handler%u", i);
            PerfLogSpanDetail("poll-handler", handlerMs, label);
        }
    }

    // NOT IN BINARY. The reconciliation line: total pass time against the callbacks that ran inside it.
    // "gap" is time in the pass that no callback accounted for -- lock waits, unlinking, GetActualFileName.
    // If gap is large, the callbacks were never the problem and no amount of timing inside them will find
    // it. Emitted only when the pass is long enough to matter, so a normal tick writes nothing.
    const uint32_t passMs = OsGetAsyncTimeMs() - start;

    if (passMs >= 8) {
        char label[48];
        SStrPrintf(label, sizeof(label), "n=%u gap=%u", callbacksRun,
                   passMs > callbackTotalMs ? passMs - callbackTotalMs : 0);
        PerfLogSpanDetail("poll-pass", passMs, label);
    }

    return 1;
}

// VERIFY: 132a0c31 manual
// OFFSET: 0x004B9C60
void AsyncFileReadAddPollHandler(void (*handler)()) {
    for (uint32_t i = 0; i < s_asyncPollHandlers.Count(); i++) {
        if (s_asyncPollHandlers[i] == handler) {
            return;
        }
    }

    s_asyncPollHandlers.New(handler);
}

// VERIFY: e6dbc58a manual
// OFFSET: 0x004B9D20
void AsyncFileReadAddStatusHandler(int32_t (*handler)()) {
    for (uint32_t i = 0; i < s_asyncStatusHandlers.Count(); i++) {
        if (s_asyncStatusHandlers[i] == handler) {
            return;
        }
    }

    s_asyncStatusHandlers.New(handler);
}

// VERIFY: 75b60811 manual
// OFFSET: 0x004B9DE0
void AsyncFileReadDestroyObject(CAsyncObject* object) {
    g_queueLock.Enter();

    if (object->m_isCurrent) {
        DebugLog("AsyncFileReadDestroyObject()");
        g_queueLock.Leave();

        // NOT IN BINARY. This spins until the async thread clears m_isCurrent, which it does only after
        // re-taking the queue lock and linking the object to the post list -- so the caller can spend the
        // whole handoff asleep here. A measured postload callback spent 1188 ms of a 1190 ms total in this
        // function. Timed and counted, because a spin here is invisible in every other phase timer.
        // NOT IN BINARY. This spins until the async thread clears m_isCurrent, which it does only after
        // re-taking the queue lock and linking the object to the post list -- so the caller can sleep here
        // for the whole handoff. The spin is also bounded: an unbounded wait on the frame thread is a hang
        // waiting to happen if the worker is stalled, and the caller must not be the thing that deadlocks
        // the whole client. After the bound it proceeds anyway; the object is unlinked and recycled either
        // way, which is the same state the unbounded loop leaves it in.
        const uint32_t spinStart = OsGetAsyncTimeMs();
        uint32_t spins = 0;

        while (object->m_isCurrent && OsGetAsyncTimeMs() - spinStart < 2000) {
            OsSleep(1);
            spins++;
        }

        if (spins) {
            char label[32];
            SStrPrintf(label, sizeof(label), "spins=%u", spins);
            PerfLogSpanDetail("destroy-spin", OsGetAsyncTimeMs() - spinStart, label);
        }

        g_queueLock.Enter();
    }

    object->m_link.Unlink();

    // NOT IN BINARY. Timed, but the span is emitted AFTER the queue lock is released below. Writing the
    // log opens and closes a file, and doing that while holding g_queueLock blocks every async worker
    // thread on the lock while the frame thread waits on a disk write -- a deadlock, and the cause of a
    // reproduced hang. No PerfLog call may sit inside this critical section.
    uint32_t closeMs = 0;
    bool closed = false;

    if (object->m_file) {
        const uint32_t closeStart = OsGetAsyncTimeMs();

        SFileCloseFile(object->m_file);

        closeMs = OsGetAsyncTimeMs() - closeStart;
        closed = true;
    }

    AsyncFileRead::s_asyncFileReadFreeList.LinkToHead(object);
    g_queueLock.Leave();

    if (closed) {
        PerfLogSpan("file-close", closeMs);
    }
}

// VERIFY: 37b4ae03 auto
// OFFSET: 0x004B9E90
void DefaultAsyncObjectCleanupCallback(CAsyncObject* object) {
    AsyncFileReadDestroyObject(object);
}

// VERIFY: 3f12de16 manual
// OFFSET: 0x004B9EA0
int32_t AsyncFile::Cancel(CAsyncObject* object, void (*callback)(CAsyncObject*)) {
    g_queueLock.Enter();

    if (object->m_isCurrent) {
        object->m_userArg = object;
        object->m_userPostUnloadCallback = reinterpret_cast<void*>(callback ? callback : DefaultAsyncObjectCleanupCallback);
        object->m_userPostloadCallback = object->m_userPostUnloadCallback;
        g_queueLock.Leave();

        return 0;
    }

    SFileCloseFile(object->m_file);
    object->m_link.Unlink();
    AsyncFileRead::s_asyncFileReadFreeList.LinkToHead(object);
    g_queueLock.Leave();

    return 1;
}

// VERIFY: 8a117547 manual
// OFFSET: 0x004B9F40
int32_t NetQueuePromoteLocalFiles(const void* data, void* param) {
    // STATIC: 0x00B4A268
    static uint32_t s_lastPromoteTime;

    uint32_t time = OsGetAsyncTimeMs();

    if (time - s_lastPromoteTime >= 250) {
        s_lastPromoteTime = time;
        g_queueLock.Enter();

        for (int32_t i = 1; i < 3; i++) {
            const uint32_t netBase = s_diskQueueCount ? s_diskQueueCount : 1;

            if (!s_asyncQueues[netBase + i - 1]) {
                continue;
            }

            CAsyncObject* object = s_asyncQueues[netBase + i - 1]->m_readList.Head();

            while (!s_asyncQueues[netBase + i - 1]->m_readList.IsEnd(object)) {
                CAsyncObject* next = s_asyncQueues[netBase + i - 1]->m_readList.RawNext(object);

                if (SFile::FileIsLocal(object->m_file, 6) == 1) {
                    s_asyncQueues[0]->m_readList.LinkToTail(object);
                    object->m_queue = s_asyncQueues[0];
                }

                object = next;
            }
        }

        g_queueLock.Leave();
    }

    return 1;
}

// VERIFY: d0d6eee1 manual
// OFFSET: 0x004BA060
void AsyncFileReadWait(CAsyncObject* object) {
    // STATIC: 0x00B4A26C
    static int32_t s_waiting;

    if (s_waiting) {
        SErrDisplayAppFatal("AsyncFileReadWait(): s_waiting != FALSE");
    }

    s_waiting++;

    // NOT IN BINARY. AsyncFileReadWait blocks the calling (frame) thread and pumps its own queue until the
    // read lands, so every millisecond here is a millisecond of frame time. Measured around the whole wait
    // and named with the file, because a 250 ms hitch with no file name says nothing about where it came from.
    const uint32_t waitStart = OsGetAsyncTimeMs();

    g_queueLock.Enter();

    if (object->m_isProcessed) {
        g_queueLock.Leave();

        // Balances the increment above. Without it the next wait trips the s_waiting guard and kills the
        // client with an app-fatal, so this is a correctness fix and not just accounting.
        s_waiting--;

        return;
    }


    s_asyncWaitObject = object;

    if (!object->m_isCurrent && !object->m_isRead) {
        object->m_link.Unlink();
        object->m_queue->m_readList.LinkToHead(object);
    }

    g_queueLock.Leave();

    if (SFile::IsStreamingMode()) {
        SFile::SetFilePriority(object->m_file, 1, 0);
    }

    char name[256];
    SFile::GetActualFileName(object->m_file, name, sizeof(name));
    DebugLog("AsyncFileReadWait():%s\n", name);

    if (s_ingameStartCallback) {
        s_ingameStartCallback();
    }

    do {
        if (s_ingameProgressCallback) {
            s_ingameProgressCallback(0.0f, 0);
        }

        AsyncFileReadPollHandler(nullptr, nullptr);

        if (!s_asyncWaitObject) {
            break;
        }

        OsSleep(1);
    } while (true);

    PerfLogSpanDetail("async-wait", OsGetAsyncTimeMs() - waitStart, name);

    s_waiting--;
}

// VERIFY: 82068864 manual
// OFFSET: 0x004BA170
CAsyncObject* AsyncFileReadAllocObject() {
    g_queueLock.Enter();

    CAsyncObject* object = AsyncFileRead::s_asyncFileReadFreeList.Head();

    if (!object) {
        object = AsyncFileRead::s_asyncFileReadFreeList.NewNode(1, 0, 0);
    }

    object->m_link.Unlink();
    g_queueLock.Leave();

    object->m_file = nullptr;
    object->m_buffer = nullptr;
    object->m_size = 0;
    object->m_userArg = nullptr;
    object->m_userPostloadCallback = nullptr;
    object->m_userPostUnloadCallback = nullptr;
    object->m_queue = nullptr;
    object->m_isProcessed = 0;
    object->m_isRead = 0;
    object->m_isCurrent = 0;
    object->m_char24 = 0;
    object->m_char25 = 0;
    object->m_queuedFrame = 0;
    object->m_priority = 126;

    return object;
}

// VERIFY: d372c124 manual
// OFFSET: 0x004BA310
CAsyncQueue::~CAsyncQueue() {
}

// VERIFY: 69785110 manual
// OFFSET: 0x004BA3D0
void AsyncFileReadLinkObject(CAsyncObject* object, int32_t urgent) {
    if (!object->m_queue) {
        return;
    }

    object->m_link.Unlink();

    for (CAsyncObject* other = object->m_queue->m_readList.Head(); !object->m_queue->m_readList.IsEnd(other); other = object->m_queue->m_readList.RawNext(other)) {
        if (object->m_priority <= other->m_priority && (urgent || object->m_priority != other->m_priority)) {
            object->m_queue->m_readList.LinkNode(object, 2, other);
            object->m_char25 = 0;
            return;
        }
    }

    object->m_queue->m_readList.LinkToTail(object);
    object->m_char25 = 0;
}

// VERIFY: c818c689 manual
// OFFSET: 0x004BA530
void AsyncFileReadLinkStaleObject(CAsyncObject* object, int32_t urgent) {
    object->m_link.Unlink();

    for (CAsyncObject* other = object->m_queue->m_staleList.Head(); !object->m_queue->m_staleList.IsEnd(other); other = object->m_queue->m_staleList.RawNext(other)) {
        if (object->m_priority <= other->m_priority && (urgent || object->m_priority != other->m_priority)) {
            object->m_queue->m_staleList.LinkNode(object, 2, other);
            object->m_char25 = 1;
            return;
        }
    }

    object->m_queue->m_staleList.LinkToTail(object);
    object->m_char25 = 1;
}

// VERIFY: 28f905c7 manual
// OFFSET: 0x004BA680
uint32_t STORMAPI AsyncFileReadThread(void* param) {
    CAsyncThread* thread = static_cast<CAsyncThread*>(param);
    CAsyncQueue* queue = thread->m_queue;
    PropSelectContext(s_propContext);

    while (AsyncFileRead::s_shutdownEvent.Wait(0)) {
        uint32_t count = 0;

        while (true) {
            g_queueLock.Enter();

            CAsyncObject* object = queue->m_readList.Head();

            if (object) {
                if (queue->m_frameLimited && g_theGxDevicePtr->m_frameCount - object->m_queuedFrame > 3) {
                    AsyncFileReadLinkStaleObject(object, 1);
                    g_queueLock.Leave();
                    continue;
                }
            } else {
                object = queue->m_staleList.Head();

                if (!object) {
                    break;
                }
            }

            object->m_link.Unlink();
            object->m_queue = nullptr;
            object->m_isCurrent = 1;
            thread->m_currentObject = object;
            g_queueLock.Leave();

            for (int32_t retries = 10; retries; ) {
                if (SFile::IsStreamingMode() && object->m_file->m_type) {
                    SFile::MakeFileLocal(object->m_file, (object->m_priority > 127) + 1, true);
                }

                if (SFileReadFile(object->m_file, object->m_buffer, object->m_size, nullptr, nullptr, 0)) {
                    break;
                }

                if (!--retries) {
                    char name[512];
                    SFile::GetActualFileName(object->m_file, name, sizeof(name));
                    char error[512];
                    SErrGetErrorStr(SErrGetLastError(), error, sizeof(error));
                    DebugLog("AsyncFileReadThread() failed reading %s: %s", name, error);
                }
            }

            g_queueLock.Enter();
            s_asyncFileReadPostList.LinkToTail(object);
            thread->m_currentObject = nullptr;
            object->m_isCurrent = 0;
            object->m_isRead = 1;
            g_queueLock.Leave();

            if (s_threadSleep && ++count == s_threadSleep) {
                OsSleep(1);
                count = 0;
            }
        }

        g_queueLock.Leave();
        OsSleep(1);
    }

    return 0;
}

// VERIFY: 1d3b9267 manual
// OFFSET: 0x004BA8E0
CAsyncQueue* AsyncFileReadCreateQueue() {
    return s_asyncQueueList.NewNode(2, 0, 0);
}

// VERIFY: 3de6d82d auto
// OFFSET: 0x004BA980
void AsyncFileReadCreateThread(CAsyncQueue* queue, const char* name) {
    CAsyncThread* thread = s_asyncThreadList.NewNode(2, 0, 0);
    thread->m_queue = queue;
    thread->m_currentObject = nullptr;
    SThread::Create(AsyncFileReadThread, thread, thread->m_thread, const_cast<char*>(name), 0);
}

// VERIFY: e94485f6 auto
// OFFSET: 0x004BAA40
void AsyncFileReadInitialize(uint32_t threadSleep, uint32_t handlerTimeout) {
    s_threadSleep = threadSleep;
    s_handlerTimeout = handlerTimeout;

    if (threadSleep > 100) {
        s_threadSleep = 100;
    }

    if (handlerTimeout < 20) {
        s_handlerTimeout = 20;
    }

    EventRegisterEx(EVENT_ID_POLL, AsyncFileReadPollHandler, nullptr, 0.0f);

    if (SFile::IsStreamingMode()) {
        EventRegisterEx(EVENT_ID_IDLE, NetQueuePromoteLocalFiles, nullptr, -1.0f);
    }

    s_asyncWaitObject = nullptr;
    s_asyncProgressCallback = nullptr;
    s_asyncProgressCallbackParams = nullptr;
    s_ingameProgressCallback = nullptr;
    s_ingameStartCallback = nullptr;
    s_propContext = PropGetSelectedContext();
    AsyncFileRead::s_shutdownEvent.Reset();

    // NOT IN BINARY. Slot 0 is the disk queue and is now a ring of workers; slots 1 and 2 keep their
    // original meaning (net geometry / net texture) and are only created in streaming mode. With no
    // configured count this creates exactly the one worker the original did.
    const int32_t diskQueues = s_diskQueueCount ? static_cast<int32_t>(s_diskQueueCount) : 1;

    for (int32_t i = 0; i < diskQueues; i++) {
        s_asyncQueues[i] = AsyncFileReadCreateQueue();
        AsyncFileReadCreateThread(s_asyncQueues[i], s_asyncQueueNames[0]);
    }

    if (SFile::IsStreamingMode()) {
        const int32_t netBase = diskQueues;

        s_asyncQueues[netBase] = AsyncFileReadCreateQueue();
        AsyncFileReadCreateThread(s_asyncQueues[netBase], s_asyncQueueNames[1]);

        s_asyncQueues[netBase + 1] = AsyncFileReadCreateQueue();
        AsyncFileReadCreateThread(s_asyncQueues[netBase + 1], s_asyncQueueNames[2]);
        s_asyncQueues[netBase + 1]->m_frameLimited = 1;
    }

    s_queueLocked = 0;
}

// VERIFY: 35423985 auto
// OFFSET: 0x004BAB50
void AsyncFileReadObject(CAsyncObject* object, int32_t urgent) {
    // NOT IN BINARY. Spread reads across the disk workers instead of stacking every one on slot 0. The
    // ring is only used when more than one worker exists, so the single-worker build behaves exactly as
    // before, including the frame stamping below.
    CAsyncQueue* queue = s_asyncQueues[0];

    if (s_diskQueueCount > 1) {
        queue = s_asyncQueues[s_diskQueueNext % s_diskQueueCount];
        s_diskQueueNext++;
    }

    if (SFile::IsStreamingMode()) {
        int32_t local = SFile::FileIsLocal(object->m_file, 6);

        if (!local || local == 2) {
            const uint32_t netBase = s_diskQueueCount ? s_diskQueueCount : 1;

            object->m_char24 = 1;
            queue = object->m_priority > 127 ? s_asyncQueues[netBase + 1] : s_asyncQueues[netBase];
        }
    }

    g_queueLock.Enter();
    object->m_queue = queue;

    if (object == s_asyncWaitObject) {
        object->m_priority = object->m_priority > 127 ? 128 : 0;
        object->m_queuedFrame = g_theGxDevicePtr->m_frameCount;
        queue->m_readList.LinkToHead(object);
        object->m_char25 = 0;
    } else if (queue->m_frameLimited) {
        AsyncFileReadLinkStaleObject(object, urgent);
    } else {
        AsyncFileReadLinkObject(object, urgent);
    }

    g_queueLock.Leave();

    if (SFile::IsStreamingMode()) {
        SFile::SetFilePriority(object->m_file, 0, 0);
    }
}

// VERIFY: 2f496060 auto
// OFFSET: 0x004BAC20
void AsyncFileReadIncreasePriority(CAsyncObject* object) {
    object->m_queuedFrame = g_theGxDevicePtr->m_frameCount;

    if (object->m_char25) {
        AsyncFileReadLinkObject(object, 1);
    }
}

// VERIFY: 3aa538d7 manual
// OFFSET: 0x004BAC50
void AsyncFileReadDestroy() {
    for (CAsyncThread* thread = s_asyncThreadList.Head(); !s_asyncThreadList.IsEnd(thread); thread = s_asyncThreadList.RawNext(thread)) {
        AsyncFileRead::s_shutdownEvent.Set();
        thread->m_thread.Wait(0xFFFFFFFF);
    }

    AsyncFileReadUnloadPostList();
    AsyncFileRead::s_asyncFileReadFreeList.Clear();

    for (CAsyncQueue* queue = s_asyncQueueList.Head(); !s_asyncQueueList.IsEnd(queue); queue = s_asyncQueueList.RawNext(queue)) {
    }

    s_asyncQueueList.Clear();

    for (uint32_t i = 0; i < sizeof(s_asyncQueues) / sizeof(s_asyncQueues[0]); i++) {
        s_asyncQueues[i] = nullptr;
    }

    s_diskQueueNext = 0;

    for (CAsyncThread* thread = s_asyncThreadList.Head(); !s_asyncThreadList.IsEnd(thread); thread = s_asyncThreadList.RawNext(thread)) {
    }

    s_asyncThreadList.Clear();
    s_asyncPollHandlers.SetCount(0);
    s_asyncStatusHandlers.SetCount(0);
    s_asyncProgressCallback = nullptr;
    s_asyncProgressCallbackParams = nullptr;
    s_ingameProgressCallback = nullptr;
    s_ingameStartCallback = nullptr;

    if (SFile::IsStreamingMode()) {
        EventUnregisterEx(EVENT_ID_IDLE, NetQueuePromoteLocalFiles, nullptr, 2);
    }

    EventUnregisterEx(EVENT_ID_POLL, AsyncFileReadPollHandler, nullptr, 0xFFFFFFFF);
}

// VERIFY: e7566958 auto
// OFFSET: 0x004BAD80
bool AsyncFileReadIsReading() {
    g_queueLock.Enter();

    bool reading = false;

    for (CAsyncThread* thread = s_asyncThreadList.Head(); !s_asyncThreadList.IsEnd(thread); thread = s_asyncThreadList.RawNext(thread)) {
        if (thread->m_currentObject) {
            reading = true;
        }
    }

    for (CAsyncQueue* queue = s_asyncQueueList.Head(); !s_asyncQueueList.IsEnd(queue); queue = s_asyncQueueList.RawNext(queue)) {
        if (queue->m_readList.Head()) {
            reading = true;
        }
    }

    if (s_asyncFileReadPostList.Head()) {
        reading = true;
    }

    g_queueLock.Leave();

    return reading;
}

// NOT IN BINARY. Walks every queue and every committed object and writes one line per stuck read: which
// queue, what state it is in, how big it is and what file it belongs to. Called when a wait has run far
// longer than any legitimate load, so the log says which asset is holding the client instead of the
// loading screen simply never finishing and leaving no trace.
void AsyncFileReadDumpPending(const char* where, uint32_t waitedMs, int32_t total, int32_t remaining) {
    MODERN_ASSETS_LOG_WARN("async: '%s' has been waiting %u ms (total=%d remaining=%d) -- dumping queues",
                           where, waitedMs, total, remaining);

    g_queueLock.Enter();

    for (uint32_t q = 0; q < sizeof(s_asyncQueues) / sizeof(s_asyncQueues[0]); q++) {
        CAsyncQueue* queue = s_asyncQueues[q];

        if (!queue) {
            continue;
        }

        uint32_t reading = 0;
        uint32_t stale = 0;

        for (CAsyncObject* object = queue->m_readList.Head(); object; object = queue->m_readList.Next(object)) {
            reading++;
        }

        for (CAsyncObject* object = queue->m_staleList.Head(); object; object = queue->m_staleList.Next(object)) {
            stale++;
        }

        MODERN_ASSETS_LOG_WARN("async:   queue %u readList=%u staleList=%u frameLimited=%d",
                               q, reading, stale, queue->m_frameLimited);

        uint32_t shown = 0;

        for (CAsyncObject* object = queue->m_readList.Head(); object && shown < 8; object = queue->m_readList.Next(object), shown++) {
            char name[260];

            name[0] = '\0';

            if (object->m_file) {
                SFile::GetActualFileName(object->m_file, name, sizeof(name));
            }

            MODERN_ASSETS_LOG_WARN("async:     read '%s' size=%u current=%d isRead=%d processed=%d prio=%u",
                                   name[0] ? name : "?", object->m_size, object->m_isCurrent,
                                   object->m_isRead, object->m_isProcessed, object->m_priority);
        }
    }

    uint32_t postCount = 0;

    for (CAsyncObject* object = s_asyncFileReadPostList.Head(); object; object = s_asyncFileReadPostList.Next(object)) {
        postCount++;
    }

    MODERN_ASSETS_LOG_WARN("async:   postList=%u waitObject=%s", postCount, s_asyncWaitObject ? "set" : "null");

    g_queueLock.Leave();
}

// VERIFY: 3e612f72 auto
// OFFSET: 0x004BAE10
void AsyncFileReadWaitAll() {
    DebugLog("AsyncFileReadWaitAll() Enter\n");

    // NOT IN BINARY. This spins until every outstanding read finishes, with OsSleep(1) per pass, so it is
    // frame time on whichever thread called it -- map loading calls it repeatedly. Untimed, a 300 ms
    // "moving fast" hitch looks like an idle frame with no phase to blame, because the engine is asleep
    // here rather than drawing. The count is what makes it useful: the wait itself is not the cost, the
    // how-many-and-how-big queued behind it is.
    const uint32_t waitStart = OsGetAsyncTimeMs();

    int32_t total = 0;

    for (uint32_t i = 0; i < s_asyncStatusHandlers.Count(); i++) {
        total += s_asyncStatusHandlers[i]();
    }

    s_progressCount = total;

    while (AsyncFileReadIsReading()) {
        AsyncFileReadPollHandler(nullptr, nullptr);

        // NOT IN BINARY. With a budget configured this returns while reads are still outstanding rather
        // than blocking the frame until the queue is empty. Callers that need the data wait again on a
        // later frame; that is what keeps a 374-read burst from costing a whole second of frame time.
        if (s_waitAllBudgetMs && OsGetAsyncTimeMs() - waitStart > s_waitAllBudgetMs) {
            break;
        }

        // NOT IN BINARY. A load that never finishes writes nothing at all, which is what makes a stuck
        // loading screen undiagnosable. After 10 s this reports what the queue still holds -- which reads,
        // their state and their file names -- then keeps waiting, so the log names the fault without
        // changing whether the load eventually completes.
        const uint32_t waitedMs = OsGetAsyncTimeMs() - waitStart;

        if (waitedMs > 10000 && (waitedMs / 10000) != ((waitedMs - 1) / 10000)) {
            AsyncFileReadDumpPending("async-wait-all", waitedMs, total, s_progressCount);
        }

        if (s_asyncProgressCallback) {
            float progress = 1.0f;

            if (total) {
                progress = static_cast<float>(total - s_progressCount) / total;

                if (progress < 0.0f) {
                    progress = 0.0f;
                } else if (progress > 1.0f) {
                    progress = 1.0f;
                }
            }

            s_asyncProgressCallback(progress, s_asyncProgressCallbackParams);
        }

        OsSleep(1);
    }

    s_asyncProgressCallback = nullptr;

    const uint32_t elapsed = OsGetAsyncTimeMs() - waitStart;

    PerfLogCount("async-wait-all", 1);
    PerfLogSpan("async-wait-all", elapsed);

    // Recorded separately from async-wait: this is the load-time path, not a single foreground texture.
    if (elapsed >= 5) {
        PerfLogCount("async-pending", static_cast<uint32_t>(total));
    }

    DebugLog("AsyncFileReadWaitAll() Leave\n");
}
