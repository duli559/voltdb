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

#include "plannodes/windowfunctionnode.h"
#include "execution/ProgressMonitorProxy.h"
#include <sstream>
namespace voltdb {

/**
 * A WindowAggregate is used to calculate one kind of
 * window function.  The input will look like a sequence
 * of rows which matches this regular expression:
 *    (F(l*n)*)(P(l*n)*)*E
 * where F is the first row, P is a row in a new partition
 * by group, l is a row in the last order by group, n
 * is a row in a new order by group and E is the end.
 * The event E occurs after all rows have been processed.
 * Each new partition by group is a new order by group as
 * well.
 *
 * For each row in this pattern:
 *   F:  advance(exprs, tuple, true)
 *   l:  advance(exprs, tuple, false)
 *   n:  advance(exprs, tuple, true)
 *   P:  resetAgg();
 *       advance(exprs, tuple, true);
 *   E:  finalize(type);
 */
class WindowAggregate {
public:
    virtual ~WindowAggregate() {

    }
    void* operator new(size_t size, Pool& memoryPool) { return memoryPool.allocate(size); }
    void operator delete(void*, Pool& memoryPool) { /* NOOP -- on alloc error unroll nothing */ }
    void operator delete(void*) { /* NOOP -- deallocate wholesale with pool */ }

    virtual void advance(const AbstractPlanNode::OwningExpressionVector &val,
                         const TableTuple &nextTuple,
                         bool newOrderByGroup) = 0;

    virtual void advanceToOrderByEdge(TableWindow *window) {
        // Do Nothing Here.
    }

    virtual NValue finalize(ValueType type)
    {
        m_value.castAs(type);
        return m_value;
    }

    virtual void resetAgg()
    {
        m_value.setNull();
    }
protected:
    NValue m_value;
};

/**
 * This class is fancification of a C array of pointers to
 * WindowAgg.  It has a pass through tuple and at the end has a
 * bunch of pointers to WindowAggregate objects.  These latter
 * calculate the actual aggregate values.
 *
 * Don't define anything which needs virtuality here, or awful things will happen.
 */
class WindowAggregateRow {
public:
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
 * Dense rank is the easiest.  We just count
 * the number of times the order by expression values
 * change.
 */
class DenseRankAgg : public WindowAggregate {
public:
    DenseRankAgg()
      : m_rank(0) {
    }

    virtual ~DenseRankAgg() {}
    virtual void advance(const AbstractPlanNode::OwningExpressionVector &val,
                         const TableTuple &nextTuple,
                         bool newOrderByGroup) {
        assert(val.size() == 0);
        if (newOrderByGroup) {
            m_rank += 1;
        }
    }
    virtual NValue finalize(ValueType type) {
        return ValueFactory::getBigIntValue(m_rank);
    }
    virtual void resetAgg() {
        WindowAggregate::resetAgg();
        m_rank = 0;
    }
protected:
    int m_rank;
};

/**
 * Rank is like dense rank, but we remember how
 * many rows there are between order by expression
 * changes.
 */
class RankAgg : public DenseRankAgg {
public:
    RankAgg()
    : DenseRankAgg(),
      m_numOrderByPeers(1) {}
    ~RankAgg() {}
    void advance(const AbstractPlanNode::OwningExpressionVector & val,
                 const TableTuple &nextTuple,
                 bool newOrderByGroup) {
       assert(val.size() == 0);
       if (newOrderByGroup) {
           m_rank += m_numOrderByPeers;
           m_numOrderByPeers = 1;
       } else {
           m_numOrderByPeers += 1;
       }
    }

