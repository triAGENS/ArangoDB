////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
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
/// @author Matthew Von-Maszewski
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// based upon leveldb/util/throttle.cc
// Copyright (c) 2011-2017 Basho Technologies, Inc. All Rights Reserved.
//
// This file is provided to you under the Apache License,
// Version 2.0 (the "License"); you may not use this file
// except in compliance with the License.  You may obtain
// a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBThrottle.h"

#ifndef WIN32
#include <sys/syscall.h>
#include <sys/resource.h>
#endif

#include "Basics/ConditionLocker.h"
#include "Basics/MutexLocker.h"

namespace arangodb {

////////////////////////////////////////////////////////////////////////////////
/// AdjustThreadPriority() below uses the Linux setpriority() function to dynamically
///  lower and raise a given thread's scheduling priority.  The Linux default is
///  to only allow a thread to lower its priority, not to raise it.  Even if the
///  raise would be to a previous priority.
///
/// Servers with 4 cores or less REALLY need the full benefit of AdjustThreadPriority().
///
/// To get full performance benefit of this code, the server needs three settings:
///
///  1. /etc/pam.d/login must contain the line "auth	   require    pam_cap.so"
///  2. /etc/security/capability.conf must contain "cap_sys_nice      arangodb"
///  3. root must execute this command "setcap cap_sys_nice+ie arangod" on
///      the arangodb binary executable
///
/// The above settings allow the code to vary the threads across 3 priorities based upon
///  the current compaction's level.  Without the settings, threads eventual lock into only 2
///  different priorities (which is still far better having everything at same priority).
///
/// Setting 3 above must be applied to the arangod binary after every build or installation.
///
/// The code does not (yet) support Windows.
////////////////////////////////////////////////////////////////////////////////

// code will dynamically change a thread's priority based upon the compaction's level:
//  base +1 : flush mem buffer to level 0
//  base +2 : level 0 compaction to level 1
//  base +3 : all other compactions
struct sPriorityInfo {
  bool _baseSet;
  int _basePriority;
  int _currentPriority;
};

thread_local sPriorityInfo gThreadPriority={false, 0, 0};


// rocksdb flushes and compactions start and stop within same thread, no overlapping
//  (OSX 10.12 requires a static initializer for thread_local ... time_point on mac does not have
//   one in clang 9.0.0)
thread_local uint8_t gFlushStart[sizeof(std::chrono::steady_clock::time_point)];


///
/// @brief Object that RocksDBThrottle gives to a compaction thread
///
class RocksDBCompactionListener : public rocksdb::CompactionEventListener {

  ///
  /// @brief This is called for every key in a compaction.  Our code only uses "level"
  ///  to help manipulate thread priority
  void OnCompaction(int level, const Slice& key,
                    CompactionListenerValueType value_type,
                    const Slice& existing_value,
                    const SequenceNumber& sn, bool is_new) override
  {RocksDBThrottle::AdjustThreadPriority( (0==level) ? 2 : 3);};

};// RocksDBCompactionListener

RocksDBCompactionListener gCompactionListener;


//
// Setup the object, clearing variables, but do no real work
//
RocksDBThrottle::RocksDBThrottle()
  : _internalRocksDB(nullptr), _threadRunning(false), _throttleBps(0), _firstThrottle(true)
{
  memset(&_throttleData, 0, sizeof(_throttleData));
}


//
// Shutdown the background thread only if it was ever started
//
RocksDBThrottle::~RocksDBThrottle() {

  if (_threadRunning.load()) {
    {
      CONDITION_LOCKER(guard, _threadCondvar);

      _threadRunning.store(false);
      _threadCondvar.signal();
    } // lock

    _threadFuture.wait();
  } // if

}


///
/// @brief CompactionEventListener::OnCompaction() will get called for every key in a
///  compaction. We only need the "level" parameter to see if thread priority should change.
///
#ifndef WIN32
CompactionEventListener * RocksDBThrottle::GetCompactionEventListener() {return &gCompactionListener;};
#else
CompactionEventListener * RocksDBThrottle::GetCompactionEventListener() {return nullptr;};
#endif  // WIN32

///
/// @brief rocksdb does not track flush time in its statistics.  Save start time in
///  a thread specific storage
///
void RocksDBThrottle::OnFlushBegin(rocksdb::DB* db, const rocksdb::FlushJobInfo& flush_job_info) {

  // save start time in thread local storage
  std::chrono::steady_clock::time_point osx_hack = std::chrono::steady_clock::now();
  memcpy(gFlushStart, &osx_hack, sizeof(std::chrono::steady_clock::time_point));
  AdjustThreadPriority(1);

  return;

} // RocksDBThrottle::OnFlushBegin


void RocksDBThrottle::OnFlushCompleted(rocksdb::DB* db, const rocksdb::FlushJobInfo& flush_job_info) {
  std::chrono::microseconds flush_time;
  uint64_t flush_size;
  std::chrono::steady_clock::time_point osx_hack;

  memcpy(&osx_hack, gFlushStart, sizeof(std::chrono::steady_clock::time_point));

  flush_time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()
                                                                     - osx_hack);
  flush_size = flush_job_info.table_properties.data_size + flush_job_info.table_properties.index_size
    + flush_job_info.table_properties.filter_size;

