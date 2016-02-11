////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
/// @author Achim Brandt
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_GENERAL_SERVER_GENERAL_SERVER_H
#define ARANGOD_GENERAL_SERVER_GENERAL_SERVER_H 1

#include "Basics/Mutex.h"
#include "Basics/SpinLock.h"
#include "HttpServer/GeneralHandler.h"
#include "Rest/ConnectionInfo.h"
#include "Scheduler/TaskManager.h"
#include "VelocyServer/VelocyCommTask.h"


namespace arangodb {
namespace rest {

class AsyncJobManager;
class Dispatcher;
class EndpointList;
class GeneralServerJob;
class HttpCommTask;
class VelocyCommTask;
class GeneralHandlerFactory;
class Job;
class ListenTask;

////////////////////////////////////////////////////////////////////////////////
/// @brief general server
////////////////////////////////////////////////////////////////////////////////

class GeneralServer : protected TaskManager {
  GeneralServer(GeneralServer const&) = delete;
  GeneralServer const& operator=(GeneralServer const&) = delete;

  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroys an endpoint server
  //////////////////////////////////////////////////////////////////////////////

  static int sendChunk(uint64_t, std::string const&);

  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructs a new general server with dispatcher and job manager
  //////////////////////////////////////////////////////////////////////////////

  GeneralServer(Scheduler*, Dispatcher*, GeneralHandlerFactory*, AsyncJobManager*,
             double keepAliveTimeout);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destructs a general server
  //////////////////////////////////////////////////////////////////////////////

  virtual ~GeneralServer();

  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the protocol @TODO: Change it to conditional argument
  //////////////////////////////////////////////////////////////////////////////

  virtual char const* protocol() const { return "http"; }

  //   virtual char const* protocol() const { 
  //   if(_isHttp) {
  //     return "http";
  //   } else{
  //     return "vstream"
  //   }  
  // }


  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the encryption to be used
  //////////////////////////////////////////////////////////////////////////////

  virtual Endpoint::EncryptionType encryptionType() const {
    return Endpoint::ENCRYPTION_NONE;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief generates a suitable communication task
  //////////////////////////////////////////////////////////////////////////////

  virtual HttpCommTask* createCommTask(TRI_socket_t, const ConnectionInfo&);

  virtual VelocyCommTask* createCommTask(TRI_socket_t, const ConnectionInfo&, bool);
  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the scheduler
  //////////////////////////////////////////////////////////////////////////////

  Scheduler* scheduler() const { return _scheduler; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the dispatcher
  //////////////////////////////////////////////////////////////////////////////

  Dispatcher* dispatcher() const { return _dispatcher; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the dispatcher
  //////////////////////////////////////////////////////////////////////////////

  AsyncJobManager* jobManager() const { return _jobManager; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return the handler factory
  //////////////////////////////////////////////////////////////////////////////

  GeneralHandlerFactory* handlerFactory() const { return _handlerFactory; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief adds the endpoint list
  //////////////////////////////////////////////////////////////////////////////

  void setEndpointList(const EndpointList* list);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief starts listening
  //////////////////////////////////////////////////////////////////////////////

  void startListening();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief stops listining
  //////////////////////////////////////////////////////////////////////////////

  void stopListening();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief removes all listen and comm tasks
  //////////////////////////////////////////////////////////////////////////////

  void stop();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief handles connection request
  //////////////////////////////////////////////////////////////////////////////

  void handleConnected(TRI_socket_t s, const ConnectionInfo& info, bool isHttp);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief handles a connection close
  //////////////////////////////////////////////////////////////////////////////

  void handleCommunicationClosed(HttpCommTask*);

  // Overloading for VelocyServer

  void handleCommunicationClosed(VelocyCommTask*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief handles a connection failure
  //////////////////////////////////////////////////////////////////////////////

  void handleCommunicationFailure(HttpCommTask*);

  // Overloading for VelocyServer

  void handleCommunicationFailure(VelocyCommTask*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief creates a job for asynchronous execution
  //////////////////////////////////////////////////////////////////////////////

  bool handleRequestAsync(arangodb::WorkItem::uptr<GeneralHandler>&,
                          uint64_t* jobId);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief executes the handler directly or add it to the queue
  //////////////////////////////////////////////////////////////////////////////

  bool handleRequest(HttpCommTask*, arangodb::WorkItem::uptr<GeneralHandler>&);

  // Overloading for VelocyStream

  bool handleRequest(VelocyCommTask*, arangodb::WorkItem::uptr<GeneralHandler>&);
  
 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief Handler, Job, and Task tuple
  //////////////////////////////////////////////////////////////////////////////

  struct handler_task_job_t {
    GeneralHandler* _handler;
    HttpCommTask* _task;
    GeneralServerJob* _job;
  };

  
 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief opens a listen port
  //////////////////////////////////////////////////////////////////////////////

  bool openEndpoint(Endpoint* endpoint);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief handle request directly
  //////////////////////////////////////////////////////////////////////////////

  void handleRequestDirectly(HttpCommTask* task, GeneralHandler* handler);

  // Overloading for VelocyStream

  void handleRequestDirectly(VelocyCommTask* task, GeneralHandler* handler);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief registers a task
  //////////////////////////////////////////////////////////////////////////////

  void registerHandler(GeneralHandler* handler, HttpCommTask* task);

  
 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief the scheduler
  //////////////////////////////////////////////////////////////////////////////

  Scheduler* _scheduler;  // TODO (fc) XXX make this a singleton

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the dispatcher
  //////////////////////////////////////////////////////////////////////////////

  Dispatcher* _dispatcher;  // TODO (fc) XXX make this a singleton

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the handler factory
  //////////////////////////////////////////////////////////////////////////////

  GeneralHandlerFactory* _handlerFactory;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the job manager
  //////////////////////////////////////////////////////////////////////////////

  AsyncJobManager* _jobManager;  // TODO (fc) XXX make this a singleton

  //////////////////////////////////////////////////////////////////////////////
  /// @brief active listen tasks
  //////////////////////////////////////////////////////////////////////////////

  std::vector<ListenTask*> _listenTasks;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief defined ports and addresses
  //////////////////////////////////////////////////////////////////////////////

  const EndpointList* _endpointList;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief mutex for comm tasks
  //////////////////////////////////////////////////////////////////////////////

  arangodb::basics::Mutex _commTasksLock;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief active comm tasks(Http)
  //////////////////////////////////////////////////////////////////////////////

  std::unordered_set<HttpCommTask*> _commTasks;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief active comm tasks(VelocyStream)
  //////////////////////////////////////////////////////////////////////////////

  std::unordered_set<VelocyCommTask*> _commTasksVstream;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief to judge whether the request received is Http or VelocyStream
  //////////////////////////////////////////////////////////////////////////////

  bool _isHttp;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief keep-alive timeout
  //////////////////////////////////////////////////////////////////////////////

  double _keepAliveTimeout;
};
}
}

#endif


