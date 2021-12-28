/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <memory>
#include "velox/exec/Exchange.h"
#include "velox/exec/MergeSource.h"
#include "velox/exec/RowContainer.h"

namespace facebook::velox::exec {

// Merge operator Implementation: This implementation uses priority queue
// to perform a k-way merge of its inputs. It stops merging if any one of
// its inputs is blocked.
class Merge : public SourceOperator {
 public:
  Merge(
      int32_t operatorId,
      DriverCtx* ctx,
      std::shared_ptr<const RowType> outputType,
      const std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>&
          sortingKeys,
      const std::vector<core::SortOrder>& sortingOrders,
      const std::string& planNodeId,
      const std::string& operatorType);

  std::string toString() override {
    return fmt::format("Merge({})", stats_.operatorId);
  }

  BlockingReason isBlocked(ContinueFuture* future) override;

  RowVectorPtr getOutput() override;

  const std::shared_ptr<const RowType> outputType() const {
    return outputType_;
  }

  memory::MappedMemory* mappedMemory() const {
    return operatorCtx_->mappedMemory();
  }

 protected:
  virtual BlockingReason addMergeSources(ContinueFuture* future) = 0;
  std::vector<std::shared_ptr<MergeSource>> sources_;
  const core::PlanNodeId planNodeId_;

 private:
  static const size_t kBatchSizeInBytes{2 * 1024 * 1024};
  using SourceRow = std::pair<size_t, char*>;

  class Comparator {
   public:
    Comparator(
        std::shared_ptr<const RowType> outputType,
        const std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>&
            sortingKeys,
        const std::vector<core::SortOrder>& sortingOrders,
        RowContainer* rowContainer);

    // Returns true if lhs > rhs, false otherwise.
    bool operator()(const SourceRow& lhs, const SourceRow& rhs) {
      for (auto& key : keyInfo_) {
        if (auto result = rowContainer_->compare(
                lhs.second,
                rhs.second,
                key.first,
                {key.second.isNullsFirst(), key.second.isAscending(), false})) {
          return result > 0;
        }
      }
      return false;
    }

   private:
    std::vector<std::pair<ChannelIndex, core::SortOrder>> keyInfo_;
    RowContainer* rowContainer_;
  };

  BlockingReason pushSource(ContinueFuture* future, size_t sourceId);
  BlockingReason ensureSourcesReady(ContinueFuture* future);

  std::vector<char*> rows_;
  std::unique_ptr<RowContainer> rowContainer_;
  std::priority_queue<SourceRow, std::vector<SourceRow>, Comparator>
      candidates_;

  RowVectorPtr extractedCols_;

  BlockingReason blockingReason_{BlockingReason::kNotBlocked};
  ContinueFuture future_;

  size_t numSourcesAdded_ = 0;
  size_t currentSourcePos_ = 0;
};

// LocalMerge merges its sources' output into a single stream of
// sorted rows. It runs single threaded. The sources may run multi-threaded
// and run in the merge's task itself.
class LocalMerge : public Merge {
 public:
  LocalMerge(
      int32_t operatorId,
      DriverCtx* driverCtx,
      int32_t numSources,
      const std::shared_ptr<const core::LocalMergeNode>& localMergeNode);

 protected:
  BlockingReason addMergeSources(ContinueFuture* future) override;

 private:
  int32_t numSources_;
};

// MergeExchange merges its sources' outputs into a single stream of
// sorted rows similar to local merge. However, the sources are splits
// and may be generated by a different task.
class MergeExchange : public Merge {
 public:
  MergeExchange(
      int32_t operatorId,
      DriverCtx* driverCtx,
      const std::shared_ptr<const core::MergeExchangeNode>& orderByNode);

  void finish() override;

 protected:
  BlockingReason addMergeSources(ContinueFuture* future) override;

 private:
  bool noMoreSplits_ = false;
  size_t numSplits_{0}; // Number of splits we took to process so far.
};

} // namespace facebook::velox::exec
