////////////////////////////////////////////////////////////////////////////////
/// @brief Write-ahead log file manager
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_WAL_LOGFILE_MANAGER_H
#define ARANGODB_WAL_LOGFILE_MANAGER_H 1

#include "Basics/Common.h"
#include "Basics/Mutex.h"
#include "Basics/ReadWriteLock.h"
#include "ApplicationServer/ApplicationFeature.h"
#include "VocBase/voc-types.h"
#include "Wal/Logfile.h"
#include "Wal/Marker.h"
#include "Wal/Slots.h"

#include <regex.h>

struct TRI_server_s;

namespace triagens {
  namespace wal {

    class AllocatorThread;
    class CollectorThread;
    class Slot;
    class SynchroniserThread;

// -----------------------------------------------------------------------------
// --SECTION--                                                      RecoverState
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief state that is built up when scanning a WAL logfile during recovery
////////////////////////////////////////////////////////////////////////////////

    struct RecoverState {
      RecoverState ()
        : collections(),
          failedTransactions(),
          droppedCollections(),
          droppedDatabases(),
          lastTick(0),
          logfilesToCollect(0) {
      }

      std::unordered_map<TRI_voc_cid_t, TRI_voc_tick_t> collections;
      std::unordered_map<TRI_voc_tid_t, std::pair<TRI_voc_tick_t, bool>> failedTransactions;
      std::unordered_set<TRI_voc_cid_t>                 droppedCollections;
      std::unordered_set<TRI_voc_tick_t>                droppedDatabases;
      TRI_voc_tick_t                                    lastTick;
      int                                               logfilesToCollect;
    };

// -----------------------------------------------------------------------------
// --SECTION--                                               LogfileManagerState
// -----------------------------------------------------------------------------

    struct LogfileManagerState {
      TRI_voc_tick_t  lastTick;
      TRI_voc_tick_t  lastDataTick;
      uint64_t        numEvents;
      std::string     timeString;
    };

// -----------------------------------------------------------------------------
// --SECTION--                                              class LogfileManager
// -----------------------------------------------------------------------------

    class LogfileManager : public rest::ApplicationFeature {

      friend class AllocatorThread;
      friend class CollectorThread;

////////////////////////////////////////////////////////////////////////////////
/// @brief LogfileManager
////////////////////////////////////////////////////////////////////////////////

      private:
        LogfileManager (LogfileManager const&) = delete;
        LogfileManager& operator= (LogfileManager const&) = delete;

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

      public:

