#pragma once

#include "Basics/ErrorCode.h"

#include "Replication2/ReplicatedLog/LogCommon.h"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>


// We have
//  - ReplicatedLog
//  - ReplicatedState
//
//  ReplicatedState is built on top of ReplicatedLogs
//
// We have (in the agency)
//  - Current
//  - Plan
//  - Target
//  for each of ReplicatedLog and ReplicatedState
//
//  write down structs that reflect the necessary information
//  in the agency for everything involved here
//
//  Then write (a) function(s)
// auto replicatedStateAction(Log, State) -> Action;

// we use namespacing to separate definitions from implementations as well as
// some of the noise from the essential information.
//
// For instance  this way we can write up the struct for Log:
//
// namespace log {
//    using namespace target;
//    using namespace current;
//    using namespace plan;
//
//    struct Log {
//        Target target;
//        Current current;
//        Plan plan;
//    };
// }
//
// and move the implementations into namespaced sections.
//
// In particular we do not litter struct Log {} with defining sub-structs
// and sub-sub-structs.
//
// risk: spreading the namespace out too much.
// risk: namespacetangle

namespace arangodb::replication2::replicated_state {

using RebootId = size_t;

using namespace arangodb::replication2;

struct Log {
  struct Target {

    struct Participant {
      bool forced;
    };
    using Participants = std::unordered_map<ParticipantId, Participant>;

    struct Config {
      size_t writeConcern;
      size_t softWriteConcern;
      bool waitForSync;
    };

    using Leader = std::optional<ParticipantId>;

    struct Properties {
     // ...
    };

    LogId id;
    Participants participants;
    Config config;
    Leader leader;
    Properties properties;
  };

  struct Plan {
    struct Term {
      using TermId = size_t;
      struct Leader {
        ParticipantId serverId;
        RebootId rebootId;
      };
      struct Config {
        bool waitForSync;
        size_t writeConcern;
        size_t softWriteConcern;
      };

      TermId id;
      std::optional<Leader> leader;
      Config config;
    };

    struct Participants {
      using Generation = size_t;

      struct Participant {
        bool forced;
        bool excluded;
      };
      using Set = std::unordered_map<ParticipantId, Participant>;

      Generation generation;
      Set set;
    };

    Term term;
    Participants participants;
  };

  struct Current {

    struct LocalState {
    };
    using LocalStates = std::unordered_map<ParticipantId, LocalState>;

    struct Leader {
      using Term = size_t;

      struct Participants {
        using Generation = size_t;

        Generation generation;
        // TODO: probably missing, participants
      };

      Term term;
      Participants participants;
    };

    struct Supervision {
      // TODO.
    };

    LocalStates localStates;
    Leader leader;
    Supervision supervision;
  };

  Target target;
  Plan plan;
  Current current;
};

struct State {
  struct Target {

    using StateId = size_t;
    struct Properties {
      enum class Hash { Crc32 };
      enum class Implementation { DocumentStore };

      Hash hash;
      Implementation implementation;
    };
    struct Configuration {
      bool waitForSync;
      size_t writeConcern;
      size_t softWriteConcern;
    };
    struct Participant {
      // TODO
    };
    using Participants = std::unordered_map<ParticipantId, Participant>;

    StateId id;
    Properties properties;
    Configuration configuration;
    Participants participants;
  };
  struct Plan {
    using PlanId = size_t;
    using Generation = size_t;
    struct Participant {
      Generation generation;
    };
    using Participants = std::unordered_map<ParticipantId, Participant>;

    PlanId id;
    Generation generation;
    Participants participants;
  };
  struct Current {
    using CurrentId = size_t;

    struct Participant {
      using Generation = size_t;
      struct Snapshot {
        enum class Status { Completed, InProgreess, Failed };
        struct Timestamp {
        // TODO std::chrono?
        };

        Status status;
        Timestamp timestamp;
      };

      Generation generation;
      Snapshot snapshot;
    };
    using Participants = std::unordered_map<ParticipantId, Participant>;

    CurrentId id;
    Participants participants;
  };

  Target target;
  Plan plan;
  Current current;
};


struct ParticipantHealth {
  RebootId rebootId;
  bool isHealthy;
};

using ParticipantsHealth = std::unordered_map<ParticipantId, ParticipantHealth>;



struct Action {
  virtual void execute() = 0;
  virtual ~Action() = default;
};

struct UpdateTermAction : Action {
  UpdateTermAction(Log::Plan::Term const& newTerm) : _newTerm(newTerm) {};
  void execute() override {};

  Log::Plan::Term _newTerm;
};

auto replicatedLogAction(Log const&, ParticipantsHealth const&) -> std::unique_ptr<Action>;

}