  SetThrottleWriteRate(flush_time, flush_job_info.table_properties.num_entries, flush_size, true);

  // start throttle after first data is posted
  //  (have seen some odd zero and small size flushes early)
  if (1024<flush_size) {
    std::call_once(_initFlag, &RocksDBThrottle::Startup, this, db);
  } // if

} // RocksDBThrottle::OnFlushCompleted


void RocksDBThrottle::OnCompactionCompleted(rocksdb::DB* db,
                                                 const rocksdb::CompactionJobInfo& ci) {

  std::chrono::microseconds elapsed(ci.stats.elapsed_micros);
  SetThrottleWriteRate(elapsed, ci.stats.num_output_records, ci.stats.total_output_bytes, false);

} // RocksDBThrottle::OnCompactionCompleted


void RocksDBThrottle::Startup(rocksdb::DB* db) {
  CONDITION_LOCKER(guard, _threadCondvar);

  _internalRocksDB = (rocksdb::DBImpl *)db;

  // addresses race condition during fast start/stop
  _threadFuture = std::async(std::launch::async, &RocksDBThrottle::ThreadLoop, this);

  while(!_threadRunning.load()) {
    _threadCondvar.wait(10);
  } // while

} // RocksDBThrottle::Startup


void RocksDBThrottle::SetThrottleWriteRate(std::chrono::microseconds Micros,
                                                uint64_t Keys, uint64_t Bytes, bool IsLevel0) {
  // lock _threadMutex while we update _throttleData
  MUTEX_LOCKER(mutexLocker, _threadMutex);
  unsigned target_idx;

  // index 0 for level 0 compactions, index 1 for all others
  target_idx = (IsLevel0 ? 0 : 1);

  _throttleData[target_idx]._micros+=Micros;
  _throttleData[target_idx]._keys+=Keys;
  _throttleData[target_idx]._bytes+=Bytes;
  _throttleData[target_idx]._compactions+=1;

  // attempt to override throttle changes by rocksdb ... hammer this often
  //  (note that _threadMutex IS HELD)
  SetThrottle();

  return;
} // RocksDBThrottle::SetThrottleWriteRate


void RocksDBThrottle::ThreadLoop() {

  _replaceIdx=2;

  // addresses race condition during fast start/stop
  {
    CONDITION_LOCKER(guard, _threadCondvar);

    _threadRunning.store(true);
    _threadCondvar.signal();
  } // lock

  while(_threadRunning.load()) {
    //
    // start actual throttle work
    //
    RecalculateThrottle();

    ++_replaceIdx;
    if (THROTTLE_INTERVALS==_replaceIdx)
      _replaceIdx=2;

    // wait on _threadCondvar
    {
      CONDITION_LOCKER(guard, _threadCondvar);

      if (_threadRunning.load()) { // test in case of race at shutdown
        _threadCondvar.wait(THROTTLE_SECONDS * 1000000);
      } //if
    } // lock
  } // while

} // RocksDBThrottle::ThreadLoop


