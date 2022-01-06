/*jshint strict: true */
/*global assertTrue */
'use strict';

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License")
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
const jsunity = require('jsunity');
const arangodb = require("@arangodb");
const _ = require('lodash');
const db = arangodb.db;
const {
    waitFor,
    readReplicatedLogAgency,
    replicatedLogSetPlan,
    replicatedLogDeletePlan,
    replicatedLogUpdatePlanParticipantsFlags,
    replicatedLogSetPlanTerm,
    createTermSpecification,
    replicatedLogIsReady,
    dbservers,
    nextUniqueLogId, testConfigurationString,
    registerAgencyTestBegin, registerAgencyTestEnd
} = require("@arangodb/testutils/replicated-logs-helper");

const database = 'ReplLogsMaintenanceTest';

const replicatedLogParticipantGeneration = function (logId, generation) {
    return function () {
        let {current} = readReplicatedLogAgency(database, logId);
        if (current === undefined) {
            return Error("current not yet defined");
        }
        if (!current.leader) {
            return Error("Leader has not yet established its term");
        }
        if (!current.leader.committedParticipantsConfig) {
            return Error("Leader has not yet committed any participants config");
        }
        if (current.leader.committedParticipantsConfig.generation < generation) {
            return Error("Leader has not yet acked new generation; "
                + `found ${current.leader.committedParticipantsConfig.generation}, expected = ${generation}`);
        }

        return true;
    };
};

const replicatedLogParticipantsFlag = function (logId, flags, generation = undefined) {
    return function () {
        let {current} = readReplicatedLogAgency(database, logId);
        if (current === undefined) {
            return Error("current not yet defined");
        }
        if (!current.leader) {
            return Error("Leader has not yet established its term");
        }
        if (!current.leader.committedParticipantsConfig) {
            return Error("Leader has not yet committed any participants config");
        }
        if (generation !== undefined) {
            if (current.leader.committedParticipantsConfig.generation < generation) {
                return Error("Leader has not yet acked new generation; "
                    + `found ${current.leader.committedParticipantsConfig.generation}, expected = ${generation}`);
            }
        }

        const participants = current.leader.committedParticipantsConfig.participants;
        for (const [p, v] of Object.entries(flags)) {
            if (v === null) {
                if (participants[p] !== undefined) {
                    return Error(`Entry for server ${p} still present in participants flags`);
                }
            } else {
                if (!_.isEqual(v, participants[p])) {
                    return Error(`Flags for participant ${p} are not as expected: ${JSON.stringify(v)} vs. ${JSON.stringify(participants[p])}`);
                }
            }
        }

        return true;
    };
};

