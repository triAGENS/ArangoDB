////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Daniel H. Larkin
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_BASICS_LOCAL_TASK_QUEUE_H
#define ARANGODB_BASICS_LOCAL_TASK_QUEUE_H 1

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

#include "Basics/Result.h"

namespace arangodb {
namespace application_features {
class ApplicationServer;
}
namespace basics {

class LocalTaskQueue;

class LocalTask : public std::enable_shared_from_this<LocalTask> {
 public:
  LocalTask() = delete;
  LocalTask(LocalTask const&) = delete;
  LocalTask& operator=(LocalTask const&) = delete;

  explicit LocalTask(std::shared_ptr<LocalTaskQueue> const& queue);
  virtual ~LocalTask();

  virtual void run() = 0;
  bool dispatch();

 protected:
  /// @brief the underlying queue
  std::shared_ptr<LocalTaskQueue> _queue;
};

/// @brief a helper task type to dispatch simple lambdas
class LambdaTask : public LocalTask {
 public:
  LambdaTask(std::shared_ptr<LocalTaskQueue> const&, std::function<Result()>&&);

  virtual void run();

 private:
  std::function<Result()> _fn;
};

class LocalTaskQueue {
  friend class LocalTask;

 public:
  typedef std::function<bool(std::function<void()>)> PostFn;

  LocalTaskQueue() = delete;
  LocalTaskQueue(LocalTaskQueue const&) = delete;
  LocalTaskQueue& operator=(LocalTaskQueue const&) = delete;

  explicit LocalTaskQueue(application_features::ApplicationServer& server, PostFn poster);

  ~LocalTaskQueue();

  void startTask();
  void stopTask();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief enqueue a task to be run
  //////////////////////////////////////////////////////////////////////////////

  void enqueue(std::shared_ptr<LocalTask>);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief post a function to the scheduler. Should only be used internally
  /// by task dispatch.
  //////////////////////////////////////////////////////////////////////////////

  bool post(std::function<bool()>&& fn);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief dispatch all tasks, including those that are queued while running,
  /// and wait for all tasks to join; then dispatch all callback tasks and wait
  /// for them to join
  //////////////////////////////////////////////////////////////////////////////

  void dispatchAndWait();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set status of queue
  //////////////////////////////////////////////////////////////////////////////

  void setStatus(Result);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return overall status of queue tasks
  //////////////////////////////////////////////////////////////////////////////

  Result status();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set limit for concurrently dispatched tasks
  //////////////////////////////////////////////////////////////////////////////

  void setConcurrency(std::size_t);

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief underlying application server
  //////////////////////////////////////////////////////////////////////////////
  application_features::ApplicationServer& _server;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief post task to scheduler/io_service
  //////////////////////////////////////////////////////////////////////////////

  PostFn _poster;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief internal task queue
  //////////////////////////////////////////////////////////////////////////////

  std::queue<std::shared_ptr<LocalTask>> _queue;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief condition variable
  //////////////////////////////////////////////////////////////////////////////

  std::condition_variable _condition;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief internal mutex
  //////////////////////////////////////////////////////////////////////////////

  std::mutex _mutex;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief number of dispatched, non-joined tasks
  //////////////////////////////////////////////////////////////////////////////

  std::atomic<std::size_t> _dispatched;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief maximum number of concurrently dispatched tasks
  //////////////////////////////////////////////////////////////////////////////

  std::size_t _concurrency;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief number of dispatched and started tasks
  //////////////////////////////////////////////////////////////////////////////

  std::atomic<std::size_t> _started;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief overall status of queue tasks
  //////////////////////////////////////////////////////////////////////////////

  Result _status;
};

}  // namespace basics
}  // namespace arangodb

#endif
