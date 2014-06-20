/* This file is part of VoltDB.
 * Copyright (C) 2008-2014 VoltDB Inc.
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

#include "projectionexecutor.h"

#include "common/tabletuple.h"
#include "expressions/abstractexpression.h"
#include "expressions/expressionutil.h"
#include "plannodes/projectionnode.h"
#include "storage/tableiterator.h"
#include "storage/temptable.h"

namespace voltdb {

bool ProjectionExecutor::p_init(AbstractPlanNode *abstractNode,
                                TempTableLimits* limits)
{
    VOLT_TRACE("init Projection Executor");
    assert(limits);

    ProjectionPlanNode* node = dynamic_cast<ProjectionPlanNode*>(m_abstractNode);
    assert(node);

    // Create output table based on output schema from the plan
    setTempOutputTable(limits);

    std::vector<AbstractExpression*>& columnExpressions = node->getOutputColumnExpressions();
    int columnCount = (int)columnExpressions.size();

    // initialize local variables
    all_tuple_array_ptr = ExpressionUtil::convertIfAllTupleValues(columnExpressions);
    all_tuple_array = all_tuple_array_ptr.get();
    all_param_array_ptr = ExpressionUtil::convertIfAllParameterValues(columnExpressions);
    all_param_array = all_param_array_ptr.get();

    needs_substitute = new bool[columnCount];
    needs_substitute_ptr.reset(needs_substitute);
    typedef AbstractExpression* ExpRawPtr;
    expression_array = new ExpRawPtr[columnCount];
    expression_array_ptr.reset(expression_array);

    for (int ctr = 0; ctr < columnCount; ctr++) {
        assert(columnExpressions[ctr] != NULL);
        VOLT_TRACE("OutputColumnExpressions [%d]: %s", ctr, columnExpressions[ctr]->debug(true).c_str());
        expression_array_ptr[ctr] = columnExpressions[ctr];
        needs_substitute_ptr[ctr] = columnExpressions[ctr]->hasParameter();
    }

    return true;
}

bool ProjectionExecutor::p_execute(const NValueArray &params) {
    ProjectionPlanNode* node = dynamic_cast<ProjectionPlanNode*>(m_abstractNode);
    assert(node);
    assert(!node->isInline()); // inline projection's execute() should not be
                                // called
    assert(m_tmpOutputTable);
    TableTuple &temp_tuple = m_tmpOutputTable->tempTuple();
    int columnCount = m_tmpOutputTable->columnCount();

#ifndef NDEBUG
    if (all_tuple_array == NULL && all_param_array == NULL) {
        for (int ctr = columnCount - 1; ctr >= 0; --ctr) {
            assert(expression_array[ctr]);
            VOLT_TRACE("predicate[%d]: %s", ctr,
                       expression_array[ctr]->debug(true).c_str());
        }
    }
#endif

    assert(node->getInputTables().size() == 1);
    Table* input_table = node->getInputTables()[0];
    assert(input_table);

    VOLT_TRACE("INPUT TABLE: %s\n", input_table->debug().c_str());

    //
    // Now loop through all the tuples and push them through our output
    // expression This will generate new tuple values that we will insert into
    // our output table
    //
    TableTuple tuple = input_table->tempTuple();
    TableIterator iterator = input_table->iteratorDeletingAsWeGo();
    assert (tuple.sizeInValues() == input_table->columnCount());
    while (iterator.next(tuple)) {
        //
        // Project (or replace) values from input tuple
        //
        if (all_tuple_array != NULL) {
            VOLT_TRACE("sweet, all tuples");
            for (int ctr = columnCount - 1; ctr >= 0; --ctr) {
                temp_tuple.setNValue(ctr, tuple.getNValue(all_tuple_array[ctr]));
            }
        } else if (all_param_array != NULL) {
            VOLT_TRACE("sweet, all params");
            for (int ctr = columnCount - 1; ctr >= 0; --ctr) {
                temp_tuple.setNValue(ctr, params[all_param_array[ctr]]);
            }
        } else {
            for (int ctr = columnCount - 1; ctr >= 0; --ctr) {
                temp_tuple.setNValue(ctr, expression_array[ctr]->eval(&tuple, NULL));
            }
        }
        m_tmpOutputTable->insertTempTuple(temp_tuple);

        VOLT_TRACE("OUTPUT TABLE: %s\n", m_tmpOutputTable->debug().c_str());
    }

    //VOLT_TRACE("PROJECTED TABLE: %s\n", m_tmpOutputTable->debug().c_str());

    return (true);
}

ProjectionExecutor::~ProjectionExecutor() {
}

} // namespace voltdb
