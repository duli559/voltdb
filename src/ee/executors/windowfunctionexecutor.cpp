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
/*
 * partitionbyexecutor.cpp
 */

#include "executors/windowfunctionexecutor.h"

#include <sstream>
#include <memory>
#include <limits.h>

#include "plannodes/windowfunctionnode.h"
#include "execution/ProgressMonitorProxy.h"
#include "common/ValueFactory.hpp"

namespace voltdb {

/**
 * This class holds all the iterators used when iterating
 * through an input table.  There is one of these each time
 * the executor runs.  Since it contains table iterators
 * which have no default constructor, it really needs to
 * know its input table at construction time.  This is not
 * available when the executor object is constructed, so
 * this needs to be heap allocated.
 */
struct TableWindow {
    TableWindow(Table *tbl)
        : m_middleEdge(tbl->iterator()),
          m_leadingEdge(tbl->iterator()),
          m_groupSize(0) {}
    std::string debug() {
        std::stringstream stream;
        stream << "Table Window: [Middle: "
                << m_middleEdge.getLocation() << ", Leading: "
                << m_leadingEdge.getLocation() << "], "
                << "ssize = " << m_groupSize
                << "\n";
        return stream.str();
    }

    void resetCounts() {
        m_groupSize = 0;
    }
    TableIterator m_middleEdge;
    TableIterator m_leadingEdge;
    /**
     * This is handy for the aggregators.  It's maintained
     * in findOrderByEdge();
     */
    size_t m_groupSize;
};

/**
 * A WindowAggregate is the base class of aggregate calculations.
 * In the algorithm for calculating window function values we are
 * sensitive to some requirements.
 *   <ol>
 *     <li>All aggregates look at each input row in each order by group to
 *         calculate a value at each input row.</li>
 *     <li>For each such input row, some aggregates can use only values
 *         which can be computed before the input row, and some need to know
 *         values after the input row.  For example, RANK and DENSE_RANK
 *         only need to know how many rows precede the input row.  On the
 *         other hand, COUNT(*) needs to know how many rows are in the
 *         order by group of the input row, which includes rows after the
 *         input row.<li>
 *     <li>Some aggregates needs to inspect each row to compute values.
 *         For example, COUNT(E) must evaluate E in each input row in
 *         the order by group and only count those where the evaluation
 *         of E is non-null.</li>
 *   </ol>
 * Since it's expensive to evaluate expressions when they are not used,
 * we want to be able to turn off evaluation when it's not needed.
 */
struct WindowAggregate {
    WindowAggregate()
      : m_needsLookahead(true) {}
    virtual ~WindowAggregate() {

    }
    void* operator new(size_t size, Pool& memoryPool) { return memoryPool.allocate(size); }
    void operator delete(void*, Pool& memoryPool) { /* NOOP -- on alloc error unroll nothing */ }
    void operator delete(void*) { /* NOOP -- deallocate wholesale with pool */ }

    /**
     * Do calculations needed when scanning each row ahead for
     * the end of an order by or partition by group.
     */
    virtual void lookaheadOneRow(TableWindow *window, NValueArray &argValues) {
        ;
    }

    /**
     * Do calculations at the end of a scan of an order by
     * group.
     */
    virtual void lookaheadNextGroup(TableWindow *window) {
        ;
    }

    /**
     * Do calculations to end the group and start the
     * next group.
     */
    virtual void endGroup(TableWindow *window,
                          WindowFunctionExecutor::EdgeType edgeType) {
        ;
    }

    /**
     * Calculate the final value for the output tuple.
     */
    virtual NValue finalize(ValueType type)
    {
        m_value.castAs(type);
        return m_value;
    }

    /**
     * Initialize the aggregate.  This is called at the
     * beginning of each partition by group.
     */
    virtual void resetAgg()
    {
        m_value.setNull();
    }
    NValue m_value;
    bool   m_needsLookahead;
};

/**
 * Dense rank is the easiest.  We just count
 * the number of times the order by expression values
 * change.
 */
class DenseRankAgg : public WindowAggregate {
public:
    DenseRankAgg() {
        m_value = ValueFactory::getBigIntValue(1);
        m_orderByPeerIncrement = m_value;
        m_needsLookahead = false;
    }

