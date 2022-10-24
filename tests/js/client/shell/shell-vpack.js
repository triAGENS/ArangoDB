/*jshint globalstrict:false, strict:false */
/*global arango, VPACK_TO_V8, V8_TO_VPACK, assertEqual, assertTrue, assertFalse, assertNull */

////////////////////////////////////////////////////////////////////////////////
/// @brief test request module
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2015 triAGENS GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
/// @author Copyright 2016, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

'use strict';

var jsunity = require('jsunity');
var expect = require('chai').expect;

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function RequestSuite() {
  return {
    testVersionJsonJson: versionJsonJson,
    testVersionVpackJson: versionVpackJson,
    testVersionJsonVpack: versionJsonVpack,
    testVersionVpackVpack: versionVpackVpack,
    testEchoVpackVpack: echoVpackVpack,
    testAdminExecuteWithHeaderVpack: adminExecuteWithHeaderVpack,
    testAdminExecuteWithHeaderVpack2: adminExecuteWithHeaderVpack2,
    testAdminExecuteNoHeaderVpack: adminExecuteNoHeaderVpack,
  };
};


function versionJsonJson() {
  var path = '/_api/version';
  var headers = {
    'content-type': 'application/json',
    'accept': 'application/json'
  };

  var res = arango.POST_RAW(path, "", headers);

  expect(String(res.headers['content-type'])).to.have.string("application/json");

  var obj = JSON.parse(res.body);

  expect(obj).to.have.property('server');
  expect(obj).to.have.property('version');
  expect(obj).to.have.property('license');

  expect(obj.server).to.be.equal('arango');
  expect(obj.version).to.match(/[0-9]+\.[0-9]+\.([0-9]+|(milestone|alpha|beta|devel|rc)[0-9]*)/);

  expect(obj.license).to.match(/enterprise|community/g);
};

function versionVpackJson() {
  var path = '/_api/version';
  var headers = {
    'content-type': 'application/x-velocypack',
    'accept': 'application/json'
  };

  var res = arango.POST_RAW(path, "", headers);

  expect(res.body).to.be.a('string');
  expect(String(res.headers['content-type'])).to.have.string("application/json");

  var obj = JSON.parse(res.body);

  expect(obj).to.have.property('server');
  expect(obj).to.have.property('version');
  expect(obj).to.have.property('license');

  expect(obj.server).to.be.equal('arango');
  expect(obj.version).to.match(/[0-9]+\.[0-9]+\.([0-9]+|(milestone|alpha|beta|devel|rc)[0-9]*)/);
  expect(obj.license).to.match(/enterprise|community/g);
};

function versionJsonVpack() {
  var path = '/_api/version';
  var headers = {
    'content-type': 'application/json',
    'accept': 'application/x-velocypack'
  };

  var res = arango.POST_RAW(path, "", headers);

  expect(String(res.headers['content-type'])).to.have.string("application/x-velocypack");

  var obj = VPACK_TO_V8(res.body);

  expect(obj).to.have.property('server');
  expect(obj).to.have.property('version');
  expect(obj).to.have.property('license');

  expect(obj.server).to.be.equal('arango');
  expect(obj.version).to.match(/[0-9]+\.[0-9]+\.([0-9]+|(milestone|alpha|beta|devel|rc)[0-9]*)/);
  expect(obj.license).to.match(/enterprise|community/g);
};

function versionVpackVpack() {
  var path = '/_api/version';
  var headers = {
    'content-type': 'application/x-velocypack',
    'accept': 'application/x-velocypack'
  };

  var res = arango.POST_RAW(path, "", headers);

  expect(String(res.headers['content-type'])).to.have.string("application/x-velocypack");

  var obj = VPACK_TO_V8(res.body);

  expect(obj).to.have.property('server');
  expect(obj).to.have.property('version');
  expect(obj).to.have.property('license');

  expect(obj.server).to.be.equal('arango');
  expect(obj.version).to.match(/[0-9]+\.[0-9]+\.([0-9]+|(milestone|alpha|beta|devel|rc)[0-9]*)/);
  expect(obj.license).to.match(/enterprise|community/g);
};

///////////////////////////////////////////////////////////////////////////////////////

function echoVpackVpack() {
  var path = '/_admin/echo';
  var headers = {
    'content-type': 'application/x-velocypack',
    'accept': 'application/x-velocypack'
  };

  var obj = {"server": "arango", "version": "3.0.devel"};
  var body = V8_TO_VPACK(obj);

  var res = arango.POST_RAW(path, body, headers);

  expect(res.body).to.be.a('SlowBuffer');
  expect(String(res.headers['content-type'])).to.have.string("application/x-velocypack");
  var replyBody = VPACK_TO_V8(res.body);
  expect(replyBody.requestBody).to.be.a('string');
};


/*
The following tests are for requests to _admin/execute in which the body is in velocypack format.
It's only interpreted as velocypack if the header has the content type for velocypack in the request.
Otherwise, as it happens when there's no content-type in the header, it's interpreted as JSON and will return an error
 */

function adminExecuteWithHeaderVpack() {
  var path = '/_admin/execute';
  var headers = {
    'content-type': 'application/x-velocypack',
  };

  var obj = "require(\"console\").log(\"abc\");";
  var body = V8_TO_VPACK(obj);

  var res = arango.POST_RAW(path, body, headers);
  assertEqual(res.code, 200);
  assertFalse(res.error);
  assertNull(res.parsedBody);
};

function adminExecuteWithHeaderVpack2() {
  var path = '/_admin/execute';
  var headers = {
    'content-type': 'application/x-velocypack',
  };

  var obj = "return \"abc\"";
  var body = V8_TO_VPACK(obj);

  var res = arango.POST_RAW(path, body, headers);
  assertEqual(res.code, 200);
  assertFalse(res.error);
  assertEqual(res.parsedBody, "abc");
};

function adminExecuteNoHeaderVpack() {
  var path = '/_admin/execute';

  var obj = "return \"abc\"";
  var body = V8_TO_VPACK(obj);

  var res = arango.POST_RAW(path, body);
  assertEqual(res.code, 400);
  assertTrue(res.error);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

jsunity.run(RequestSuite);

return jsunity.done();
