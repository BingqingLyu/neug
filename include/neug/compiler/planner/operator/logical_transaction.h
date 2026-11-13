#pragma once

#include <string>
#include <utility>

#include "neug/compiler/common/enums/transaction_action.h"
#include "neug/compiler/planner/operator/logical_operator.h"

namespace neug {
namespace planner {

class LogicalTransaction : public LogicalOperator {
  static constexpr LogicalOperatorType type_ = LogicalOperatorType::TRANSACTION;

 public:
  explicit LogicalTransaction(transaction::TransactionAction transactionAction,
                              std::string vertexLabel = "")
      : LogicalOperator{type_},
        transactionAction{transactionAction},
        vertexLabel{std::move(vertexLabel)} {}

  std::string getExpressionsForPrinting() const final { return std::string(); }

  void computeFlatSchema() final { createEmptySchema(); }
  void computeFactorizedSchema() final { createEmptySchema(); }

  transaction::TransactionAction getTransactionAction() const {
    return transactionAction;
  }

  /// COMPACT scope: empty means every vertex label (spec 013).
  const std::string& getVertexLabel() const { return vertexLabel; }

  std::unique_ptr<LogicalOperator> copy() final {
    return std::make_unique<LogicalTransaction>(transactionAction, vertexLabel);
  }

 private:
  transaction::TransactionAction transactionAction;
  std::string vertexLabel;
};

}  // namespace planner
}  // namespace neug
