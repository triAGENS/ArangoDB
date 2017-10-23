'use strict';

const db = require('@arangodb').db;
const name = 'foxx_queue_test';

if (!db._collection(name)) {
  db._createDocumentCollection(name);
}

const queues = require('@arangodb/foxx/queues');
const queue2 = queues.create('test_queue');