    virtual ~DenseRankAgg() {}

    virtual void endGroup(TableWindow *window, WindowFunctionExecutor::EdgeType etype) {
        m_value = m_value.op_add(orderByPeerIncrement());
    }

    virtual void resetAgg() {
        WindowAggregate::resetAgg();
        m_value = ValueFactory::getBigIntValue(1);
        m_orderByPeerIncrement = m_value;
    }

    virtual NValue orderByPeerIncrement() {
        return m_orderByPeerIncrement;
    }
    NValue m_orderByPeerIncrement;
};

/**
 * Rank is like dense rank, but we increment
 * the m_rank by the size of the order by group.
 */
class RankAgg : public DenseRankAgg {
public:
    RankAgg()
    : DenseRankAgg() {
    }
    void lookaheadNextGroup(TableWindow *window) {
        m_orderByPeerIncrement = ValueFactory::getBigIntValue(window->m_groupSize);
    }
    ~RankAgg() {}
};

/**
 * Count is a bit like rank, but we need to contrive
 * to calculate when the argument expression is null,
 * and add the count of non-null rows to the count output
 * before we output the rows.
 */
class CountAgg : public WindowAggregate {
public:
    CountAgg() {
        m_one = ValueFactory::getBigIntValue(1);
    }

    virtual ~CountAgg() {}

    virtual void lookaheadOneRow(TableWindow *window, NValueArray &argVals) {
        /*
         * COUNT(*) has no arguments.  If there are arguments,
         * and the argument value is null, then don't count the row.
         */
        if (argVals.size() == 0 || ! argVals[0].isNull()) {
            m_value = m_value.op_add(m_one);
        }
    }

    virtual void resetAgg() {
        WindowAggregate::resetAgg();
        m_value = ValueFactory::getBigIntValue(0);
    }

    NValue m_one;
};

#if  0
/*
 * The following are examples of other aggregate definitions.
 */
class MinAgg : public WindowAggregate {
public:
    MinAgg() {
        m_value = ValueFactory::getBigIntValue(LONG_MAX);
    }
    /**
     * Calculate the min by looking ahead in the
     * order by group.
     */
    virtual void lookaheadOneRow(TableWindow *window, NValueArray &argVals) {
        assert(argVals.size() == 1);
        if ( ! argVals[0].isNull()) {
            if (argVals[0].op_lessThan(m_value)) {
                m_value = argVals[0];
            }
        }
    }
};

class MaxAgg : public WindowAggregate {
public:
    MaxAgg() {
        m_value = ValueFactory::getBigIntValue(LONG_MIN);
    }
    /**
     * Calculate the min by looking ahead in the
     * order by group.
     */
    virtual void lookaheadOneRow(TableWindow *window, NValueArray &argVals) {
        assert(argVals.size() == 1);
        if ( ! argVals[0].isNull()) {
            if (argVals[0].op_greaterThan(m_value)) {
                m_value = argVals[0];
            }
        }
    }
};

class SumAgg : public WindowAggregate {
public:
    SumAgg() : m_ct(0) {
    }
    /**
     * Calculate the min by looking ahead in the
     * order by group.
     */
    virtual void lookaheadOneRow(TableWindow *window, NValueArray &argVals) {
        assert(argVals.size() == 1);
        if ( ! argVals[0].isNull()) {
            m_value = m_value.op_add(argVals[0]);
        }
    }
};

class AvgAgg : public WindowAggregate {
public:
    AvgAgg()
      : m_count(ValueFactory::getBigIntValue(0)),
        m_one(ValueFactory::getBigIntValue(1)) {
    }
    virtual void lookaheadOneRow(TableWindow *window, NValueArray &argVals) {
        assert(argVals.size() == 1);
        if ( ! argVals[0].isNull() ) {
            m_value = m_value.op_add(argVals[0]);
            m_count = m_count.op_add(m_one);
            m_ct += 1;
        }
    }
    virtual NValue finalize(ValueType type) {
        if (m_ct == 0) {
            // Not sure how to do this really.
            return NValue::getNullValue(VALUE_TYPE_BIGINT);
        }
        return m_value.op_divide(m_count);
    }
private:
    NValue m_count;
    NValue m_one;
    size_t m_ct;
};
#endif

/**
 * This class is fancification of a C array of pointers to
 * WindowAgg.  It has a pass through tuple and at the end has a
 * bunch of pointers to WindowAggregate objects.  These latter
 * calculate the actual aggregate values.
 *
 * Don't define anything which needs virtuality here, or awful things will happen.
 */
struct WindowAggregateRow {
    WindowAggregateRow(const TupleSchema *inputSchema, Pool &pool) {
        m_passThroughStorage.init(inputSchema, &pool);
        m_passThroughStorage.allocateActiveTuple();
    }
    void* operator new(size_t size, Pool& memoryPool, size_t nAggs)
    {
      return memoryPool.allocateZeroes(size + (sizeof(void*) * (nAggs + 1)));
    }
    void operator delete(void*, Pool& memoryPool, size_t nAggs) { /* NOOP -- on alloc error unroll */ }
    void operator delete(void*) { /* NOOP -- deallocate wholesale with pool */ }