//
// Routine to actually perform the throttle calculation,
//  now is external routing from ThreadLoop() to easy unit test
void RocksDBThrottle::RecalculateThrottle() {
  unsigned loop;
  std::chrono::microseconds tot_micros;
  uint64_t tot_bytes, tot_keys, tot_compact, adjustment_bytes;
  int64_t new_throttle, compaction_backlog, temp_rate;
  bool no_data;

  tot_micros*=0;
  tot_keys=0;
  tot_bytes=0;
  tot_compact=0;
  temp_rate=0;

  compaction_backlog = ComputeBacklog();

  {
    MUTEX_LOCKER(mutexLocker, _threadMutex);

    _throttleData[_replaceIdx]=_throttleData[1];
    memset(&_throttleData[1], 0, sizeof(_throttleData[1]));

    // this could be faster by keeping running totals and
    //  subtracting [_replaceIdx] before copying [0] into it,
    //  then adding new [_replaceIdx].  But that needs more
    //  time for testing.
    for (loop=2; loop<THROTTLE_INTERVALS; ++loop)
    {
      tot_micros+=_throttleData[loop]._micros;
      tot_keys+=_throttleData[loop]._keys;
      tot_bytes+=_throttleData[loop]._bytes;
      tot_compact+=_throttleData[loop]._compactions;
    }   // for
  } // unique_lock

    // flag to skip throttle changes if zero data available
  no_data = (0 == tot_bytes && 0 == _throttleData[0]._bytes);

  // reduce bytes by 10% for each excess level_0 files and/or excess write buffers
  adjustment_bytes = (tot_bytes*compaction_backlog)/10;
  if (adjustment_bytes<tot_bytes) {
    tot_bytes -= adjustment_bytes;
  } else {
    tot_bytes = 1;   // not zero, let smoothing drift number down instead of taking level-0
  }

  // lock _threadMutex while we update _throttleData
  if (!no_data) {
    MUTEX_LOCKER(mutexLocker, _threadMutex);

    // non-level0 data available?
    if (0!=tot_bytes && 0!=tot_micros.count())
    {
      // average bytes per secon for level 1+ compactions
      //  (adjust bytes upward by 1000000 since dividing by microseconds,
      //   yields integer bytes per second)
      new_throttle=((tot_bytes*1000000) / tot_micros.count());

    }   // if

    // attempt to most recent level0
    //  (only use most recent level0 until level1+ data becomes available,
    //   useful on restart of heavily loaded server)
    else if (0!=_throttleData[0]._bytes && 0!=_throttleData[0]._micros.count())
    {
      new_throttle=(_throttleData[0]._bytes * 1000000) / _throttleData[0]._micros.count();
    }   // else if
    else
    {
      new_throttle=1;
    }   // else

    if (0==new_throttle)
      new_throttle=1;     // throttle must have an effect

    // change the throttle slowly
    //  (+1 & +2 keep throttle moving toward goal when difference new and
    //   old is less than THROTTLE_SCALING)
    if (!_firstThrottle) {
      temp_rate=_throttleBps;

      if (temp_rate < new_throttle)
        temp_rate+=(new_throttle - temp_rate)/THROTTLE_SCALING +1;
      else
        temp_rate-=(temp_rate - new_throttle)/THROTTLE_SCALING +2;

      // +2 can make this go negative
      if (temp_rate<1)
        temp_rate=1;   // throttle must always have an effect

      _throttleBps=temp_rate;

      // prepare for next interval
      memset(&_throttleData[0], 0, sizeof(_throttleData[0]));
    } else if (1<new_throttle) {
      // never had a valid throttle, and have first hint now
      _throttleBps=new_throttle;

      _firstThrottle=false;
    }  // else if

    SetThrottle();

  } // !no_data && unlock _threadMutex

} // RocksDBThrottle::RecalculateThrottle


