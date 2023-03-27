////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBUpgrade.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/application-exit.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "ProgramOptions/ProgramOptions.h"
#include "RestServer/DatabaseFeature.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBFormat.h"

#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/transaction_log.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/write_batch.h>

#include <velocypack/Iterator.h>

using namespace arangodb;

void arangodb::rocksdbStartupVersionCheck(ArangodServer& server,
                                          rocksdb::TransactionDB* db,
                                          bool dbExisted) {
  static_assert(
      std::is_same<char, std::underlying_type<RocksDBEndianness>::type>::value,
      "RocksDBEndianness has wrong type");

  // try to find version, using the version key
  char const version = rocksDBFormatVersion();
  RocksDBKey versionKey;
  versionKey.constructSettingsValue(RocksDBSettingsType::Version);

  RocksDBKey endianKey;
  endianKey.constructSettingsValue(RocksDBSettingsType::Endianness);

  // default endianess for existing DBs
  RocksDBEndianness endianess = RocksDBEndianness::Invalid;

  if (dbExisted) {
    rocksdb::PinnableSlice oldVersion;
    rocksdb::Status s =
        db->Get(rocksdb::ReadOptions(),
                RocksDBColumnFamilyManager::get(
                    RocksDBColumnFamilyManager::Family::Definitions),
                versionKey.string(), &oldVersion);

    if (s.IsNotFound() || oldVersion.size() != 1) {
      LOG_TOPIC("614d7", FATAL, Logger::ENGINES)
          << "Error reading stored version from database: "
          << rocksutils::convertStatus(s).errorMessage();
      FATAL_ERROR_EXIT();
    } else if (oldVersion.data()[0] < version) {
      // Performing 'upgrade' routine
      if (oldVersion.data()[0] == '0' && version == '1') {
        endianess = RocksDBEndianness::Little;
      } else {
        LOG_TOPIC("c30ee", FATAL, Logger::ENGINES)
            << "Your database is in an old "
            << "format. Please downgrade the server, "
            << "dump & restore the data";
        FATAL_ERROR_EXIT();
      }

    } else if (oldVersion.data()[0] > version) {
      LOG_TOPIC("c9009", FATAL, Logger::ENGINES)
          << "You are using an old version of ArangoDB, please update "
          << "before opening this database";
      FATAL_ERROR_EXIT();
    } else {
      TRI_ASSERT(oldVersion.data()[0] == version);

      // read current endianess
      rocksdb::PinnableSlice endianSlice;
      s = db->Get(rocksdb::ReadOptions(),
                  RocksDBColumnFamilyManager::get(
                      RocksDBColumnFamilyManager::Family::Definitions),
                  endianKey.string(), &endianSlice);
      if (s.ok() && endianSlice.size() == 1) {
        TRI_ASSERT(endianSlice.data()[0] == 'L' ||
                   endianSlice.data()[0] == 'B');
        endianess = static_cast<RocksDBEndianness>(endianSlice.data()[0]);
      } else {
        LOG_TOPIC("b0083", FATAL, Logger::ENGINES)
            << "Error reading key-format, your db directory is invalid";
        FATAL_ERROR_EXIT();
      }
    }
  } else {
    // new DBs are always created with Big endian data-format
    endianess = RocksDBEndianness::Big;
  }

  // enable correct key format
  TRI_ASSERT(endianess == RocksDBEndianness::Little ||
             endianess == RocksDBEndianness::Big);
  rocksutils::setRocksDBKeyFormatEndianess(endianess);

  if (!dbExisted) {
    // store endianess forever
    TRI_ASSERT(endianess == RocksDBEndianness::Big);

    char const endVal = static_cast<char>(endianess);
    static_assert(sizeof(endVal) == 1);

    rocksdb::Status s =
        db->Put(rocksdb::WriteOptions(),
                RocksDBColumnFamilyManager::get(
                    RocksDBColumnFamilyManager::Family::Definitions),
                endianKey.string(), rocksdb::Slice(&endVal, sizeof(endVal)));
    if (s.ok()) {
      // store current version
      TRI_ASSERT(version == rocksDBFormatVersion());
      static_assert(sizeof(version) == 1);

      s = db->Put(rocksdb::WriteOptions(),
                  RocksDBColumnFamilyManager::get(
                      RocksDBColumnFamilyManager::Family::Definitions),
                  versionKey.string(),
                  rocksdb::Slice(&version, sizeof(version)));
    }

    if (!s.ok()) {
      LOG_TOPIC("3d88b", FATAL, Logger::ENGINES)
          << "Error storing endianess/version: "
          << rocksutils::convertStatus(s).errorMessage();
      FATAL_ERROR_EXIT();
    }

    TRI_ASSERT(s.ok());
  }

  // fetch stored values of startup options
  auto checkSetting = [&db, &server, dbExisted](RocksDBSettingsType keyType,
                                                std::string_view optionName,
                                                bool localValue, auto cb) {
    // fetch stored value for option
    RocksDBKey extendedNamesKey;
    extendedNamesKey.constructSettingsValue(keyType);

    if (dbExisted) {
      rocksdb::PinnableSlice storedValue;
      rocksdb::Status s =
          db->Get(rocksdb::ReadOptions(),
                  RocksDBColumnFamilyManager::get(
                      RocksDBColumnFamilyManager::Family::Definitions),
                  extendedNamesKey.string(), &storedValue);

      if (s.ok() && storedValue.size() == 1) {
        if (storedValue[0] == '1') {
          if (!localValue && server.options()->processingResult().touched(
                                 std::string{optionName})) {
            // user is trying to switch back from extended names to traditional
            // names. this is unsupported
            LOG_TOPIC("1d4f6", FATAL, Logger::ENGINES)
                << "It is unsupported to change the value of the startup "
                   "option `--"
                << optionName << "`"
                << " back to `false` after it was set to `true` before. "
                << "Please remove the setting "
                   "`--"
                << optionName
                << " false` from the startup "
                   "options.";
            FATAL_ERROR_EXIT();
          }
        }
        // set flag for our local instance
        cb(storedValue[0] == '1');
      } else if (!s.IsNotFound()) {
        // arbitrary error. we need to abort
        LOG_TOPIC("f3a71", FATAL, Logger::ENGINES)
            << "Error reading stored value for --" << optionName
            << " from storage engine";
        FATAL_ERROR_EXIT();
      }
    }

    // once we have the extended names flag enabled, we must store it forever
    if (localValue) {
      // now permanently store value
      constexpr char value = '1';
      static_assert(sizeof(value) == 1);

      rocksdb::Status s = db->Put(
          rocksdb::WriteOptions(),
          RocksDBColumnFamilyManager::get(
              RocksDBColumnFamilyManager::Family::Definitions),
          extendedNamesKey.string(), rocksdb::Slice(&value, sizeof(value)));
      if (!s.ok()) {
        LOG_TOPIC("d61a8", FATAL, Logger::ENGINES)
            << "Error storing value for --" << optionName
            << " in storage engine: "
            << rocksutils::convertStatus(s).errorMessage();
        FATAL_ERROR_EXIT();
      }
    }
  };

  // read settings for extended names from persisted storage
  auto& df = server.getFeature<DatabaseFeature>();

  // --database.extended-names-databases
  checkSetting(RocksDBSettingsType::ExtendedNamesDatabases,
               "database.extended-names-databases",
               df.extendedNamesForDatabases(),
               [&df](bool value) { df.extendedNamesForDatabases(value); });

  // --database.extended-names-collections
  checkSetting(RocksDBSettingsType::ExtendedNamesCollections,
               "database.extended-names-collections",
               df.extendedNamesForCollections(),
               [&df](bool value) { df.extendedNamesForCollections(value); });

  // --database.extended-names-views
  checkSetting(RocksDBSettingsType::ExtendedNamesViews,
               "database.extended-names-views", df.extendedNamesForViews(),
               [&df](bool value) { df.extendedNamesForViews(value); });
}