    void resetAgg() {
        DenseRankAgg::resetAgg();
        m_numOrderByPeers = 1;
    }
private:
    long m_numOrderByPeers;
};

struct TableWindow {
    TableWindow(Table *tbl)
        : m_trailingEdge(tbl->iterator()),
          m_middleEdge(tbl->iterator()),
          m_orderByEdge(tbl->iterator()),
          m_leadingEdge(tbl->iterator()) {}
    TableIterator m_trailingEdge;
    TableIterator m_middleEdge;
    TableIterator m_orderByEdge;
    TableIterator m_leadingEdge;
    std::string debug() {
        std::stringstream stream;
        stream << "Table Window: ["
                << m_trailingEdge.getLocation() << ", "
                << m_middleEdge.getLocation() << ", "
                << m_orderByEdge.getLocation() << ", "
                << m_leadingEdge.getLocation() << "]\n";
        return stream.str();
    }
};

/*
 * Create an instance of a window aggregator for the specified aggregate type
 * and "distinct" flag.  The object is allocated from the provided memory pool.
 */
inline WindowAggregate* getWindowedAggInstance(Pool& memoryPool, ExpressionType agg_type, bool isDistinct)
{
    switch (agg_type) {
    case EXPRESSION_TYPE_AGGREGATE_WINDOWED_RANK:
        return new (memoryPool) RankAgg();
    case EXPRESSION_TYPE_AGGREGATE_WINDOWED_DENSE_RANK:
        return new (memoryPool) DenseRankAgg();
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
        aggs[ii] = getWindowedAggInstance(m_memoryPool, m_aggTypes[ii], m_distinctAggs[ii]);
    }
}

inline void WindowFunctionExecutor::advanceAggsToOrderByEdge(TableWindow *window) {
    WindowAggregate** aggs = m_aggregateRow->getAggregates();
    for (int ii = 0; ii < m_aggTypes.size(); ii++) {
        aggs[ii]->advanceToOrderByEdge(window);
    }
}
inline void WindowFunctionExecutor::advanceAggs(const TableTuple& tuple,
                                                bool newOrderByGroup)
{
    m_aggregateRow->recordPassThroughTuple(tuple);
    WindowAggregate **aggs = m_aggregateRow->getAggregates();
    for (int ii = 0; ii < m_aggTypes.size(); ii++) {
        const AbstractPlanNode::OwningExpressionVector &inputExprs
            = getAggregateInputExpressions()[ii];
        aggs[ii]->advance(inputExprs, tuple, newOrderByGroup);
    }
}

/*
 *
 * Helper method responsible for inserting the results of the
 * aggregation into a new tuple in the output table as well as passing
 * through any additional columns from the input table.
 */
inline void WindowFunctionExecutor::insertOutputTuple(WindowAggregateRow* winFunRow)
{
    TableTuple& tempTuple = m_tmpOutputTable->tempTuple();

    // We copy the aggregate values into the output tuple,
    // then the passthrough columns.
    WindowAggregate** aggs = winFunRow->getAggregates();
    for (int ii = 0; ii < getAggregateCount(); ii++) {
        NValue result = aggs[ii]->finalize(tempTuple.getSchema()->columnType(ii));
        tempTuple.setNValue(ii, result);
    }

    VOLT_TRACE("Setting passthrough columns");
    size_t tupleSize = tempTuple.sizeInValues();
    for (int ii = getAggregateCount(); ii < tupleSize; ii += 1) {
        AbstractExpression *expr = m_outputColumnExpressions[ii];
        tempTuple.setNValue(ii, expr->eval(&(winFunRow->getPassThroughTuple())));
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
    initWorkingTupleStorage();
    // Input table
    Table * input_table = m_abstractNode->getInputTable();
    assert(input_table);
    VOLT_TRACE("WindowFunctionExecutor: input table\n%s", input_table->debug().c_str());

    m_inputSchema = input_table->schema();
    assert(m_inputSchema);

    boost::scoped_ptr<TableWindow>  scoped_window(new TableWindow(input_table));
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

    /*
     * The first row is slightly different.  We don't want to
     * compare the the first row to anything.  See findLeadingEdge
     * and findOrderByEdge for more details.
     */
    TableTuple nextTuple(m_inputSchema);
    bool firstRow = true;
    while (findLeadingEdge(window, firstRow)) {
        VOLT_TRACE("LeadingEdge: %s", window->debug().c_str());
        m_aggregateRow->resetAggs();
        while (findOrderByEdge(window, firstRow)) {
            VOLT_TRACE("OrderByEdge: %s", window->debug().c_str());
            // This may not be necessary.
            firstRow = false;
            advanceAggsToOrderByEdge(window);
            bool firstOrderByGroupRow = true;
            while (window->m_middleEdge != window->m_orderByEdge) {
                window->m_middleEdge.next(nextTuple);
                m_pmp->countdownProgress();
                advanceAggs(nextTuple, firstOrderByGroupRow);
                firstOrderByGroupRow = false;
                insertOutputTuple(m_aggregateRow);
            }
        }
    }
    p_execute_finish();
    VOLT_TRACE("WindowFunctionExecutor: finalizing..");

    cleanupInputTempTable(input_table);
    VOLT_TRACE("WindowFunctionExecutor::p_execute(end)\n");
    return true;
}

bool WindowFunctionExecutor::findLeadingEdge(TableWindow *window, bool firstRow) {
#ifdef VOLT_TRACE_ENABLED
    int rowCount = 0;
#endif
    TableTuple nextTuple(m_inputSchema);

    assert(window->m_middleEdge == window->m_leadingEdge);
    window->m_trailingEdge = window->m_leadingEdge;
    if ( ! window->m_leadingEdge.hasNext()) {
        // Advance all the iterators to the leading edge.
        window->m_middleEdge = window->m_orderByEdge = window->m_leadingEdge;
        VOLT_TRACE("findLeadingEdge: 0 Rows");
        return false;
    }
    do {
        if (window->m_leadingEdge.next(nextTuple)) {
            /*
             * The method initPartitionByKeyTuple
             * sets the inprogress partition by key to the result
             * of evaluating the partition by values.  If this is
             * the first tuple it sets the last partition by key to
             * undefined.  If this is *not* the first tuple it sets
             * the last tuple to the last tuple value.  So for
             * the first tuple we cannot compare, but for
             * subsequent tuples the comparison always has the
             * right values.
             */
            initPartitionByKeyTuple(nextTuple);
            if (!firstRow
                    && compareTuples(getInProgressPartitionByKeyTuple(),
                                     getLastPartitionByKeyTuple()) != 0) {
                break;
            }
            firstRow = false;
        } else {
            break;
        }
        rowCount += 1;
    } while (true);
    VOLT_TRACE("findLeadingEdge: %d Rows", rowCount);
    return true;
}

bool WindowFunctionExecutor::findOrderByEdge(TableWindow *window, bool firstGroup) {
#ifdef VOLT_TRACE_ENABLED
    int rowCount = 0;
#endif
    TableTuple nextTuple(m_inputSchema);

    assert(window->m_middleEdge == window->m_orderByEdge);
    if (window->m_orderByEdge == window->m_leadingEdge) {
        VOLT_TRACE("findOrderByEdge: %d Rows", rowCount);
        return false;
    }
    do {
        if (window->m_orderByEdge != window->m_leadingEdge) {
            window->m_orderByEdge.next(nextTuple);
            initOrderByKeyTuple(nextTuple);
            if (!firstGroup
                    && compareTuples(getInProgressOrderByKeyTuple(),
                                     getLastOrderByKeyTuple()) != 0) {
                break;
            }
            firstGroup = false;
        } else {
            break;
        }
        rowCount += 1;
    } while (true);
    VOLT_TRACE("findOrderByEdge: %d Rows", rowCount);
    return true;
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


void WindowFunctionExecutor::p_execute_finish() {
    /*
     * The working tuples should not be null.
     */
    assert( ! getInProgressPartitionByKeyTuple().isNullTuple());
    assert( ! getInProgressOrderByKeyTuple().isNullTuple());
    assert( ! getLastPartitionByKeyTuple().isNullTuple());
    assert( ! getLastOrderByKeyTuple().isNullTuple());
    getInProgressPartitionByKeyTuple().move(NULL);
    getInProgressOrderByKeyTuple().move(NULL);
    getLastPartitionByKeyTuple().move(NULL);
    getLastOrderByKeyTuple().move(NULL);
    /*
     * The working tuples have just been set to null.
     */
    assert( getInProgressPartitionByKeyTuple().isNullTuple());
    assert( getInProgressOrderByKeyTuple().isNullTuple());
    assert( getLastPartitionByKeyTuple().isNullTuple());
    assert( getLastOrderByKeyTuple().isNullTuple());
    m_memoryPool.purge();
}

void WindowFunctionExecutor::initWorkingTupleStorage() {
    assert( getInProgressPartitionByKeyTuple().isNullTuple());
    assert( getInProgressOrderByKeyTuple().isNullTuple());
    assert( getLastPartitionByKeyTuple().isNullTuple());
    assert( getLastOrderByKeyTuple().isNullTuple());

    m_inProgressPartitionByKeyStorage.init(m_partitionByKeySchema, &m_memoryPool);
    m_lastPartitionByKeyStorage.init(m_partitionByKeySchema, &m_memoryPool);

    m_lastOrderByKeyStorage.init(m_orderByKeySchema, &m_memoryPool);
    m_inProgressOrderByKeyStorage.init(m_orderByKeySchema, &m_memoryPool);

    m_inProgressPartitionByKeyStorage.allocateActiveTuple();
    m_lastPartitionByKeyStorage.allocateActiveTuple();

    m_inProgressOrderByKeyStorage.allocateActiveTuple();
    m_lastOrderByKeyStorage.allocateActiveTuple();

    assert( ! getInProgressPartitionByKeyTuple().isNullTuple());
    assert( ! getInProgressOrderByKeyTuple().isNullTuple());
    assert( ! getLastPartitionByKeyTuple().isNullTuple());
    assert( ! getLastOrderByKeyTuple().isNullTuple());

}
} /* namespace voltdb */
