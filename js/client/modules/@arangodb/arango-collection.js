/*jshint strict: false */

// //////////////////////////////////////////////////////////////////////////////
// / @brief ArangoCollection
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2013 triagens GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License")
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     http://www.apache.org/licenses/LICENSE-2.0
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is triAGENS GmbH, Cologne, Germany
// /
// / @author Achim Brandt
// / @author Dr. Frank Celler
// / @author Copyright 2012-2013, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const internal = require('internal');
const arangosh = require('@arangodb/arangosh');
let engine = null;

function getEngine(db) {
  if (engine === null) {
    try {
      let requestResult = db._connection.GET('/_api/engine');
      if (requestResult && requestResult.name) {
        engine = requestResult.name;
      }
    } catch (err) {
      // ignore any errors
    }
  }
  return engine;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief add options from arguments to index specification
// //////////////////////////////////////////////////////////////////////////////

function addIndexOptions (body, parameters) {
  body.fields = [];

  var setOption = function (k) {
    if (! body.hasOwnProperty(k)) {
      body[k] = parameters[i][k];
    }
  };

  var i;
  for (i = 0; i < parameters.length; ++i) {
    if (typeof parameters[i] === 'string') {
      // set fields
      body.fields.push(parameters[i]);
    }
    else if (typeof parameters[i] === 'object' &&
      ! Array.isArray(parameters[i]) &&
      parameters[i] !== null) {
      // set arbitrary options
      Object.keys(parameters[i]).forEach(setOption);
      break;
    }
  }

  return body;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief constructor
// //////////////////////////////////////////////////////////////////////////////

function ArangoCollection (database, data) {
  this._database = database;
  this._dbName = database._name();
  this._dbPrefix = '/_db/' + encodeURIComponent(database._name());

  if (typeof data === 'string') {
    this._id = null;
    this._name = data;
    this._status = null;
    this._type = null;
  }
  else if (data !== undefined) {
    this._id = data.id;
    this._name = data.name;
    this._status = data.status;
    this._type = data.type;
  }else {
    this._id = null;
    this._name = null;
    this._status = null;
    this._type = null;
  }
}

exports.ArangoCollection = ArangoCollection;

// must be called after exporting ArangoCollection
require('@arangodb/arango-collection-common');

var ArangoError = require('@arangodb').ArangoError;

// //////////////////////////////////////////////////////////////////////////////
// / @brief append the waitForSync parameter to a URL
// //////////////////////////////////////////////////////////////////////////////

let appendSyncParameter = function (url, waitForSync) {
  if (waitForSync) {
    if (url.indexOf('?') === -1) {
      url += '?';
    }else {
      url += '&';
    }
    url += 'waitForSync=true';
  }
  return url;
};

let appendOverwriteModeParameter = function (url, mode) {
  if (mode) {
    if (url.indexOf('?') === -1) {
      url += '?';
    }else {
      url += '&';
    }
    url += 'overwriteMode=' + mode;
  }
  return url;
};


// //////////////////////////////////////////////////////////////////////////////
// / @brief append some boolean parameter to a URL
// //////////////////////////////////////////////////////////////////////////////

let appendBoolParameter = function (url, name, val) {
  if (url.indexOf('?') === -1) {
    url += '?';
  }else {
    url += '&';
  }
  url += name + (val ? '=true' : '=false');
  return url;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief prefix a URL with the database name of the collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype._prefixurl = function (url) {
  if (url.substr(0, 5) === '/_db/') {
    return url;
  }

  if (url[0] === '/') {
    return this._dbPrefix + url;
  }
  return this._dbPrefix + '/' + url;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief return the base url for collection usage
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype._baseurl = function (suffix) {
  var url = this._database._collectionurl(this.name());

  if (suffix) {
    url += '/' + suffix;
  }

  return this._prefixurl(url);
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief return the base url for document usage
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype._documenturl = function (id) {
  var s = id.split('/'), url;
  var name = this.name();
  if (s.length === 1) {
    url = this._database._documenturl(name + '/' + id, name);
  }else {
    url = this._database._documenturl(id, name);
  }

  return this._prefixurl(url);
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief return the base url for document usage with collection name
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype._documentcollectionurl = function () {
  return this._prefixurl('/_api/document/' + this.name());
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief return the base url for collection index usage
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype._indexurl = function () {
  return this._prefixurl('/_api/index?collection=' + encodeURIComponent(this.name()));
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief executes an edge query
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype._edgesQuery = function (vertex, direction) {
  if (!(vertex instanceof Array)) {
    vertex = [vertex];
  }
  vertex = vertex.map(function (v) {
    if (v.hasOwnProperty('_id')) {
      return v._id;
    }
    return v;
  });

  // get the edges
  var url = '/_api/edges/' + encodeURIComponent(this.name())
  + (direction ? '?direction=' + direction : '');

  var requestResult = this._database._connection.POST(this._prefixurl(url), vertex);

  arangosh.checkRequestResult(requestResult);

  return requestResult.edges;
};

ArangoCollection.prototype.shards = function (details) {
  var requestResult = this._database._connection.GET(this._baseurl('shards') + '?details=' + (details ? 'true' : 'false'));

  arangosh.checkRequestResult(requestResult);

  return requestResult.shards;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief converts into an array
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.toArray = function () {
  return this.all().toArray();
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief print the help for ArangoCollection
// //////////////////////////////////////////////////////////////////////////////

var helpArangoCollection = arangosh.createHelpHeadline('ArangoCollection help') +
  'ArangoCollection constructor:                                             ' + '\n' +
  ' > col = db.mycoll;                                                       ' + '\n' +
  ' > col = db._create("mycoll");                                            ' + '\n' +
  '                                                                          ' + '\n' +
  'Administration Functions:                                                 ' + '\n' +
  '  name()                                collection name                   ' + '\n' +
  '  status()                              status of the collection          ' + '\n' +
  '  type()                                type of the collection            ' + '\n' +
  '  truncate()                            remove all documents              ' + '\n' +
  '  properties()                          show collection properties        ' + '\n' +
  '  properties(<data>)                    change collection properties      ' + '\n' +
  '  drop()                                delete a collection               ' + '\n' +
  '  load()                                load a collection                 ' + '\n' +
  '  unload()                              unload a collection               ' + '\n' +
  '  rename(<new-name>)                    renames a collection              ' + '\n' +
  '  getIndexes()                          return defined indexes            ' + '\n' +
  '  refresh()                             refresh the status and name       ' + '\n' +
  '  _help()                               this help                         ' + '\n' +
  '                                                                          ' + '\n' +
  'Document Functions:                                                       ' + '\n' +
  '  count()                               return number of documents        ' + '\n' +
  '  save(<data>)                          create document and return handle ' + '\n' +
  '  document(<id>)                        get document by handle (_id or _key)' + '\n' +
  '  replace(<id>, <data>, <overwrite>)    overwrite document                ' + '\n' +
  '  update(<id>, <data>, <overwrite>,     partially update document         ' + '\n' +
  '         <keepNull>)                                                      ' + '\n' +
  '  remove(<id>)                          remove document                   ' + '\n' +
  '  exists(<id>)                          check whether a document exists   ' + '\n' +
  '                                                                          ' + '\n' +
  'Attributes:                                                               ' + '\n' +
  '  _database                             database object                   ' + '\n' +
  '  _id                                   collection identifier             ';

ArangoCollection.prototype._help = function () {
  internal.print(helpArangoCollection);
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets the name of a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.name = function () {
  if (this._name === null) {
    this.refresh();
  }

  return this._name;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets the status of a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.status = function () {
  if (this._status === null) {
    this._status = null;
    this.refresh();
  }

  // save original status
  return this._status;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets the type of a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.type = function () {
  if (this._type === null) {
    this.refresh();
  }

  return this._type;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets or sets the properties of a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.properties = function (properties) {
  var attributes = {
    'globallyUniqueId': false,
    'isSmart': false,
    'isSystem': false,
    'waitForSync': true,
    'shardKeys': false,
    'smartGraphAttribute': false,
    'smartJoinAttribute': false,
    'numberOfShards': false,
    'keyOptions': false,
    'replicationFactor': true,
    'minReplicationFactor': true,
    'writeConcern': true,
    'distributeShardsLike': false,
    'shardingStrategy': false,
    'cacheEnabled': true,
    'computedValues': true,
    'syncByRevision': false,
    'schema' : true,
    'isDisjoint': false,
  };
  var a;

  var requestResult;
  if (properties === undefined) {
    requestResult = this._database._connection.GET(this._baseurl('properties'));

    arangosh.checkRequestResult(requestResult);
  } else {
    var body = {};

    for (a in attributes) {
      if (attributes.hasOwnProperty(a) &&
        attributes[a] &&
        properties.hasOwnProperty(a)) {
        body[a] = properties[a];
      }
    }

    requestResult = this._database._connection.PUT(this._baseurl('properties'), body);

    arangosh.checkRequestResult(requestResult);
  }

  var result = { };
  for (a in attributes) {
    if (attributes.hasOwnProperty(a) &&
      requestResult.hasOwnProperty(a) &&
      requestResult[a] !== undefined) {
      result[a] = requestResult[a];
    }
  }

  return result;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief recalculate counts of a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.recalculateCount = function () {
  var requestResult = this._database._connection.PUT(this._baseurl('recalculateCount'), null);

  arangosh.checkRequestResult(requestResult);

  return requestResult.result;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets the figures of a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.figures = function (details) {
  var requestResult = this._database._connection.GET(this._baseurl('figures') + '?details=' + (details ? 'true' : 'false'));

  arangosh.checkRequestResult(requestResult);

  return requestResult.figures;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets the responsible shard for a specific value
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.getResponsibleShard = function (data) {
  if (data === undefined || data === null) {
    data = {};
  } else if (typeof data === 'string' || typeof data === 'number') {
    data = { _key: String(data) };
  }
  var requestResult = this._database._connection.PUT(this._baseurl('responsibleShard'), data);

  arangosh.checkRequestResult(requestResult);

  return requestResult.shardId;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets the checksum of a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.checksum = function (withRevisions, withData) {
  var append = '';
  if (withRevisions) {
    append += '?withRevisions=true';
  }
  if (withData) {
    append += (append === '' ? '?' : '&') + 'withData=true';
  }
  var requestResult = this._database._connection.GET(this._baseurl('checksum') + append);

  arangosh.checkRequestResult(requestResult);

  return {
    checksum: requestResult.checksum,
    revision: requestResult.revision
  };
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets the revision id of a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.revision = function () {
  var requestResult = this._database._connection.GET(this._baseurl('revision'));

  arangosh.checkRequestResult(requestResult);

  return requestResult.revision;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief drops a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.drop = function (options) {
  var requestResult;
  if (typeof options === 'object' && options.isSystem) {
    requestResult = this._database._connection.DELETE(this._baseurl() + '?isSystem=true');
  } else {
    requestResult = this._database._connection.DELETE(this._baseurl());
  }

  if (requestResult !== null
    && requestResult !== undefined
    && requestResult.error === true
    && requestResult.errorNum !== internal.errors.ERROR_ARANGO_DATA_SOURCE_NOT_FOUND.code) {
    // check error in case we got anything else but "collection not found"
    arangosh.checkRequestResult(requestResult);
  }

  this._status = ArangoCollection.STATUS_DELETED;

  var database = this._database;
  var name;

  for (name in database) {
    if (database.hasOwnProperty(name)) {
      var collection = database[name];

      if (collection instanceof ArangoCollection) {
        if (collection.name() === this.name()) {
          delete database[name];
        }
      }
    }
  }
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief truncates a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.truncate = function (options) {
  if (typeof options === 'boolean') {
    options = { waitForSync: options || false };
  } else {
    options = options || {};
  }
  if (!options.hasOwnProperty('compact')) {
    options.compact = true;
  }
  let headers = {};
  if (options && options.transactionId) {
    headers['x-arango-trx-id'] = options.transactionId;
  }

  var append = (options.waitForSync ? '?waitForSync=true' : '');
  append += (append === '') ? '?' : '&' + (options.compact ? 'compact=true' : 'compact=false');
  var requestResult = this._database._connection.PUT(this._baseurl('truncate') + append, null, headers);
  arangosh.checkRequestResult(requestResult);
  // invalidate cache
  this._status = null;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief compacts a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.compact = function () {
  let requestResult = this._database._connection.PUT(this._baseurl('compact'), null);

  arangosh.checkRequestResult(requestResult);
  // invalidate cache
  this._status = null;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief loads a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.load = function (count) {
  var data = { count: true };

  // return the number of documents? this might slow down loading
  if (count !== undefined) {
    data.count = count;
  }

  var requestResult = this._database._connection.PUT(this._baseurl('load'), data);

  arangosh.checkRequestResult(requestResult);

  // invalidate cache
  this._status = null;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief unloads a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.unload = function () {
  this._status = null;
  var requestResult = this._database._connection.PUT(this._baseurl('unload'), null);
  this._status = null;

  arangosh.checkRequestResult(requestResult);
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief renames a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.rename = function (name) {
  var body = { name: name };
  var requestResult = this._database._connection.PUT(this._baseurl('rename'), body);

  arangosh.checkRequestResult(requestResult);

  delete this._database[this._name];
  this._database[name] = this;

  this._status = null;
  this._name = null;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief refreshes a collection status and name
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.refresh = function () {
  var requestResult = this._database._connection.GET(this._database._collectionurl(this._id));

  arangosh.checkRequestResult(requestResult);

  this._name = requestResult.name;
  this._status = requestResult.status;
  this._type = requestResult.type;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets all indexes
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.getIndexes = ArangoCollection.prototype.indexes = function (withStats, withHidden) {
  let url = this._indexurl() + '&withStats=' + (withStats || false);
  if (withHidden) {
    url += '&withHidden=true';
  }
  var requestResult = this._database._connection.GET(url);

  arangosh.checkRequestResult(requestResult);

  return requestResult.indexes;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets one index
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.index = function (id) {
  if (id.hasOwnProperty('id')) {
    id = id.id;
  } else if (id.hasOwnProperty('name')) {
    id = id.name;
  }

  var requestResult = this._database._connection.GET(this._database._indexurl(id, this.name()));

  arangosh.checkRequestResult(requestResult);

  return requestResult;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief deletes an index
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.dropIndex = function (id) {
  if (id.hasOwnProperty('id')) {
    id = id.id;
  } else if (id.hasOwnProperty('name')) {
    id = id.name;
  }

  var requestResult = this._database._connection.DELETE(this._database._indexurl(id, this.name()));

  if (requestResult !== null
    && requestResult.error === true
    && requestResult.errorNum
    === internal.errors.ERROR_ARANGO_INDEX_NOT_FOUND.code) {
    return false;
  }

  arangosh.checkRequestResult(requestResult);

  return true;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief ensures an index
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.ensureIndex = function (data) {
  if (typeof data !== 'object' || Array.isArray(data)) {
    throw 'usage: ensureIndex(<description>)';
  }

  var requestResult = this._database._connection.POST(this._indexurl(), data);

  arangosh.checkRequestResult(requestResult);

  return requestResult;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief updates an index
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.updateIndex = function (id, data) {
  if (typeof data !== 'object' || Array.isArray(data)) {
    throw 'usage: ensureIndex(<id>, <description>)';
  }
  
   if (id.hasOwnProperty('id')) {
    id = id.id;
  } else if (id.hasOwnProperty('name')) {
    id = id.name;
  }

  var requestResult = this._database._connection.PATCH(
    this._database._indexurl(id, this.name()), data);

  arangosh.checkRequestResult(requestResult);

  return requestResult;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets the number of documents
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.count = function (details) {
  var requestResult;
  if (details) {
    requestResult = this._database._connection.GET(this._baseurl('count') + '?details=true');
  } else {
    requestResult = this._database._connection.GET(this._baseurl('count'));
  }
  arangosh.checkRequestResult(requestResult);

  return requestResult.count;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets a single document from the collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.document = function (id, options) {
  var rev = null;
  var requestResult;

  if (id === undefined || id === null) {
    throw new ArangoError({
      errorNum: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.code,
      errorMessage: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.message
    });
  }

  let headers = {};
  if (options) {
    if (options.transactionId) {
      headers['x-arango-trx-id'] = options.transactionId;
    }
    if (options.allowDirtyReads) {
      headers['x-arango-allow-dirty-read'] = "true";
    }
  }

  if (Array.isArray(id)) {
    var url = this._documentcollectionurl() + '?onlyget=true&ignoreRevs=false';
    requestResult = this._database._connection.PUT(url, id, headers);
  } else {
    if (typeof id === 'object') {
      if (id.hasOwnProperty('_rev')) {
        rev = id._rev;
      }
      if (id.hasOwnProperty('_id')) {
        id = id._id;
      } else if (id.hasOwnProperty('_key')) {
        id = id._key;
      }
    }
    if (rev !== null) {
      headers['if-match'] = JSON.stringify(rev);
    }
    requestResult = this._database._connection.GET(this._documenturl(id), headers);
  }

  if (requestResult !== null && requestResult.error === true) {
    if (requestResult.errorNum === internal.errors.ERROR_HTTP_PRECONDITION_FAILED.code) {
      requestResult.errorNum = internal.errors.ERROR_ARANGO_CONFLICT.code;
    }
  }

  arangosh.checkRequestResult(requestResult);

  return requestResult;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief checks whether a specific document exists
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.exists = function (id, options) {
  var rev = null;
  var requestResult;

  if (id === undefined || id === null) {
    throw new ArangoError({
      errorNum: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.code,
      errorMessage: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.message
    });
  }

  if (typeof id === 'object') {
    if (id.hasOwnProperty('_rev')) {
      rev = id._rev;
    }
    if (id.hasOwnProperty('_id')) {
      id = id._id;
    } else if (id.hasOwnProperty('_key')) {
      id = id._key;
    }
  }

  let headers = {};
  if (options) {
    if (options.transactionId) {
      headers['x-arango-trx-id'] = options.transactionId;
    }
    if (options.allowDirtyReads) {
      headers['x-arango-allow-dirty-read'] = "true";
    }
  }

  if (rev === null) {
    requestResult = this._database._connection.GET(this._documenturl(id), headers);
  }else {
    headers['if-match'] = JSON.stringify(rev);
    requestResult = this._database._connection.GET(this._documenturl(id),
      headers);
  }

  if (requestResult !== null && requestResult.error === true) {
    if (requestResult.errorNum === internal.errors.ERROR_ARANGO_DOCUMENT_NOT_FOUND.code) {
      return false;
    }
    if (requestResult.errorNum === internal.errors.ERROR_HTTP_PRECONDITION_FAILED.code) {
      requestResult.errorNum = internal.errors.ERROR_ARANGO_CONFLICT.code;
    }
  }

  arangosh.checkRequestResult(requestResult);

  return {_id: requestResult._id, _key: requestResult._key,
  _rev: requestResult._rev};
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief gets a random element from the collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.any = function () {
  var requestResult = this._database._connection.PUT(
    this._prefixurl('/_api/simple/any'),
    { collection: this._name });

  arangosh.checkRequestResult(requestResult);

  return requestResult.document;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief constructs a query-by-example for a collection
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.firstExample = function (example) {
  var e;
  var i;

  // example is given as only argument
  if (arguments.length === 1) {
    e = example;
  }

  // example is given as list
  else {
    e = {};

    for (i = 0;  i < arguments.length;  i += 2) {
      e[arguments[i]] = arguments[i + 1];
    }
  }

  var data = {
    collection: this.name(),
    example: e
  };

  var requestResult = this._database._connection.PUT(
    this._prefixurl('/_api/simple/first-example'),
    data
  );

  if (requestResult !== null
    && requestResult.error === true
    && requestResult.errorNum === internal.errors.ERROR_HTTP_NOT_FOUND.code) {
    return null;
  }

  arangosh.checkRequestResult(requestResult);

  return requestResult.document;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief saves a document in the collection
// / note: this method is used to save documents and edges, but save() has a
// / different signature for both. For document collections, the signature is
// / save(<data>, <options>), whereas for edge collections, the signature is
// / save(<from>, <to>, <data>, <options>)
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.save =
  ArangoCollection.prototype.insert = function (from, to, data, options) {
    let type = this.type(), url;

    if (type === undefined) {
      type = ArangoCollection.TYPE_DOCUMENT;
    }

    if (type === ArangoCollection.TYPE_DOCUMENT || data === undefined) {
      data = from;
      options = to;
    }
    else if (type === ArangoCollection.TYPE_EDGE) {
      if (typeof data === 'object' && Array.isArray(data)) {
        throw new ArangoError({
          errorNum: internal.errors.ERROR_ARANGO_DOCUMENT_TYPE_INVALID.code,
          errorMessage: internal.errors.ERROR_ARANGO_DOCUMENT_TYPE_INVALID.message
        });
      }
      if (data === undefined || data === null || typeof data !== 'object') {
        throw new ArangoError({
          errorNum: internal.errors.ERROR_ARANGO_DOCUMENT_TYPE_INVALID.code,
          errorMessage: internal.errors.ERROR_ARANGO_DOCUMENT_TYPE_INVALID.message
        });
      }

      if (typeof from === 'object' && from.hasOwnProperty('_id')) {
        from = from._id;
      }

      if (typeof to === 'object' && to.hasOwnProperty('_id')) {
        to = to._id;
      }

      data._from = from;
      data._to = to;
    }

    url = this._dbPrefix + '/_api/document/' + encodeURIComponent(this.name());

    if (options === undefined) {
      options = {};
    }

    // the following parameters are optional, so we only append them if necessary
    if (options.waitForSync) {
      url = appendSyncParameter(url, options.waitForSync);
    }

    ["skipDocumentValidation", "returnNew", "returnOld", "silent", "overwrite", "isRestore"].forEach(function(key) {
      if (options[key]) {
        url = appendBoolParameter(url, key, options[key]);
      }
    });

    if (options.overwriteMode) {
      url = appendOverwriteModeParameter(url, options.overwriteMode);

      if (options.keepNull) {
        url = appendBoolParameter(url, 'keepNull', options.keepNull);
      }

      if (options.mergeObjects !== undefined) {
        url = appendBoolParameter(url, 'mergeObjects', options.mergeObjects);
      }
    }

    let headers = {};
    if (options.transactionId) {
      headers['x-arango-trx-id'] = options.transactionId;
    }

    if (data === undefined || typeof data !== 'object') {
      throw new ArangoError({
        errorNum: internal.errors.ERROR_ARANGO_DOCUMENT_TYPE_INVALID.code,
        errorMessage: internal.errors.ERROR_ARANGO_DOCUMENT_TYPE_INVALID.message
      });
    }

    let requestResult = this._database._connection.POST(
      url, data, headers
    );

    arangosh.checkRequestResult(requestResult);

    return options.silent ? true : requestResult;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief removes a document in the collection
// / @param id the id of the document
// / @param overwrite (optional) a boolean value or a json object
// / @param waitForSync (optional) a boolean value .
// / @example remove("example/996280832675")
// / @example remove("example/996280832675", true)
// / @example remove("example/996280832675", false)
// / @example remove("example/996280832675", {waitForSync: false, overwrite: true})
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.remove = function (id, overwrite, waitForSync) {
  let rev = null;

  if (id === undefined || id === null) {
    throw new ArangoError({
      errorNum: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.code,
      errorMessage: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.message
    });
  }

  let ignoreRevs = false;
  let options;
  if (typeof overwrite === 'object') {
    // we assume the caller uses new signature (id, data, options)
    if (typeof waitForSync !== 'undefined') {
      throw 'too many arguments';
    }
    options = overwrite;
    if (options.hasOwnProperty('overwrite') && options.overwrite) {
      ignoreRevs = true;
    }
    if (options.hasOwnProperty('waitForSync')) {
      waitForSync = options.waitForSync;
    }
  } else {
    if (overwrite) {
      ignoreRevs = true;
    }
    options = {};
  }

  if (ignoreRevs) {
    delete id._rev;
  }

  let url;
  let body = null;

  if (Array.isArray(id)) {
    url = this._documentcollectionurl();
    body = id;
  } else {
    if (typeof id === 'object') {
      if (id.hasOwnProperty('_rev')) {
        rev = id._rev;
      }
      if (id.hasOwnProperty('_id')) {
        id = id._id;
      } else if (id.hasOwnProperty('_key')) {
        id = id._key;
      }
    }
    url = this._documenturl(id);
  }

  url = appendBoolParameter(url, 'ignoreRevs', ignoreRevs);
  // the following parameters are optional, so we only append them if necessary

  if (options.returnOld) {
    url = appendBoolParameter(url, 'returnOld', options.returnOld);
  }
  if (options.silent) {
    url = appendBoolParameter(url, 'silent', options.silent);
  }

  let headers = {};
  if (options.transactionId) {
    headers['x-arango-trx-id'] = options.transactionId;
  }
  if (rev !== null && !ignoreRevs) {
    headers['if-match'] = JSON.stringify(rev);
  }

  let requestResult = this._database._connection.DELETE(url, body, headers);
  if (requestResult !== null && requestResult.error === true) {
    if (requestResult.errorNum === internal.errors.ERROR_HTTP_PRECONDITION_FAILED.code) {
      requestResult.errorNum = internal.errors.ERROR_ARANGO_CONFLICT.code;
    }
  }

  arangosh.checkRequestResult(requestResult);

  return options.silent ? true : requestResult;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief replaces a document in the collection
// / @param id the id of the document
// / @param overwrite (optional) a boolean value or a json object
// / @param waitForSync (optional) a boolean value .
// / @example replace("example/996280832675", { a : 1, c : 2} )
// / @example replace("example/996280832675", { a : 1, c : 2}, true)
// / @example replace("example/996280832675", { a : 1, c : 2}, false)
// / @example replace("example/996280832675", { a : 1, c : 2}, {waitForSync: false, overwrite: true})
// //////////////////////////////////////////////////////////////////////////////

function fillInSpecial (id, data) {
  let pos;
  if (data === null || typeof data !== 'object' || Array.isArray(data)) {
    return;
  }
  if (typeof id === 'object' && id !== null && !Array.isArray(id)) {
    if (id.hasOwnProperty('_rev')) {
      data._rev = id._rev;
    }
    if (id.hasOwnProperty('_id')) {
      data._id = id._id;
      pos = id._id.indexOf('/');
      if (pos >= 0) {
        data._key = id._id.substr(pos + 1);
      } else {
        delete data._key;
      }
    } else if (id.hasOwnProperty('_key')) {
      data._key = id._key;
      delete data._id;
    } else {
      delete data._id;
      delete data._key;
    // Server shall fail here
    }
  } else if (typeof id === 'string') {
    delete data._rev;
    pos = id.indexOf('/');
    if (pos >= 0) {
      data._id = id;
      data._key = id.substr(pos + 1);
    } else {
      data._key = id;
      delete data._id;
    }
  } else {
    delete data._id;
    delete data._key;
  }
}

ArangoCollection.prototype.replace = function (id, data, overwrite, waitForSync) {
  let rev = null;

  if (id === undefined || id === null) {
    throw new ArangoError({
      error: true,
      errorCode: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.code,
      errorNum: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.code,
      errorMessage: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.message
    });
  }

  var ignoreRevs = false;
  var options;
  if (typeof overwrite === 'object') {
    if (typeof waitForSync !== 'undefined') {
      throw 'too many arguments';
    }
    // we assume the caller uses new signature (id, data, options)
    options = overwrite;
    if (options.hasOwnProperty('overwrite') && options.overwrite) {
      ignoreRevs = true;
    }
    if (options.hasOwnProperty('waitForSync')) {
      waitForSync = options.waitForSync;
    }

    if (!options.hasOwnProperty('skipDocumentValidation')) {
      options.skipDocumentValidation = false;
    }
  } else {
    if (overwrite) {
      ignoreRevs = true;
    }
    options = {};
  }

  let url;
  if (Array.isArray(id)) {
    if (!Array.isArray(data) || id.length !== data.length) {
      throw new ArangoError({
        error: true,
        errorCode: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.code,
        errorNum: internal.errors.ERROR_ARANGO_DOCUMENT_TYPE_INVALID.code,
        errorMessage: internal.errors.ERROR_ARANGO_DOCUMENT_TYPE_INVALID.message
      });
    }
    for (let i = 0; i < id.length; i++) {
      fillInSpecial(id[i], data[i]);
    }
    url = this._documentcollectionurl();
  } else if (typeof id === 'object') {
    if (id.hasOwnProperty('_rev')) {
      rev = id._rev;
    }
    if (id.hasOwnProperty('_id')) {
      id = id._id;
    } else if (id.hasOwnProperty('_key')) {
      id = id._key;
    }
    url = this._documenturl(id);
  } else {
    url = this._documenturl(id);
  }

  url = appendBoolParameter(url, 'ignoreRevs', ignoreRevs);
  // the following parameters are optional, so we only append them if necessary
  if (waitForSync) {
    url = appendSyncParameter(url, waitForSync);
  }
  if (options.skipDocumentValidation) {
    url = appendBoolParameter(url, 'skipDocumentValidation', options.skipDocumentValidation);
  }
  if (options.returnOld) {
    url = appendBoolParameter(url, 'returnOld', options.returnOld);
  }
  if (options.returnNew) {
    url = appendBoolParameter(url, 'returnNew', options.returnNew);
  }
  if (options.silent) {
    url = appendBoolParameter(url, 'silent', options.silent);
  }

  let headers ={};
  if (options.transactionId) {
    headers['x-arango-trx-id'] = options.transactionId;
  }
  if (rev !== null && !ignoreRevs) {
    headers['if-match'] = JSON.stringify(rev);
  }

  let requestResult = this._database._connection.PUT(url, data, headers);
  if (requestResult !== null && requestResult.error === true) {
    if (requestResult.errorNum === internal.errors.ERROR_HTTP_PRECONDITION_FAILED.code) {
      requestResult.errorNum = internal.errors.ERROR_ARANGO_CONFLICT.code;
    }
  }

  arangosh.checkRequestResult(requestResult);

  return options.silent ? true : requestResult;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief update a document in the collection
// / @param id the id of the document
// / @param overwrite (optional) a boolean value or a json object
// / @param keepNull (optional) determines if null values should saved or not
// / @param mergeObjects (optional) whether or not object values should be merged
// / @param waitForSync (optional) a boolean value .
// / @example update("example/996280832675", { a : 1, c : 2} )
// / @example update("example/996280832675", { a : 1, c : 2, x: null}, true, true, true)
// / @example update("example/996280832675", { a : 1, c : 2, x: null},
//                 {keepNull: true, waitForSync: false, overwrite: true})
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.update = function (id, data, overwrite, keepNull, waitForSync) {
  let rev = null;

  if (id === undefined || id === null) {
    throw new ArangoError({
      errorNum: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.code,
      errorMessage: internal.errors.ERROR_ARANGO_DOCUMENT_HANDLE_BAD.message
    });
  }

  var params = '';
  var ignoreRevs = false;
  var options;

  if (typeof overwrite === 'object') {
    if (typeof keepNull !== 'undefined') {
      throw 'too many arguments';
    }
    // we assume the caller uses new signature (id, data, options)
    options = overwrite;

    if (!options.hasOwnProperty('skipDocumentValidation')) {
      options.skipDocumentValidation = false;
    }
    params = '?skipDocumentValidation=' + options.skipDocumentValidation;

    if (! options.hasOwnProperty('keepNull')) {
      options.keepNull = true;
    }
    params += '&keepNull=' + options.keepNull;

    if (!options.hasOwnProperty('mergeObjects')) {
      options.mergeObjects = true;
    }
    params += '&mergeObjects=' + options.mergeObjects;

    if (options.hasOwnProperty('overwrite') && options.overwrite) {
      ignoreRevs = true;
    }
  } else {
    // set default value for keepNull
    var keepNullValue = ((typeof keepNull === 'undefined') ? true : keepNull);
    params = '?keepNull=' + (keepNullValue ? 'true' : 'false');

    if (overwrite) {
      ignoreRevs = true;
    }

    options = {};
  }

  var url;
  if (Array.isArray(id)) {
    if (!Array.isArray(data) || id.length !== data.length) {
      throw new ArangoError({
        errorNum: internal.errors.ERROR_ARANGO_DOCUMENT_TYPE_INVALID.code,
        errorMessage: internal.errors.ERROR_ARANGO_DOCUMENT_TYPE_INVALID.message
      });
    }
    for (var i = 0; i < id.length; i++) {
      fillInSpecial(id[i], data[i]);
    }
    url = this._documentcollectionurl() + params;
  } else if (typeof id === 'object') {
    if (id.hasOwnProperty('_rev')) {
      rev = id._rev;
    }
    if (id.hasOwnProperty('_id')) {
      id = id._id;
    } else if (id.hasOwnProperty('_key')) {
      id = id._key;
    }
    url = this._documenturl(id) + params;
  } else {
    url = this._documenturl(id) + params;
  }

  url = appendBoolParameter(url, 'ignoreRevs', ignoreRevs);
  // the following parameters are optional, so we only append them if necessary
  if (waitForSync) {
    url = appendSyncParameter(url, waitForSync);
  }

  if (options.returnOld) {
    url = appendBoolParameter(url, 'returnOld', options.returnOld);
  }
  if (options.returnNew) {
    url = appendBoolParameter(url, 'returnNew', options.returnNew);
  }
  if (options.silent) {
    url = appendBoolParameter(url, 'silent', options.silent);
  }

  let headers = {};
  if (options.transactionId) {
    headers['x-arango-trx-id'] = options.transactionId;
  }
  if (rev !== null && !ignoreRevs) {
    headers['if-match'] = JSON.stringify(rev);
  }

  let requestResult = this._database._connection.PATCH(url, data, headers);
  if (requestResult !== null && requestResult.error === true) {
    if (requestResult.errorNum === internal.errors.ERROR_HTTP_PRECONDITION_FAILED.code) {
      requestResult.errorNum = internal.errors.ERROR_ARANGO_CONFLICT.code;
    }
  }

  arangosh.checkRequestResult(requestResult);

  return options.silent ? true : requestResult;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief returns the edges starting or ending in a vertex
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.edges = function (vertex) {
  return this._edgesQuery(vertex);
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief returns the edges ending in a vertex
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.inEdges = function (vertex) {
  return this._edgesQuery(vertex, 'in');
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief returns the edges starting in a vertex
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.outEdges = function (vertex) {
  return this._edgesQuery(vertex, 'out');
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief removes documents matching an example
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.removeByExample = function (example,
  waitForSync,
  limit) {
  var data = {
    collection: this._name,
    example: example,
    waitForSync: waitForSync,
    limit: limit
  };

  if (typeof waitForSync === 'object') {
    if (typeof limit !== 'undefined') {
      throw 'too many parameters';
    }
    data = {
      collection: this._name,
      example: example,
      options: waitForSync
    };
  }

  var requestResult = this._database._connection.PUT(
    this._prefixurl('/_api/simple/remove-by-example'), data);

  arangosh.checkRequestResult(requestResult);

  return requestResult.deleted;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief replaces documents matching an example
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.replaceByExample = function (example,
  newValue,
  waitForSync,
  limit) {
  var data = {
    collection: this._name,
    example: example,
    newValue: newValue,
    waitForSync: waitForSync,
    limit: limit
  };

  if (typeof waitForSync === 'object') {
    if (typeof limit !== 'undefined') {
      throw 'too many parameters';
    }
    data = {
      collection: this._name,
      example: example,
      newValue: newValue,
      options: waitForSync
    };
  }
  var requestResult = this._database._connection.PUT(
    this._prefixurl('/_api/simple/replace-by-example'), data);

  arangosh.checkRequestResult(requestResult);

  return requestResult.replaced;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief updates documents matching an example
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.updateByExample = function (example,
  newValue,
  keepNull,
  waitForSync,
  limit) {
  var data = {
    collection: this._name,
    example: example,
    newValue: newValue,
    keepNull: keepNull,
    waitForSync: waitForSync,
    limit: limit
  };
  if (typeof keepNull === 'object') {
    if (typeof waitForSync !== 'undefined') {
      throw 'too many parameters';
    }
    var options = keepNull;
    data = {
      collection: this._name,
      example: example,
      newValue: newValue,
      options: options
    };
  }
  var requestResult = this._database._connection.PUT(
    this._prefixurl('/_api/simple/update-by-example'), data);

  arangosh.checkRequestResult(requestResult);

  return requestResult.updated;
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief looks up documents by keys
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.documents = function (keys) {
  var data = {
    collection: this._name,
    keys: keys || []
  };

  var requestResult = this._database._connection.PUT(
    this._prefixurl('/_api/simple/lookup-by-keys'), data);

  arangosh.checkRequestResult(requestResult);

  return {
    documents: requestResult.documents
  };
};

// .lookupByKeys is now an alias for .documents
ArangoCollection.prototype.lookupByKeys = ArangoCollection.prototype.documents;

// //////////////////////////////////////////////////////////////////////////////
// / @brief removes documents by keys
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.removeByKeys = function (keys) {
  var data = {
    collection: this._name,
    keys: keys || []
  };

  var requestResult = this._database._connection.PUT(
    this._prefixurl('/_api/simple/remove-by-keys'), data);

  arangosh.checkRequestResult(requestResult);

  return {
    removed: requestResult.removed,
    ignored: requestResult.ignored
  };
};

// //////////////////////////////////////////////////////////////////////////////
// / @brief load indexes of a collection into memory
// //////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype.loadIndexesIntoMemory = function () {
  this._status = null;
  var requestResult = this._database._connection.PUT(this._baseurl('loadIndexesIntoMemory'), null);
  this._status = null;

  arangosh.checkRequestResult(requestResult);
  return { result: true };
};

//////////////////////////////////////////////////////////////////////////////
/// @brief MerkleTreeVerification
//////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype._revisionTreeVerification = function() {
  var batch = this._database._connection.POST(this._prefixurl('/_api/replication/batch'), {ttl : 3600});
  if (!batch.hasOwnProperty("id")) {
    throw "Could not create batch!";
  }
  var requestResult = this._database._connection.GET(this._prefixurl(
    `/_api/replication/revisions/tree?collection=${encodeURIComponent(this._name)}&verification=true&batchId=${batch.id}`));
  this._database._connection.DELETE(this._prefixurl(
    `/_api/replication/batch/${batch.id}`));
  return requestResult;
};

//////////////////////////////////////////////////////////////////////////////
/// @brief MerkleTreeRebuilding
//////////////////////////////////////////////////////////////////////////////

ArangoCollection.prototype._revisionTreeRebuild = function() {
  // For some reason we need a batch ID here, which is not used!
  let requestResult = this._database._connection.POST(this._prefixurl(
    `/_api/replication/revisions/tree?collection=${encodeURIComponent(this._name)}&batchId=42`), {});
  arangosh.checkRequestResult(requestResult);
  return { result: true };
};
