////////////////////////////////////////////////////////////////////////////////
/// @brief abstract base class for tasks
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
/// @author Achim Brandt
/// @author Copyright 2009-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Task.h"

#include "Scheduler/Scheduler.h"

using namespace triagens::rest;
using namespace std;

// -----------------------------------------------------------------------------
// constructors and destructors
// -----------------------------------------------------------------------------

Task::Task (uint64_t id, string const& name)
  : _scheduler(0), 
    _loop(0), 
    _name(name), 
    _id(id), 
    _active(1) {
}



Task::~Task () {
}

// -----------------------------------------------------------------------------
// protected methods
// -----------------------------------------------------------------------------

bool Task::isUserDefined () const {
  return false;
}

bool Task::needsMainEventLoop () const {
  return false;
}

