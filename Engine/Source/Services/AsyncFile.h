#ifndef ENGINE_SOURCE_SERVICES_ASYNCFILE_H
#define ENGINE_SOURCE_SERVICES_ASYNCFILE_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Services/Types.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Storm/Source/SThread.h"

class CAsyncQueue : public TSLinkedNode<CAsyncQueue> {
    public:
    /* 0008 */ TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> m_readList;
    /* 0014 */ TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> m_staleList;
    /* 0020 */ int32_t m_frameLimited;

    ~CAsyncQueue();
};

class CAsyncThread : public TSLinkedNode<CAsyncThread> {
    public:
    /* 0008 */ SThread m_thread;
    /* 000C */ CAsyncQueue* m_queue;
    /* 0010 */ CAsyncObject* m_currentObject;
};

class AsyncFile {
    public:
    static int32_t Cancel(CAsyncObject* object, void (*callback)(CAsyncObject*));
    static void EnterQueueLock();
    static void LeaveQueueLock();
};

void AsyncFileReadAddPollHandler(void (*handler)());
void AsyncFileReadAddStatusHandler(int32_t (*handler)());
CAsyncObject* AsyncFileReadAllocObject();
CAsyncQueue* AsyncFileReadCreateQueue();
void AsyncFileReadCreateThread(CAsyncQueue* queue, const char* name);
void AsyncFileReadDestroy();
void AsyncFileReadDestroyObject(CAsyncObject* object);
void AsyncFileReadIncreasePriority(CAsyncObject* object);
void AsyncFileReadIngameProgressCallback(void (*startCallback)(), void (*progressCallback)(float, int32_t));
void AsyncFileReadInitialize(uint32_t threadSleep, uint32_t handlerTimeout);

// NOT IN BINARY. Number of worker threads serving the disk queue. The original creates exactly one outside
// streaming mode, which is a spinning-disk assumption: one blocking read at a time. Must be set before
// AsyncFileReadInitialize runs (the threads are created there). 0 keeps the original single worker.
void AsyncFileReadSetDiskQueueCount(uint32_t count);
uint32_t AsyncFileReadGetDiskQueueCount();

// NOT IN BINARY. Frame-time budget for the AsyncFileReadWaitAll drain. Left at 0 (block until empty,
// original behaviour) after measuring: capping it made the worst case worse, because map load simply
// calls it again. Kept as a seam for later work rather than exercised today.
void AsyncFileReadSetWaitAllBudget(uint32_t budgetMs);
uint32_t AsyncFileReadGetWaitAllBudget();
bool AsyncFileReadIsReading();
void AsyncFileReadLinkObject(CAsyncObject* object, int32_t urgent);
void AsyncFileReadLinkStaleObject(CAsyncObject* object, int32_t urgent);
void AsyncFileReadObject(CAsyncObject* object, int32_t urgent);
int32_t AsyncFileReadPollHandler(const void* data, void* param);
void AsyncFileReadSetProgressCallback(void (*callback)(float, void*), void* param);
uint32_t STORMAPI AsyncFileReadThread(void* param);
void AsyncFileReadUnloadPostList();
void AsyncFileReadWait(CAsyncObject* object);
void AsyncFileReadWaitAll();
void DefaultAsyncObjectCleanupCallback(CAsyncObject* object);
int32_t NetQueuePromoteLocalFiles(const void* data, void* param);

#endif