///
/// @brief Hack a throttle rate into the WriteController object
///
void RocksDBThrottle::SetThrottle() {
  // called by routine with _threadMutex held

  // this routine can get called before _internalRocksDB is set
  if (nullptr != _internalRocksDB) {
    // inform write_controller_ of our new rate
    if (1<_throttleBps) {
      // hard casting away of "const" ...
      _delayToken=(((WriteController&)_internalRocksDB->write_controller()).GetDelayToken(_throttleBps));
    } else {
      _delayToken.reset();
    } // else
  } // if
} // RocksDBThrottle::SetThrottle


///
/// @brief Use rocksdb's internal statistics to determine if
///  additional slowing of writes is warranted
///
int64_t RocksDBThrottle::ComputeBacklog() {
  int64_t compaction_backlog, imm_backlog, imm_trigger;
  bool ret_flag;
  std::string ret_string, property_name;
  int temp;

  // want count of level 0 files to estimate if compactions "behind"
  //  and therefore likely to start stalling / stopping
  compaction_backlog = 0;
  imm_backlog = 0;
  if (_families.size()) {
    imm_trigger = _internalRocksDB->GetOptions(_families[0]).max_write_buffer_number / 2;
  } else {
    imm_trigger = 3;
  } // else

  // loop through column families to obtain family specific counts
  for (auto & cf : _families) {
    property_name = rocksdb::DB::Properties::kNumFilesAtLevelPrefix;
    property_name.append("0");
    ret_flag=_internalRocksDB->GetProperty(cf, property_name, &ret_string);
    if (ret_flag) {
      temp=std::stoi(ret_string);
    } else {
      temp =0;
    } // else

    if (kL0_SlowdownWritesTrigger<=temp) {
      temp -= (kL0_SlowdownWritesTrigger -1);
    } else {
      temp = 0;
    } // else

    compaction_backlog += temp;

    property_name=rocksdb::DB::Properties::kNumImmutableMemTable;
    ret_flag=_internalRocksDB->GetProperty(cf, property_name, &ret_string);

    if (ret_flag) {
      temp=std::stoi(ret_string);
      imm_backlog += temp;
    } // if
  } // for

  if (imm_trigger<imm_backlog) {
    compaction_backlog += (imm_backlog - imm_trigger);
  } // if

  return compaction_backlog;
} // RocksDBThrottle::Computebacklog


/// @brief Adjust the active thread's priority to match the work
///  it is performing.  The routine is called HEAVILY.
void RocksDBThrottle::AdjustThreadPriority(int Adjustment) {
#ifndef WIN32
  // initialize thread infor if this the first time the thread has ever called
  if (!gThreadPriority._baseSet) {
    pid_t tid;
    int ret_val;

    tid = syscall(SYS_gettid);
    if (-1!=(int)tid)
    {
      errno=0;
      ret_val=getpriority(PRIO_PROCESS, tid);
      // ret_val could be -1 legally, so double test
      if (-1!=ret_val || 0==errno) {
        gThreadPriority._baseSet = true;
        gThreadPriority._basePriority = ret_val;
        gThreadPriority._currentPriority = ret_val;
      }   // if
    }   // if
  } // if

  // only change priorities if we
  if (gThreadPriority._baseSet
      && (gThreadPriority._basePriority+Adjustment)!=gThreadPriority._currentPriority) {

    pid_t tid;
    tid = syscall(SYS_gettid);
    if (-1!=(int)tid)
    {
      gThreadPriority._currentPriority = gThreadPriority._basePriority + Adjustment;
      setpriority(PRIO_PROCESS, tid, gThreadPriority._currentPriority);
    } // if
  } // if

#endif   // WIN32
} // RocksDBThrottle::AdjustThreadPriority


} // namespace arangodb