        LogfileManager (struct TRI_server_s*,
                        std::string*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the logfile manager
////////////////////////////////////////////////////////////////////////////////

        ~LogfileManager ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get the logfile manager instance
////////////////////////////////////////////////////////////////////////////////

        static LogfileManager* instance ();

////////////////////////////////////////////////////////////////////////////////
/// @brief initialise the logfile manager instance
////////////////////////////////////////////////////////////////////////////////

        static void initialise (std::string*,
                                struct TRI_server_s*);

// -----------------------------------------------------------------------------
// --SECTION--                                        ApplicationFeature methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        void setupOptions (std::map<string, triagens::basics::ProgramOptionsDescription>&);

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool prepare ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool open ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool start ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        void close ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        void stop ();

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief get the logfile directory
////////////////////////////////////////////////////////////////////////////////

        inline std::string directory () const {
          return _directory;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the logfile size
////////////////////////////////////////////////////////////////////////////////

        inline uint32_t filesize () const {
          return _filesize;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the logfile size
////////////////////////////////////////////////////////////////////////////////

        inline void filesize (uint32_t value) {
          _filesize = value;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the sync interval
////////////////////////////////////////////////////////////////////////////////

        inline uint64_t syncInterval () const {
          return _syncInterval / 1000;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the sync interval
////////////////////////////////////////////////////////////////////////////////

        inline void syncInterval (uint64_t value) {
          _syncInterval = value * 1000;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the number of reserve logfiles
////////////////////////////////////////////////////////////////////////////////

        inline uint32_t reserveLogfiles () const {
          return _reserveLogfiles;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the number of reserve logfiles
////////////////////////////////////////////////////////////////////////////////

        inline void reserveLogfiles (uint32_t value) {
          _reserveLogfiles = value;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the number of historic logfiles to keep
////////////////////////////////////////////////////////////////////////////////

        inline uint32_t historicLogfiles () const {
          return _historicLogfiles;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the number of historic logfiles
////////////////////////////////////////////////////////////////////////////////

        inline void historicLogfiles (uint32_t value) {
          _historicLogfiles = value;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not there was a SHUTDOWN file with a tick value
/// at server start
////////////////////////////////////////////////////////////////////////////////

        inline bool hasFoundLastTick () const {
          return _hasFoundLastTick;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the slots manager
////////////////////////////////////////////////////////////////////////////////

        Slots* slots () {
          return _slots;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not oversize entries are allowed
////////////////////////////////////////////////////////////////////////////////

        inline bool allowOversizeEntries () const {
          return _allowOversizeEntries;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the "allowOversizeEntries" value
////////////////////////////////////////////////////////////////////////////////

        inline void allowOversizeEntries (bool value) {
          _allowOversizeEntries = value;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not write-throttling can be enabled
////////////////////////////////////////////////////////////////////////////////

        inline bool canBeThrottled () const {
          return (_throttleWhenPending > 0);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief maximum wait time when write-throttled (in milliseconds)
////////////////////////////////////////////////////////////////////////////////

        inline uint64_t maxThrottleWait () const {
          return _maxThrottleWait;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief maximum wait time when write-throttled (in milliseconds)
////////////////////////////////////////////////////////////////////////////////

        inline void maxThrottleWait (uint64_t value) {
          _maxThrottleWait = value;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not write-throttling is currently enabled
////////////////////////////////////////////////////////////////////////////////

        inline bool isThrottled () {
          return (_writeThrottled != 0);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief activate write-throttling
////////////////////////////////////////////////////////////////////////////////

        void activateWriteThrottling () {
          _writeThrottled = 1;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief deactivate write-throttling
////////////////////////////////////////////////////////////////////////////////
        
        void deactivateWriteThrottling () {
          _writeThrottled = 0;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief allow or disallow writes to the WAL
////////////////////////////////////////////////////////////////////////////////

        inline void allowWrites (bool value) {
          _allowWrites = value;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the value of --wal.throttle-when-pending
////////////////////////////////////////////////////////////////////////////////

        inline uint64_t throttleWhenPending () const {
          return _throttleWhenPending;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the value of --wal.throttle-when-pending
////////////////////////////////////////////////////////////////////////////////

        inline void throttleWhenPending (uint64_t value) {
          _throttleWhenPending = value;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not we are in the recovery mode
////////////////////////////////////////////////////////////////////////////////

        inline bool isInRecovery () const {
          return _inRecovery;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief registers a transaction
////////////////////////////////////////////////////////////////////////////////

        bool registerTransaction (TRI_voc_tid_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief unregisters a transaction
////////////////////////////////////////////////////////////////////////////////

        void unregisterTransaction (TRI_voc_tid_t,
                                    bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the set of failed transactions
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<TRI_voc_tid_t> getFailedTransactions ();

////////////////////////////////////////////////////////////////////////////////
/// @brief return the set of dropped collections
/// this is used during recovery and not used afterwards
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<TRI_voc_cid_t> getDroppedCollections ();

////////////////////////////////////////////////////////////////////////////////
/// @brief return the set of dropped databases
/// this is used during recovery and not used afterwards
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<TRI_voc_tick_t> getDroppedDatabases ();

////////////////////////////////////////////////////////////////////////////////
/// @brief unregister a list of failed transactions
////////////////////////////////////////////////////////////////////////////////

        void unregisterFailedTransactions (std::unordered_set<TRI_voc_tid_t> const&);

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not it is currently allowed to create an additional
/// logfile
////////////////////////////////////////////////////////////////////////////////

        bool logfileCreationAllowed (uint32_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not there are reserve logfiles
////////////////////////////////////////////////////////////////////////////////

        bool hasReserveLogfiles ();

////////////////////////////////////////////////////////////////////////////////
/// @brief signal that a sync operation is required
////////////////////////////////////////////////////////////////////////////////

        void signalSync ();

////////////////////////////////////////////////////////////////////////////////
/// @brief reserve space in a logfile
////////////////////////////////////////////////////////////////////////////////

        SlotInfo allocate (void const*,
                           uint32_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief finalise a log entry
////////////////////////////////////////////////////////////////////////////////

        void finalise (SlotInfo&, bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief write data into the logfile
/// this is a convenience function that combines allocate, memcpy and finalise
////////////////////////////////////////////////////////////////////////////////

        SlotInfoCopy allocateAndWrite (void*,
                                       uint32_t,
                                       bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief write data into the logfile
/// this is a convenience function that combines allocate, memcpy and finalise
////////////////////////////////////////////////////////////////////////////////

        SlotInfoCopy allocateAndWrite (Marker const&,
                                       bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief set all open logfiles to status sealed
////////////////////////////////////////////////////////////////////////////////

        void setAllSealed ();

////////////////////////////////////////////////////////////////////////////////
/// @brief finalise and seal the currently open logfile
/// this is useful to ensure that any open writes up to this point have made
/// it into a logfile
////////////////////////////////////////////////////////////////////////////////

        int flush (bool,
                   bool,
                   bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief re-inserts a logfile back into the inventory only
////////////////////////////////////////////////////////////////////////////////

        void relinkLogfile (Logfile*);

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a logfile from the inventory only
////////////////////////////////////////////////////////////////////////////////

        bool unlinkLogfile (Logfile*);

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a logfile from the inventory only
////////////////////////////////////////////////////////////////////////////////

        Logfile* unlinkLogfile (Logfile::IdType);

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a logfile from the inventory and in the file system
////////////////////////////////////////////////////////////////////////////////

        void removeLogfile (Logfile*,
                            bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the status of a logfile to open
////////////////////////////////////////////////////////////////////////////////

        void setLogfileOpen (Logfile*);

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the status of a logfile to seal-requested
////////////////////////////////////////////////////////////////////////////////

        void setLogfileSealRequested (Logfile*);

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the status of a logfile to sealed
////////////////////////////////////////////////////////////////////////////////

        void setLogfileSealed (Logfile*);

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the status of a logfile to sealed
////////////////////////////////////////////////////////////////////////////////

        void setLogfileSealed (Logfile::IdType);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the status of a logfile
////////////////////////////////////////////////////////////////////////////////

        Logfile::StatusType getLogfileStatus (Logfile::IdType);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the file descriptor of a logfile
////////////////////////////////////////////////////////////////////////////////

        int getLogfileDescriptor (Logfile::IdType);

////////////////////////////////////////////////////////////////////////////////
/// @brief get the current open region of a logfile
/// this uses the slots lock
////////////////////////////////////////////////////////////////////////////////

        void getActiveLogfileRegion (Logfile*,
                                     char const*&,
                                     char const*&);

////////////////////////////////////////////////////////////////////////////////
/// @brief get logfiles for a tick range
////////////////////////////////////////////////////////////////////////////////

        std::vector<Logfile*> getLogfilesForTickRange (TRI_voc_tick_t,
                                                       TRI_voc_tick_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief return logfiles for a tick range
////////////////////////////////////////////////////////////////////////////////

        void returnLogfiles (std::vector<Logfile*> const&);

////////////////////////////////////////////////////////////////////////////////
/// @brief get a logfile by id
////////////////////////////////////////////////////////////////////////////////

        Logfile* getLogfile (Logfile::IdType);

////////////////////////////////////////////////////////////////////////////////
/// @brief get a logfile for writing. this may return nullptr
////////////////////////////////////////////////////////////////////////////////

        Logfile* getWriteableLogfile (uint32_t,
                                      Logfile::StatusType&);

////////////////////////////////////////////////////////////////////////////////
/// @brief get a logfile to collect. this may return nullptr
////////////////////////////////////////////////////////////////////////////////

        Logfile* getCollectableLogfile ();

////////////////////////////////////////////////////////////////////////////////
/// @brief get a logfile to remove. this may return nullptr
////////////////////////////////////////////////////////////////////////////////

        Logfile* getRemovableLogfile ();

////////////////////////////////////////////////////////////////////////////////
/// @brief increase the number of collect operations for a logfile
////////////////////////////////////////////////////////////////////////////////

        void increaseCollectQueueSize (Logfile*);

////////////////////////////////////////////////////////////////////////////////
/// @brief decrease the number of collect operations for a logfile
////////////////////////////////////////////////////////////////////////////////

        void decreaseCollectQueueSize (Logfile*);

////////////////////////////////////////////////////////////////////////////////
/// @brief mark a file as being requested for collection
////////////////////////////////////////////////////////////////////////////////

        void setCollectionRequested (Logfile*);

////////////////////////////////////////////////////////////////////////////////
/// @brief mark a file as being done with collection
////////////////////////////////////////////////////////////////////////////////

        void setCollectionDone (Logfile*);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the current state
////////////////////////////////////////////////////////////////////////////////

        LogfileManagerState state ();

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief wait for the collector thread to collect a specific logfile
////////////////////////////////////////////////////////////////////////////////

        void waitForCollector (Logfile::IdType);

////////////////////////////////////////////////////////////////////////////////
/// @brief scan a single logfile
////////////////////////////////////////////////////////////////////////////////

        bool scanLogfile (Logfile const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief write abort markers for all open transactions
////////////////////////////////////////////////////////////////////////////////

        void closeOpenTransactions ();

////////////////////////////////////////////////////////////////////////////////
/// @brief run the recovery procedure
////////////////////////////////////////////////////////////////////////////////

        bool runRecovery ();

////////////////////////////////////////////////////////////////////////////////
/// @brief closes all logfiles
////////////////////////////////////////////////////////////////////////////////

        void closeLogfiles ();

////////////////////////////////////////////////////////////////////////////////
/// @brief reads the shutdown information
////////////////////////////////////////////////////////////////////////////////

        int readShutdownInfo ();

////////////////////////////////////////////////////////////////////////////////
/// @brief writes the shutdown information
////////////////////////////////////////////////////////////////////////////////

        int writeShutdownInfo (bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief start the synchroniser thread
////////////////////////////////////////////////////////////////////////////////

        int startSynchroniserThread ();

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the synchroniser thread
////////////////////////////////////////////////////////////////////////////////

        void stopSynchroniserThread ();

////////////////////////////////////////////////////////////////////////////////
/// @brief start the allocator thread
////////////////////////////////////////////////////////////////////////////////

        int startAllocatorThread ();

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the allocator thread
////////////////////////////////////////////////////////////////////////////////

        void stopAllocatorThread ();

////////////////////////////////////////////////////////////////////////////////
/// @brief start the collector thread
////////////////////////////////////////////////////////////////////////////////

        int startCollectorThread ();

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the collector thread
////////////////////////////////////////////////////////////////////////////////

        void stopCollectorThread ();

////////////////////////////////////////////////////////////////////////////////
/// @brief check which logfiles are present in the log directory
////////////////////////////////////////////////////////////////////////////////

        int inventory ();

////////////////////////////////////////////////////////////////////////////////
/// @brief inspect all found WAL logfiles
/// this searches for the max tick in the logfiles
////////////////////////////////////////////////////////////////////////////////

        int inspectLogfiles ();

////////////////////////////////////////////////////////////////////////////////
/// @brief open the logfiles in the log directory
////////////////////////////////////////////////////////////////////////////////

        int openLogfiles ();

////////////////////////////////////////////////////////////////////////////////
/// @brief allocate a new reserve logfile
////////////////////////////////////////////////////////////////////////////////

        int createReserveLogfile (uint32_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief get an id for the next logfile
////////////////////////////////////////////////////////////////////////////////

        Logfile::IdType nextId ();

////////////////////////////////////////////////////////////////////////////////
/// @brief ensure the wal logfiles directory is actually there
////////////////////////////////////////////////////////////////////////////////

        int ensureDirectory ();

////////////////////////////////////////////////////////////////////////////////
/// @brief return the absolute name of the shutdown file
////////////////////////////////////////////////////////////////////////////////

        std::string shutdownFilename () const;

////////////////////////////////////////////////////////////////////////////////
/// @brief return an absolute filename for a logfile id
////////////////////////////////////////////////////////////////////////////////

        std::string logfileName (Logfile::IdType) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief return the current time as a string
////////////////////////////////////////////////////////////////////////////////

        static std::string getTimeString ();

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief pointer to the server
////////////////////////////////////////////////////////////////////////////////

        struct TRI_server_s* _server;

////////////////////////////////////////////////////////////////////////////////
/// @brief the arangod config variable containing the database path
////////////////////////////////////////////////////////////////////////////////

        std::string* _databasePath;

////////////////////////////////////////////////////////////////////////////////
/// @brief the logfile directory
////////////////////////////////////////////////////////////////////////////////

        std::string _directory;

////////////////////////////////////////////////////////////////////////////////
/// @brief state during recovery
////////////////////////////////////////////////////////////////////////////////
    
        RecoverState* _recoverState;

////////////////////////////////////////////////////////////////////////////////
/// @brief the size of each logfile
////////////////////////////////////////////////////////////////////////////////

        uint32_t _filesize;

////////////////////////////////////////////////////////////////////////////////
/// @brief maximum number of reserve logfiles
////////////////////////////////////////////////////////////////////////////////

        uint32_t _reserveLogfiles;

////////////////////////////////////////////////////////////////////////////////
/// @brief maximum number of historic logfiles
////////////////////////////////////////////////////////////////////////////////

        uint32_t _historicLogfiles;

////////////////////////////////////////////////////////////////////////////////
/// @brief maximum number of parallel open logfiles
////////////////////////////////////////////////////////////////////////////////

        uint32_t _maxOpenLogfiles;

////////////////////////////////////////////////////////////////////////////////
/// @brief number of slots to be used in parallel
////////////////////////////////////////////////////////////////////////////////

        uint32_t _numberOfSlots;

////////////////////////////////////////////////////////////////////////////////
/// @brief interval for automatic, non-requested disk syncs
////////////////////////////////////////////////////////////////////////////////

        uint64_t _syncInterval;

////////////////////////////////////////////////////////////////////////////////
/// @brief maximum wait time for write-throttling
////////////////////////////////////////////////////////////////////////////////

        uint64_t _maxThrottleWait;

////////////////////////////////////////////////////////////////////////////////
/// @brief throttle writes to WAL when at least such many operations are
/// waiting for garbage collection
////////////////////////////////////////////////////////////////////////////////

        uint64_t _throttleWhenPending;

////////////////////////////////////////////////////////////////////////////////
/// @brief allow entries that are bigger than a single logfile
////////////////////////////////////////////////////////////////////////////////

        bool _allowOversizeEntries;

////////////////////////////////////////////////////////////////////////////////
/// @brief ignore logfile errors when opening logfiles
////////////////////////////////////////////////////////////////////////////////

        bool _ignoreLogfileErrors;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not writes to the WAL are allowed
////////////////////////////////////////////////////////////////////////////////

        bool _allowWrites;

////////////////////////////////////////////////////////////////////////////////
/// @brief this is true if there was a SHUTDOWN file with a last tick at
/// server start
////////////////////////////////////////////////////////////////////////////////

        bool _hasFoundLastTick;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the recovery procedure is running
////////////////////////////////////////////////////////////////////////////////

        bool _inRecovery;

////////////////////////////////////////////////////////////////////////////////
/// @brief the slots manager
////////////////////////////////////////////////////////////////////////////////

        Slots* _slots;

////////////////////////////////////////////////////////////////////////////////
/// @brief the synchroniser thread
////////////////////////////////////////////////////////////////////////////////

        SynchroniserThread* _synchroniserThread;

////////////////////////////////////////////////////////////////////////////////
/// @brief the allocator thread
////////////////////////////////////////////////////////////////////////////////

        AllocatorThread* _allocatorThread;

////////////////////////////////////////////////////////////////////////////////
/// @brief the collector thread
////////////////////////////////////////////////////////////////////////////////

        CollectorThread* _collectorThread;

////////////////////////////////////////////////////////////////////////////////
/// @brief a lock protecting the _logfiles map, _lastOpenedId, _lastCollectedId
/// etc.
////////////////////////////////////////////////////////////////////////////////

        basics::ReadWriteLock _logfilesLock;

////////////////////////////////////////////////////////////////////////////////
/// @brief last opened logfile id
////////////////////////////////////////////////////////////////////////////////

        Logfile::IdType _lastOpenedId;

////////////////////////////////////////////////////////////////////////////////
/// @brief last fully collected logfile id
////////////////////////////////////////////////////////////////////////////////

        Logfile::IdType _lastCollectedId;

////////////////////////////////////////////////////////////////////////////////
/// @brief last fully sealed logfile id
////////////////////////////////////////////////////////////////////////////////

        Logfile::IdType _lastSealedId;

////////////////////////////////////////////////////////////////////////////////
/// @brief the logfiles
////////////////////////////////////////////////////////////////////////////////

        std::map<Logfile::IdType, Logfile*> _logfiles;

////////////////////////////////////////////////////////////////////////////////
/// @brief currently ongoing transactions
////////////////////////////////////////////////////////////////////////////////

        std::map<TRI_voc_tid_t, std::pair<Logfile::IdType, Logfile::IdType>> _transactions;

////////////////////////////////////////////////////////////////////////////////
/// @brief set of failed transactions
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<TRI_voc_tid_t> _failedTransactions;

////////////////////////////////////////////////////////////////////////////////
/// @brief set of dropped collections
/// this is populated during recovery and not used afterwards
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<TRI_voc_cid_t> _droppedCollections;

////////////////////////////////////////////////////////////////////////////////
/// @brief set of dropped databases
/// this is populated during recovery and not used afterwards
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<TRI_voc_tick_t> _droppedDatabases;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not write-throttling is currently enabled
////////////////////////////////////////////////////////////////////////////////

        alignas(64) int _writeThrottled;

////////////////////////////////////////////////////////////////////////////////
/// @brief regex to match logfiles
////////////////////////////////////////////////////////////////////////////////

        regex_t _filenameRegex;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not we have been shut down already
////////////////////////////////////////////////////////////////////////////////

        volatile sig_atomic_t _shutdown;

    };

  }
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
