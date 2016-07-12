/* This file is part of VoltDB.
 * Copyright (C) 2008-2016 VoltDB Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "common/executorcontext.hpp"

#include "common/debuglog.h"
#include "executors/abstractexecutor.h"
#include "storage/AbstractDRTupleStream.h"
#include "storage/persistenttable.h"
#include "plannodes/insertnode.h"

#include "boost/foreach.hpp"

#include "expressions/functionexpression.h" // Really for datefunctions and its dependencies.

#include <pthread.h>
#ifdef LINUX
#include <malloc.h>
#endif // LINUX

using namespace std;

namespace voltdb {

pthread_mutex_t sharedEngineMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t sharedEngineCondition;
SharedEngineLocalsType enginesByPartitionId;
EngineLocals mpEngineLocals;
std::atomic<int32_t> globalTxnStartCountdownLatch(0);
int32_t globalTxnEndCountdownLatch = 0;
int32_t SITES_PER_HOST = -1;
AbstractExecutor * mpExecutor = NULL;

static pthread_key_t static_key;
static pthread_once_t static_keyOnce = PTHREAD_ONCE_INIT;

/**
 * This function will initiate global settings and create thread key once per process.
 * */
static void globalInitOrCreateOncePerProcess() {
#ifdef LINUX
    // We ran into an issue where memory wasn't being returned to the
    // operating system (and thus reducing RSS) when freeing. See
    // ENG-891 for some info. It seems that some code we use somewhere
    // (maybe JVM, but who knows) calls mallopt and changes some of
    // the tuning parameters. At the risk of making that software
    // angry, the following code resets the tunable parameters to
    // their default values.

    // Note: The parameters and default values come from looking at
    // the glibc 2.5 source, which I is the version that shipps
    // with redhat/centos 5. The code seems to also be effective on
    // newer versions of glibc (tested againsts 2.12.1).

    mallopt(M_MXFAST, 128);                 // DEFAULT_MXFAST
    // note that DEFAULT_MXFAST was increased to 128 for 64-bit systems
    // sometime between glibc 2.5 and glibc 2.12.1
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);  // DEFAULT_TRIM_THRESHOLD
    mallopt(M_TOP_PAD, 0);                  // DEFAULT_TOP_PAD
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);  // DEFAULT_MMAP_THRESHOLD
    mallopt(M_MMAP_MAX, 65536);             // DEFAULT_MMAP_MAX
    mallopt(M_CHECK_ACTION, 3);             // DEFAULT_CHECK_ACTION
#endif // LINUX
    // Be explicit about running in the standard C locale for now.
    std::locale::global(std::locale("C"));
    setenv("TZ", "UTC", 0); // set timezone as "UTC" in EE level

    (void)pthread_key_create(&static_key, NULL);
}

ExecutorContext::ExecutorContext(int64_t siteId,
                CatalogId partitionId,
                UndoQuantum *undoQuantum,
                Topend* topend,
                Pool* tempStringPool,
                NValueArray* params,
                VoltDBEngine* engine,
                std::string hostname,
                CatalogId hostId,
                AbstractDRTupleStream *drStream,
                AbstractDRTupleStream *drReplicatedStream,
                CatalogId drClusterId) :
    m_topEnd(topend),
    m_tempStringPool(tempStringPool),
    m_undoQuantum(undoQuantum),
    m_staticParams(params),
    m_executorsMap(),
    m_drStream(drStream),
    m_drReplicatedStream(drReplicatedStream),
    m_engine(engine),
    m_txnId(0),
    m_spHandle(0),
    m_lastCommittedSpHandle(0),
    m_siteId(siteId),
    m_partitionId(partitionId),
    m_hostname(hostname),
    m_hostId(hostId),
    m_drClusterId(drClusterId)
{
    (void)pthread_once(&static_keyOnce, globalInitOrCreateOncePerProcess);
    bindToThread();
}

ExecutorContext::~ExecutorContext() {
    // currently does not own any of its pointers

    // ... or none, now that the one is going away.
    VOLT_DEBUG("De-installing EC(%ld)", (long)this);

    pthread_setspecific(static_key, NULL);
}

void ExecutorContext::assignThreadLocals(EngineLocals& mapping)
{
    pthread_setspecific(static_key, mapping.context);
    ThreadLocalPool::assignThreadLocals(mapping);
}

