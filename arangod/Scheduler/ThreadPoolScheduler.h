////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Scheduler.h"
#include "SimpleThreadPool.h"
#include "WorkStealingThreadPool.h"
#include "SchedulerMetrics.h"

namespace arangodb {

struct ThreadPoolScheduler final : Scheduler {
  explicit ThreadPoolScheduler(ArangodServer& server, uint64_t maxThreads,
                               std::shared_ptr<SchedulerMetrics> metrics);
  void toVelocyPack(velocypack::Builder& builder) const override;
  QueueStatistics queueStatistics() const override;
  void trackCreateHandlerTask() noexcept override;
  void trackBeginOngoingLowPriorityTask() noexcept override;
  void trackEndOngoingLowPriorityTask() noexcept override;
  void trackQueueTimeViolation() override;
  void trackQueueItemSize(std::int64_t x) noexcept override;
  uint64_t getLastLowPriorityDequeueTime() const noexcept override;
  void setLastLowPriorityDequeueTime(uint64_t time) noexcept override;
  std::pair<uint64_t, uint64_t> getNumberLowPrioOngoingAndQueued()
      const override;
  double approximateQueueFillGrade() const override;
  double unavailabilityQueueFillGrade() const override;

  void shutdown() override;

 protected:
  bool queueItem(RequestLane lane, std::unique_ptr<WorkItemBase> item,
                 bool bounded) override;
  bool isStopping() override { return _stopping; }

 private:
  using ThreadPool = WorkStealingThreadPool;
  std::shared_ptr<SchedulerMetrics> _metrics;
  std::atomic<bool> _stopping = false;
  std::atomic<uint64_t> _lastLowPriorityDequeueTime = 0;
  std::vector<std::unique_ptr<ThreadPool>> _threadPools;
};

}  // namespace arangodb