    void resetAggs()
    {
        // Stop at the terminating null agg pointer that has been allocated as an extra and ignored since.
        for (int ii = 0; m_aggregates[ii] != NULL; ++ii) {
            m_aggregates[ii]->resetAgg();
        }
    }

    WindowAggregate **getAggregates() {
        return &(m_aggregates[0]);
    }
    void recordPassThroughTuple(const TableTuple &nextTuple) {
        getPassThroughTuple().copy(nextTuple);
    }
    TableTuple &getPassThroughTuple() {
        return m_passThroughStorage;
    }
private:
    PoolBackedTupleStorage  m_passThroughStorage;
    WindowAggregate *m_aggregates[0];
};

WindowFunctionExecutor::~WindowFunctionExecutor() {
    // NULL Safe Operation
    TupleSchema::freeTupleSchema(m_partitionByKeySchema);
    TupleSchema::freeTupleSchema(m_orderByKeySchema);
}

TupleSchema* WindowFunctionExecutor::constructSchemaFromExpressionVector
        (const AbstractPlanNode::OwningExpressionVector &exprs) {
    std::vector<ValueType> columnTypes;
    std::vector<int32_t> columnSizes;
    std::vector<bool> columnAllowNull;
    std::vector<bool> columnInBytes;

    BOOST_FOREACH (AbstractExpression* expr, exprs) {
            columnTypes.push_back(expr->getValueType());
            columnSizes.push_back(expr->getValueSize());
            columnAllowNull.push_back(true);
            columnInBytes.push_back(expr->getInBytes());
    }
    return TupleSchema::createTupleSchema(columnTypes,
                                          columnSizes,
                                          columnAllowNull,
                                          columnInBytes);
}

/**
 * When this function is called, the AbstractExecutor's init function
 * will have set the input tables in the plan node, but nothing else.
 */
bool WindowFunctionExecutor::p_init(AbstractPlanNode *init_node, TempTableLimits *limits) {
    VOLT_TRACE("WindowFunctionExecutor::p_init(start)");
    WindowFunctionPlanNode* node = dynamic_cast<WindowFunctionPlanNode*>(m_abstractNode);
    assert(node);

    if (!node->isInline()) {
        setTempOutputTable(limits);
    }
    /*
     * Initialize the memory pool early, so that we can
     * use it for constructing temp. tuples.
     */
    m_memoryPool.purge();

    assert( getInProgressPartitionByKeyTuple().isNullTuple());
    assert( getInProgressOrderByKeyTuple().isNullTuple());
    assert( getLastPartitionByKeyTuple().isNullTuple());
    assert( getLastOrderByKeyTuple().isNullTuple());

    m_partitionByKeySchema = constructSchemaFromExpressionVector(m_partitionByExpressions);
    m_orderByKeySchema = constructSchemaFromExpressionVector(m_orderByExpressions);

    /*
     * Initialize all the data for partition by and
     * order by storage once and for all.
     */
    VOLT_TRACE("WindowFunctionExecutor::p_init(end)\n");
    return true;
}

/**
 * Create an instance of a window aggregator for the specified aggregate type.
 * The object is allocated from the provided memory pool.
 */
inline WindowAggregate* getWindowedAggInstance(Pool& memoryPool,
                                               ExpressionType agg_type)
{
    switch (agg_type) {
    case EXPRESSION_TYPE_AGGREGATE_WINDOWED_RANK:
        return new (memoryPool) RankAgg();
    case EXPRESSION_TYPE_AGGREGATE_WINDOWED_DENSE_RANK:
        return new (memoryPool) DenseRankAgg();
    case EXPRESSION_TYPE_AGGREGATE_WINDOWED_COUNT:
        return new (memoryPool) CountAgg();
    default:
        {
            char message[128];
            snprintf(message, sizeof(message), "Unknown aggregate type %d", agg_type);
            throw SerializableEEException(VOLT_EE_EXCEPTION_TYPE_EEEXCEPTION, message);
        }
    }
}

/*
 * Create an instance of an aggregate calculator for the specified aggregate type.
 * The object is constructed in memory from the provided memory pool.
 */
inline void WindowFunctionExecutor::initAggInstances()
{
    WindowAggregate** aggs = m_aggregateRow->getAggregates();
    for (int ii = 0; ii < m_aggTypes.size(); ii++) {
        aggs[ii] = getWindowedAggInstance(m_memoryPool,
                                          m_aggTypes[ii]);
    }
}

inline void WindowFunctionExecutor::lookaheadOneRowForAggs(TableWindow *window,
                                                           const TableTuple &tuple) {
    WindowAggregate** aggs = m_aggregateRow->getAggregates();
    for (int ii = 0; ii < m_aggTypes.size(); ii++) {
        if (aggs[ii]->m_needsLookahead) {
            const AbstractPlanNode::OwningExpressionVector &inputExprs
                = getAggregateInputExpressions()[ii];
            NValueArray vals(inputExprs.size());
            for (int idx = 0; idx < inputExprs.size(); idx += 1) {
                vals[idx] = inputExprs[idx]->eval(&tuple);
            }
            aggs[ii]->lookaheadOneRow(window, vals);
        }
    }
}

inline void WindowFunctionExecutor::lookaheadNextGroupForAggs(TableWindow *window) {
    WindowAggregate** aggs = m_aggregateRow->getAggregates();
    for (int ii = 0; ii < m_aggTypes.size(); ii++) {
        aggs[ii]->lookaheadNextGroup(window);
    }
}

inline void WindowFunctionExecutor::endGroupForAggs(TableWindow *window, EdgeType edgeType) {
    WindowAggregate** aggs = m_aggregateRow->getAggregates();
    for (int ii = 0; ii < m_aggTypes.size(); ii++) {
        aggs[ii]->endGroup(window, edgeType);
    }
}

/*
 *
 * Helper method responsible for inserting the results of the
 * aggregation into a new tuple in the output table as well as passing
 * through any additional columns from the input table.
 */
inline void WindowFunctionExecutor::insertOutputTuple()
{
    TableTuple& tempTuple = m_tmpOutputTable->tempTuple();

    // We copy the aggregate values into the output tuple,
    // then the passthrough columns.
    WindowAggregate** aggs = m_aggregateRow->getAggregates();
    for (int ii = 0; ii < getAggregateCount(); ii++) {
        NValue result = aggs[ii]->finalize(tempTuple.getSchema()->columnType(ii));
        tempTuple.setNValue(ii, result);
    }

    VOLT_TRACE("Setting passthrough columns");
    size_t tupleSize = tempTuple.sizeInValues();
    for (int ii = getAggregateCount(); ii < tupleSize; ii += 1) {
        AbstractExpression *expr = m_outputColumnExpressions[ii];
        tempTuple.setNValue(ii, expr->eval(&(m_aggregateRow->getPassThroughTuple())));
    }

    m_tmpOutputTable->insertTempTuple(tempTuple);
    VOLT_TRACE("output_table:\n%s", m_tmpOutputTable->debug().c_str());
}

int WindowFunctionExecutor::compareTuples(const TableTuple &tuple1,
                                          const TableTuple &tuple2) const {
    const TupleSchema *schema = tuple1.getSchema();
    assert (schema == tuple2.getSchema());

    for (int ii = schema->columnCount() - 1; ii >= 0; --ii) {
        int cmp = tuple2.getNValue(ii)
                        .compare(tuple1.getNValue(ii));
        if (cmp != 0) {
            return cmp;
        }
    }
    return 0;

}

/**
 * This RAII class sets a pointer to null when it is goes out of scope.
 * We could use boost::scoped_ptr with a specialized destructor, but it
 * seems more obscure than this simple class.
 */
struct ScopedNullingPointer {
    ScopedNullingPointer(ProgressMonitorProxy * & ptr)
        : m_ptr(ptr) { }