const replicatedLogSuite = function (targetConfig) {

    const {setUpAll, tearDownAll} = (function () {
        let previousDatabase, databaseExisted = true;
        return {
            setUpAll: function () {
                previousDatabase = db._name();
                if (!_.includes(db._databases(), database)) {
                    db._createDatabase(database);
                    databaseExisted = false;
                }
                db._useDatabase(database);
            },

            tearDownAll: function () {
                db._useDatabase(previousDatabase);
                if (!databaseExisted) {
                    db._dropDatabase(database);
                }
            },
        };
    }());

    return {
        setUpAll, tearDownAll,
        setUp: registerAgencyTestBegin,
        tearDown: registerAgencyTestEnd,

        testCreateReplicatedLog: function () {
            const logId = nextUniqueLogId();
            const servers = _.sampleSize(dbservers, targetConfig.replicationFactor);
            const leader = servers[0];
            const term = 1;
            replicatedLogSetPlan(database, logId, {
                id: logId,
                targetConfig,
                currentTerm: createTermSpecification(term, servers, targetConfig, leader),
            });

            // wait for all servers to have reported in current
            waitFor(replicatedLogIsReady(database, logId, term, servers, leader));

            replicatedLogDeletePlan(database, logId);
        },

        testCreateReplicatedLogWithoutLeader: function () {
            const logId = nextUniqueLogId();
            const servers = _.sampleSize(dbservers, targetConfig.replicationFactor);
            const term = 1;
            replicatedLogSetPlan(database, logId, {
                id: logId,
                targetConfig,
                currentTerm: createTermSpecification(term, servers, targetConfig),
            });

            // wait for all servers to have reported in current
            waitFor(replicatedLogIsReady(database, logId, term, servers));

            replicatedLogDeletePlan(database, logId);
        },

        testAddParticipantFlagForced: function () {
            if (targetConfig.replicationFactor < 2) {
                return;
            }
            const logId = nextUniqueLogId();
            const servers = _.sampleSize(dbservers, targetConfig.replicationFactor);
            const leader = servers[0];
            const term = 1;
            replicatedLogSetPlan(database, logId, {
                id: logId,
                targetConfig,
                currentTerm: createTermSpecification(term, servers, targetConfig, leader),
            });

            // wait for all servers to have reported in current
            waitFor(replicatedLogIsReady(database, logId, term, servers, leader));

            // now update the excluded flag for one participant
            const follower = servers[1];
            let newGeneration = replicatedLogUpdatePlanParticipantsFlags(database, logId, {[follower]: {forced: true}});
            waitFor(replicatedLogParticipantsFlag(logId, {[follower]: {forced: true, excluded: false}}, newGeneration));

            newGeneration = replicatedLogUpdatePlanParticipantsFlags(database, logId, {[follower]: null});
            waitFor(replicatedLogParticipantGeneration(logId, newGeneration));

            waitFor(replicatedLogParticipantsFlag(logId, {[follower]: null}, newGeneration));
            replicatedLogDeletePlan(database, logId);
        },

        testAddParticipantFlagExcluded: function () {
            // this test only works if not all servers are required for writeConcern
            if (targetConfig.writeConcern >= targetConfig.replicationFactor || targetConfig.replicationFactor < 2) {
                return;
            }

            const logId = nextUniqueLogId();
            const servers = _.sampleSize(dbservers, targetConfig.replicationFactor);
            const leader = servers[0];
            const term = 1;
            replicatedLogSetPlan(database, logId, {
                id: logId,
                targetConfig,
                currentTerm: createTermSpecification(term, servers, targetConfig, leader),
            });

            // wait for all servers to have reported in current
            waitFor(replicatedLogIsReady(database, logId, term, servers, leader));

            // now update the excluded flag for one participant
            const follower = servers[1];
            let newGeneration = replicatedLogUpdatePlanParticipantsFlags(database, logId, {[follower]: {excluded: true}});
            waitFor(replicatedLogParticipantsFlag(logId, {[follower]: {excluded: true, forced: false}}, newGeneration));

            newGeneration = replicatedLogUpdatePlanParticipantsFlags(database, logId, {[follower]: null});
            waitFor(replicatedLogParticipantGeneration(logId, newGeneration));

            waitFor(replicatedLogParticipantsFlag(logId, {[follower]: null}, newGeneration));
            replicatedLogDeletePlan(database, logId);
        },

        testUpdateTermInPlanLog: function () {
            const logId = nextUniqueLogId();
            const servers = _.sampleSize(dbservers, targetConfig.replicationFactor);
            const leader = servers[0];
            const term = 1;
            replicatedLogSetPlan(database, logId, {
                id: logId,
                targetConfig,
                currentTerm: createTermSpecification(term, servers, targetConfig, leader),
            });

            // wait for all servers to have reported in current
            waitFor(replicatedLogIsReady(database, logId, term, servers, leader));
            replicatedLogSetPlanTerm(database, logId, createTermSpecification(term + 1, servers, targetConfig, leader));

            // wait again for all servers to have acked term
            waitFor(replicatedLogIsReady(database, logId, term + 1, servers, leader));
            replicatedLogDeletePlan(database, logId);
        },

        testUpdateTermInPlanLogWithNewLeader: function () {
            if (targetConfig.replicationFactor < 2) {
                return;
            }
            const logId = nextUniqueLogId();
            const servers = _.sampleSize(dbservers, targetConfig.replicationFactor);
            const leader = servers[0];
            const term = 1;
            replicatedLogSetPlan(database, logId, {
                id: logId,
                targetConfig,
                currentTerm: createTermSpecification(term, servers, targetConfig, leader),
            });

            // wait for all servers to have reported in current
            waitFor(replicatedLogIsReady(database, logId, term, servers));
            // wait again for all servers to have acked term
            const otherLeader = servers[1];
            replicatedLogSetPlanTerm(database, logId, createTermSpecification(term + 1, servers, targetConfig, otherLeader));
            waitFor(replicatedLogIsReady(database, logId, term + 1, servers, otherLeader));
            replicatedLogDeletePlan(database, logId);
        },

        testUpdateTermAddParticipant: function () {
            const logId = nextUniqueLogId();
            const servers = _.sampleSize(dbservers, targetConfig.replicationFactor);
            const leader = servers[0];
            const remaining = _.difference(dbservers, servers);
            const term = 1;
            replicatedLogSetPlan(database, logId, {
                id: logId,
                targetConfig,
                currentTerm: createTermSpecification(term, servers, targetConfig, leader),
            });

            // wait for all servers to have reported in current
            waitFor(replicatedLogIsReady(database, logId, term, servers));
            // now rewrite the term with an additional participant
            const newServers = [...servers, _.sample(remaining)];
            replicatedLogSetPlanTerm(database, logId, createTermSpecification(term, newServers, targetConfig, leader));
            waitFor(replicatedLogIsReady(database, logId, term, newServers, leader));
            replicatedLogDeletePlan(database, logId);
        },

        testUpdateTermRemoveParticipant: function () {
            const logId = nextUniqueLogId();
            const servers = _.sampleSize(dbservers, targetConfig.replicationFactor);
            const remaining = _.difference(dbservers, servers);
            const toBeRemoved = _.sample(remaining);
            const leader = servers[0];
            const term = 1;
            const newServers = [...servers, toBeRemoved];
            replicatedLogSetPlan(database, logId, {
                id: logId,
                targetConfig,
                currentTerm: createTermSpecification(term, newServers, targetConfig, leader),
            });

            // wait for all servers to have reported in current
            waitFor(replicatedLogIsReady(database, logId, term, newServers));
            // now rewrite the term with an additional participant
            replicatedLogSetPlanTerm(database, logId, createTermSpecification(term, servers, targetConfig, leader));
            // TODO waitFor(replicatedLogParticipantsFlag(logId, {[toBeRemoved]: null})); -- doesn't work yet
            replicatedLogDeletePlan(database, logId);
        },
    };
};