void ExecutorContext::bindToThread()
{
    pthread_setspecific(static_key, this);
    VOLT_DEBUG("Installing EC(%ld)", (long)this);
}


ExecutorContext* ExecutorContext::getExecutorContext() {
    (void)pthread_once(&static_keyOnce, globalInitOrCreateOncePerProcess);
    return static_cast<ExecutorContext*>(pthread_getspecific(static_key));
}

Table* ExecutorContext::executeExecutors(int subqueryId)
{
    const std::vector<AbstractExecutor*>& executorList = getExecutors(subqueryId);
    return executeExecutors(executorList, subqueryId);
}

Table* ExecutorContext::executeExecutors(const std::vector<AbstractExecutor*>& executorList,
                                         int subqueryId)
{
    // Walk through the list and execute each plannode.
    // The query planner guarantees that for a given plannode,
    // all of its children are positioned before it in this list,
    // therefore dependency tracking is not needed here.
    size_t ttl = executorList.size();
    int ctr = 0;

    EngineLocals* ourEngineLocals = &enginesByPartitionId[m_partitionId];
    bool needsReleaseLock = false;
    try {
        BOOST_FOREACH (AbstractExecutor *executor, executorList) {
            assert(executor);

            if (executor->getPlanNode()->getPlanNodeType() == PLAN_NODE_TYPE_INSERT) {
                InsertPlanNode* node = dynamic_cast<InsertPlanNode*>(executor->getPlanNode());
                assert(node);
                Table* targetTable = node->getTargetTable();
                PersistentTable *persistentTarget = dynamic_cast<PersistentTable*>(targetTable);
                if (persistentTarget != NULL && persistentTarget->isReplicatedTable()) {
                    if (mpEngineLocals.context == this) {
                        mpExecutor = executor;
                    }
                    if (VoltDBEngine::countDownGlobalTxnStartCount()) {
                        ExecutorContext::assignThreadLocals(mpEngineLocals);
                        needsReleaseLock = true;
                        // Call the execute method to actually perform whatever action
                        // it is that the node is supposed to do...
                        if (!mpExecutor->execute(*m_staticParams)) {
                            throw SerializableEEException(VOLT_EE_EXCEPTION_TYPE_EEEXCEPTION,
                               "Unspecified execution error detected");
                        }
                        ++ctr;
                        mpExecutor = NULL;
                        needsReleaseLock = false;
                        globalTxnStartCountdownLatch = SITES_PER_HOST;
                        // Assign the correct pool back to this thread
                        ExecutorContext::assignThreadLocals(*ourEngineLocals);
                        VoltDBEngine::signalLastSiteFinished();
                    } else {
                        VoltDBEngine::waitForLastSiteFinished();
                    }
                } else {
                    // Call the execute method to actually perform whatever action
                    // it is that the node is supposed to do...
                    if (!executor->execute(*m_staticParams)) {
                        throw SerializableEEException(VOLT_EE_EXCEPTION_TYPE_EEEXCEPTION,
                            "Unspecified execution error detected");
                    }
                    ++ctr;
                }
            } else {
                // Call the execute method to actually perform whatever action
                // it is that the node is supposed to do...
                if (!executor->execute(*m_staticParams)) {
                    throw SerializableEEException(VOLT_EE_EXCEPTION_TYPE_EEEXCEPTION,
                        "Unspecified execution error detected");
                }
                ++ctr;
            }
        }
    } catch (const SerializableEEException &e) {
        if (needsReleaseLock) {
            globalTxnStartCountdownLatch = SITES_PER_HOST;
            // Assign the correct pool back to this thread
            ExecutorContext::assignThreadLocals(*ourEngineLocals);
            VoltDBEngine::signalLastSiteFinished();
        }

        // Clean up any tempTables when the plan finishes abnormally.
        // This needs to be the caller's responsibility for normal returns because
        // the caller may want to first examine the final output table.
        cleanupAllExecutors();
        // Normally, each executor cleans its memory pool as it finishes execution,
        // but in the case of a throw, it may not have had the chance.
        // So, clean up all the memory pools now.
        //TODO: This code singles out inline nodes for cleanup.
        // Is that because the currently active (memory pooling) non-inline
        // executor always cleans itself up before throwing???
        // But if an active executor can be that smart, an active executor with
        // (potential) inline children could also be smart enough to clean up
        // after its inline children, and this post-processing would not be needed.
        BOOST_FOREACH (AbstractExecutor *executor, executorList) {
            assert (executor);
            AbstractPlanNode * node = executor->getPlanNode();
            std::map<PlanNodeType, AbstractPlanNode*>::iterator it;
            std::map<PlanNodeType, AbstractPlanNode*> inlineNodes = node->getInlinePlanNodes();
            for (it = inlineNodes.begin(); it != inlineNodes.end(); it++ ) {
                AbstractPlanNode *inlineNode = it->second;
                inlineNode->getExecutor()->cleanupMemoryPool();
            }
        }

        if (subqueryId == 0) {
            VOLT_TRACE("The Executor's execution at position '%d' failed", ctr);
        } else {
            VOLT_TRACE("The Executor's execution at position '%d' in subquery %d failed", ctr, subqueryId);
        }
        throw;
    }
    return executorList[ttl-1]->getPlanNode()->getOutputTable();
}