    ~ScopedNullingPointer() {
        if (m_ptr) {
            m_ptr = NULL;
        }
    }
    ProgressMonitorProxy    * & m_ptr;
};

/*
 * This function is called straight from AbstractExecutor::execute,
 * which is called from executeExecutors, which is called from the
 * VoltDBEngine::executePlanFragments.  So, this is really the start
 * of execution for this executor.
 *
 * The executor will already have been initialized by p_init.
 */
bool WindowFunctionExecutor::p_execute(const NValueArray& params) {
    VOLT_TRACE("windowFunctionExecutor::p_execute(start)\n");
    // Input table
    Table * input_table = m_abstractNode->getInputTable();
    assert(input_table);
    VOLT_TRACE("WindowFunctionExecutor: input table\n%s", input_table->debug().c_str());

    m_inputSchema = input_table->schema();
    assert(m_inputSchema);

    /*
     * Do this after setting the m_inputSchema.
     */
    initWorkingTupleStorage();

    boost::shared_ptr<TableWindow>  scoped_window(new TableWindow(input_table));
    TableWindow *window = scoped_window.get();
    ProgressMonitorProxy pmp(m_engine, this);
    /*
     * This will set m_pmp to NULL on return, which avoids
     * a reference to the dangling pointer pmp if something
     * throws.
     */
    ScopedNullingPointer np(m_pmp);
    m_pmp = &pmp;

    m_aggregateRow
        = new (m_memoryPool, m_aggTypes.size())
             WindowAggregateRow(m_inputSchema, m_memoryPool);

    initAggInstances();

    VOLT_TRACE("Beginning: %s", window->debug().c_str());

    TableTuple nextTuple(m_inputSchema);
    for (EdgeType etype = StartOfInput,
                  nextEtype = InvalidEdgeType;
         etype != EndOfInput;
         etype = nextEtype) {
        // Reset the aggregates if this is the
        // start of a partition group.  The start of
        // input is a special form of this.
        if (etype == StartOfInput || etype == StartOfPartitionByGroup) {
            m_aggregateRow->resetAggs();
        }
        // Find the next edge.  This will
        // give the aggs a crack at each row
        // if they want it.
        nextEtype = findNextEdge(window, etype);
        // Let the aggs know the results
        // of the lookahead.
        lookaheadNextGroupForAggs(window);
        // Advance to the end of the current group.
        for (int idx = 0; idx < window->m_groupSize; idx += 1) {
            VOLT_TRACE("MiddleEdge: Window = %s", window->debug().c_str());
            window->m_middleEdge.next(nextTuple);
            m_pmp->countdownProgress();
            m_aggregateRow->recordPassThroughTuple(nextTuple);
            insertOutputTuple();
        }
        endGroupForAggs(window, etype);
        VOLT_TRACE("FirstEdge: %s", window->debug().c_str());
    }
    p_execute_finish(window);
    VOLT_TRACE("WindowFunctionExecutor: finalizing..");

    cleanupInputTempTable(input_table);
    VOLT_TRACE("WindowFunctionExecutor::p_execute(end)\n");
    return true;
}

WindowFunctionExecutor::EdgeType WindowFunctionExecutor::findNextEdge(TableWindow *window,
                                                                      EdgeType     edgeType)
{
    // This is just an alias for the buffered input tuple.
    TableTuple &nextTuple = getBufferedInputTuple();
    VOLT_TRACE("findNextEdge(start): %s", window->debug().c_str());
    /*
     * At the start of the input we need to prime the
     * tuple pairs.
     */
    if (edgeType == StartOfInput) {
        if (window->m_leadingEdge.next(nextTuple)) {
            initPartitionByKeyTuple(nextTuple);
            initOrderByKeyTuple(nextTuple);
            /* First row.  Nothing to compare it with. */
            window->m_groupSize = 1;
            lookaheadOneRowForAggs(window, nextTuple);
        } else {
            /*
             * If there is no first row, then just
             * return false.  The leading edge iterator
             * will never have a next row, so we can
             * ask for its next again and will always get false.
             * We return a zero length group here.
             */
            window->m_groupSize = 0;
            return EndOfInput;
        }
    } else {
        /*
         * We've already got a row, so
         * count it.
         */
        window->m_groupSize = 1;
        lookaheadOneRowForAggs(window, nextTuple);
    }
    do {
        VOLT_TRACE("findNextEdge(loopStart): %s", window->debug().c_str());
        if (window->m_leadingEdge.next(nextTuple)) {
            initPartitionByKeyTuple(nextTuple);
            initOrderByKeyTuple(nextTuple);
            if (compareTuples(getInProgressPartitionByKeyTuple(),
                              getLastPartitionByKeyTuple()) != 0) {
                VOLT_TRACE("findNextEdge(Partition): %s", window->debug().c_str());
                return StartOfPartitionByGroup;
            }
            if (compareTuples(getInProgressOrderByKeyTuple(),
                              getLastOrderByKeyTuple()) != 0) {
                VOLT_TRACE("findNextEdge(Group): %s", window->debug().c_str());
                return StartOfOrderByGroup;
            }
            window->m_groupSize += 1;
            lookaheadOneRowForAggs(window, nextTuple);
            VOLT_TRACE("findNextEdge(loop): %s", window->debug().c_str());
        } else {
            VOLT_TRACE("findNextEdge(EOI): %s", window->debug().c_str());
            return EndOfInput;
        }
    } while (true);
}

void WindowFunctionExecutor::initPartitionByKeyTuple(const TableTuple& nextTuple)
{
    /*
     * The partition by keys should not be null tuples.
     */
    assert( ! getInProgressPartitionByKeyTuple().isNullTuple());
    assert( ! getLastPartitionByKeyTuple().isNullTuple());
    /*
     * Swap the data, so that m_inProgressPartitionByKey
     * gets m_lastPartitionByKey's data and vice versa.
     * This just swaps the data pointers.
     */
    swapPartitionByKeyTupleData();
    /*
     * The partition by keys should still not be null tuples.
     */
    assert( ! getInProgressPartitionByKeyTuple().isNullTuple());
    assert( ! getLastPartitionByKeyTuple().isNullTuple());
    /*
     * Calculate the partition by key values.  Put them in
     * getInProgressPartitionByKeyTuple().
     */
    for (int ii = 0; ii < m_partitionByExpressions.size(); ii++) {
        getInProgressPartitionByKeyTuple().setNValue(ii, m_partitionByExpressions[ii]->eval(&nextTuple));
    }
}

void WindowFunctionExecutor::initOrderByKeyTuple(const TableTuple& nextTuple)
{
    /*
     * The OrderByKey should not be null tuples.
     */
    assert( ! getInProgressOrderByKeyTuple().isNullTuple());
    assert( ! getLastOrderByKeyTuple().isNullTuple());
    /*
     * Swap the data pointers.  No data is moved.
     */
    swapOrderByKeyTupleData();
    /*
     * Still should not be null tuples.
     */
    assert( ! getInProgressOrderByKeyTuple().isNullTuple());
    assert( ! getLastOrderByKeyTuple().isNullTuple());
    /*
     * Calculate the order by key values.
     */
    for (int ii = 0; ii < m_orderByExpressions.size(); ii++) {
        getInProgressOrderByKeyTuple().setNValue(ii, m_orderByExpressions[ii]->eval(&nextTuple));
    }
    /*
     * Still should not be null tuples.
     */
    assert( ! getInProgressOrderByKeyTuple().isNullTuple());
    assert( ! getLastOrderByKeyTuple().isNullTuple());
}

void WindowFunctionExecutor::swapPartitionByKeyTupleData() {
    assert( ! getInProgressPartitionByKeyTuple().isNullTuple());
    assert( ! getLastPartitionByKeyTuple().isNullTuple());
    void* inProgressData = getInProgressPartitionByKeyTuple().address();
    void* nextData = getLastPartitionByKeyTuple().address();
    getInProgressPartitionByKeyTuple().move(nextData);
    getLastPartitionByKeyTuple().move(inProgressData);
    assert( ! getInProgressPartitionByKeyTuple().isNullTuple());
    assert( ! getLastPartitionByKeyTuple().isNullTuple());
}

void WindowFunctionExecutor::swapOrderByKeyTupleData() {
    /*
     * Should not be null tuples.
     */
    assert( ! getInProgressOrderByKeyTuple().isNullTuple());
    assert( ! getLastOrderByKeyTuple().isNullTuple());
    void* inProgressData = getInProgressOrderByKeyTuple().address();
    void* nextData = getLastOrderByKeyTuple().address();
    getInProgressOrderByKeyTuple().move(nextData);
    getLastOrderByKeyTuple().move(inProgressData);
    /*
     * Still should not be null tuples.
     */
    assert( ! getInProgressOrderByKeyTuple().isNullTuple());
    assert( ! getLastOrderByKeyTuple().isNullTuple());
}


void WindowFunctionExecutor::p_execute_finish(TableWindow *window) {
    /*
     * The working tuples should not be null.
     */
    assert( ! getInProgressPartitionByKeyTuple().isNullTuple());
    assert( ! getInProgressOrderByKeyTuple().isNullTuple());
    assert( ! getLastPartitionByKeyTuple().isNullTuple());
    assert( ! getLastOrderByKeyTuple().isNullTuple());
    assert( ! getBufferedInputTuple().isNullTuple());
    getInProgressPartitionByKeyTuple().move(NULL);
    getInProgressOrderByKeyTuple().move(NULL);
    getLastPartitionByKeyTuple().move(NULL);
    getLastOrderByKeyTuple().move(NULL);
    getBufferedInputTuple().move(NULL);
    /*
     * The working tuples have just been set to null.
     */
    assert( getInProgressPartitionByKeyTuple().isNullTuple());
    assert( getInProgressOrderByKeyTuple().isNullTuple());
    assert( getLastPartitionByKeyTuple().isNullTuple());
    assert( getLastOrderByKeyTuple().isNullTuple());
    assert( getBufferedInputTuple().isNullTuple());
    m_memoryPool.purge();
}

void WindowFunctionExecutor::initWorkingTupleStorage() {
    assert( getInProgressPartitionByKeyTuple().isNullTuple());
    assert( getInProgressOrderByKeyTuple().isNullTuple());
    assert( getLastPartitionByKeyTuple().isNullTuple());
    assert( getLastOrderByKeyTuple().isNullTuple());
    assert( getBufferedInputTuple().isNullTuple());

    m_inProgressPartitionByKeyStorage.init(m_partitionByKeySchema, &m_memoryPool);
    m_lastPartitionByKeyStorage.init(m_partitionByKeySchema, &m_memoryPool);

    m_lastOrderByKeyStorage.init(m_orderByKeySchema, &m_memoryPool);
    m_inProgressOrderByKeyStorage.init(m_orderByKeySchema, &m_memoryPool);

    m_bufferedInputStorage.init(m_inputSchema, &m_memoryPool);

    m_inProgressPartitionByKeyStorage.allocateActiveTuple();
    m_lastPartitionByKeyStorage.allocateActiveTuple();

    m_inProgressOrderByKeyStorage.allocateActiveTuple();
    m_lastOrderByKeyStorage.allocateActiveTuple();

    m_bufferedInputStorage.allocateActiveTuple();

    assert( ! getInProgressPartitionByKeyTuple().isNullTuple());
    assert( ! getInProgressOrderByKeyTuple().isNullTuple());
    assert( ! getLastPartitionByKeyTuple().isNullTuple());
    assert( ! getLastOrderByKeyTuple().isNullTuple());
    assert( ! getBufferedInputTuple().isNullTuple());

}
} /* namespace voltdb */
