/* This file is part of VoltDB.
 * Copyright (C) 2008-2016 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
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
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "swaptablesexecutor.h"

#include "plannodes/swaptablesnode.h"
//#include "storage/temptable.h"
#include "storage/persistenttable.h"

using namespace std;
using namespace voltdb;

bool SwapTablesExecutor::p_init(AbstractPlanNode *abstract_node, TempTableLimits* limits)
{
    VOLT_TRACE("init SwapTable Executor");
#ifndef NDEBUG
    SwapTablesPlanNode* node = dynamic_cast<SwapTablesPlanNode*>(m_abstractNode);
    assert(node);
    assert(node->getTargetTable1());
    assert(node->getTargetTable2());
    assert(node->getInputTableCount() == 0);
#endif

    setDMLCountOutputTable(limits);
    return true;
}

bool SwapTablesExecutor::p_execute(const NValueArray &params) {
    // target tables should be persistent tables
    // update target table references from table delegates
    SwapTablesPlanNode* node = static_cast<SwapTablesPlanNode*>(m_abstractNode);
    assert(dynamic_cast<SwapTablesPlanNode*>(m_abstractNode));
    PersistentTable* targetTable1 = node->getTargetTable1();
    assert(targetTable1);
    PersistentTable* targetTable2 = node->getTargetTable2();
    assert(targetTable2);

    int64_t modified_tuples = 0;

    VOLT_TRACE("swap tables %s and %s",
               targetTable1->name().c_str(),
               targetTable2->name().c_str());
    // count the active tuples in both tables as modified
    modified_tuples = targetTable1->visibleTupleCount() +
            targetTable2->visibleTupleCount();

    VOLT_TRACE("Swap Tables: %s with %d active, %d visible, %d allocated"
               " and %s with %d active, %d visible, %d allocated",
               targetTable1->name().c_str(),
               (int)targetTable1->activeTupleCount(),
               (int)targetTable1->visibleTupleCount(),
               (int)targetTable1->allocatedTupleCount(),
               targetTable2->name().c_str(),
               (int)targetTable2->activeTupleCount(),
               (int)targetTable2->visibleTupleCount(),
               (int)targetTable2->allocatedTupleCount());

    // Swap the table catalog delegates and corresponding indexes and views.
    targetTable1->swapTable(targetTable2, m_engine);

    TableTuple& count_tuple = m_tmpOutputTable->tempTuple();
    count_tuple.setNValue(0, ValueFactory::getBigIntValue(modified_tuples));
    // try to put the tuple into the output table
    m_tmpOutputTable->insertTempTuple(count_tuple);
    m_engine->addToTuplesModified(modified_tuples);
    return true;
}