const instantiateTestSuite = function (suite, config) {
    const proto = suite(config);
    const testName = (name) => testConfigurationString(name, config);
    let object = {};
    for (const [name, test] of Object.entries(proto)) {
        if (["setUp", "tearDown", "setUpAll", "tearDownAll"].indexOf(name) !== -1) {
            object[name] = test;
        } else {
            object[testName(name)] = test;
        }
    }
    return function () {
        return object;
    };
};


const testConfigurations = [
    {
        writeConcern: 1,
        softWriteConcern: 1,
        replicationFactor: 1,
        waitForSync: false,
    },
    {
        writeConcern: 1,
        softWriteConcern: 2,
        replicationFactor: 3,
        waitForSync: false,
    },
    {
        writeConcern: 2,
        softWriteConcern: 2,
        replicationFactor: 3,
        waitForSync: false,
    },
    {
        writeConcern: 1,
        softWriteConcern: 2,
        replicationFactor: 4,
        waitForSync: false,
    },
    {
        writeConcern: 3,
        softWriteConcern: 3,
        replicationFactor: 4,
        waitForSync: false,
    },
    {
        writeConcern: 4,
        softWriteConcern: 4,
        replicationFactor: 4,
        waitForSync: false,
    }
];



for (const config of testConfigurations) {
    jsunity.run(instantiateTestSuite(replicatedLogSuite, config));
}

return jsunity.done();