Table* ExecutorContext::getSubqueryOutputTable(int subqueryId) const
{
    const std::vector<AbstractExecutor*>& executorList = getExecutors(subqueryId);
    assert(!executorList.empty());
    return executorList.back()->getPlanNode()->getOutputTable();
}

void ExecutorContext::cleanupAllExecutors()
{
    typedef std::map<int, std::vector<AbstractExecutor*>* >::value_type MapEntry;
    BOOST_FOREACH(MapEntry& entry, *m_executorsMap) {
        int subqueryId = entry.first;
        cleanupExecutorsForSubquery(subqueryId);
    }

    // Clear any cached results from executed subqueries
    m_subqueryContextMap.clear();
}

void ExecutorContext::cleanupExecutorsForSubquery(const std::vector<AbstractExecutor*>& executorList) const {
    BOOST_FOREACH (AbstractExecutor *executor, executorList) {
        assert(executor);
        executor->cleanupTempOutputTable();
    }
}

void ExecutorContext::cleanupExecutorsForSubquery(int subqueryId) const
{
    const std::vector<AbstractExecutor*>& executorList = getExecutors(subqueryId);
    cleanupExecutorsForSubquery(executorList);
}

bool ExecutorContext::allOutputTempTablesAreEmpty() const {
    typedef std::map<int, std::vector<AbstractExecutor*>* >::value_type MapEntry;
    BOOST_FOREACH (MapEntry &entry, *m_executorsMap) {
        BOOST_FOREACH(AbstractExecutor* executor, *(entry.second)) {
            if (! executor->outputTempTableIsEmpty()) {
                return false;
            }
        }
    }
    return true;
}

void ExecutorContext::setDrStream(AbstractDRTupleStream *drStream) {
    assert (m_drStream != NULL);
    assert (drStream != NULL);
    assert (m_drStream->m_committedSequenceNumber >= drStream->m_committedSequenceNumber);
    int64_t lastCommittedSpHandle = std::max(m_lastCommittedSpHandle, drStream->m_openSpHandle);
    m_drStream->periodicFlush(-1L, lastCommittedSpHandle);
    int64_t oldSeqNum = m_drStream->m_committedSequenceNumber;
    m_drStream = drStream;
    m_drStream->setLastCommittedSequenceNumber(oldSeqNum);
}

void ExecutorContext::setDrReplicatedStream(AbstractDRTupleStream *drReplicatedStream) {
    assert (m_drReplicatedStream != NULL);
    assert (drReplicatedStream != NULL);
    assert (m_drReplicatedStream->m_committedSequenceNumber >= drReplicatedStream->m_committedSequenceNumber);
    int64_t lastCommittedSpHandle = std::max(m_lastCommittedSpHandle, drReplicatedStream->m_openSpHandle);
    m_drReplicatedStream->periodicFlush(-1L, lastCommittedSpHandle);
    int64_t oldSeqNum = m_drReplicatedStream->m_committedSequenceNumber;
    m_drReplicatedStream = drReplicatedStream;
    m_drReplicatedStream->setLastCommittedSequenceNumber(oldSeqNum);
}

} // end namespace voltdb
