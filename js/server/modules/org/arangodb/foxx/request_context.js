/*jslint indent: 2, nomen: true, maxlen: 120 */
/*global module, require, exports */

////////////////////////////////////////////////////////////////////////////////
/// @brief Foxx Request Context
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2013 triagens GmbH, Cologne, Germany
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
/// @author Lucas Dohmen
/// @author Copyright 2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var RequestContext,
  RequestContextBuffer,
  SwaggerDocs,
  _ = require("underscore"),
  extend = _.extend,
  internal = require("org/arangodb/foxx/internals"),
  is = require("org/arangodb/is"),
  UnauthorizedError = require("org/arangodb/foxx/authentication").UnauthorizedError,
  createErrorBubbleWrap,
  createBodyParamBubbleWrap,
  addCheck;

var elementExtractFactory = function (paramName, rootElement) {
  'use strict';
  var extractElement;

  if (rootElement) {
    extractElement = function (req) {
      return req.body()[paramName];
    };
  } else {
    extractElement = function (req) {
      return req.body();
    };
  }

  return extractElement;
};

var bubbleWrapFactory = function (handler, paramName, Proto, extractElement, multiple) {
  'use strict';
  if (multiple) {
    Proto = Proto[0];
  }

  return function (req, res) {
    if (multiple) {
      req.parameters[paramName] = _.map(extractElement(req), function (raw) {
        return new Proto(raw);
      });
    } else {
      req.parameters[paramName] = new Proto(extractElement(req));
    }
    handler(req, res);
  };
};

createBodyParamBubbleWrap = function (handler, paramName, Proto, rootElement) {
  'use strict';
  var extractElement = elementExtractFactory(paramName, rootElement);
  return bubbleWrapFactory(handler, paramName, Proto, extractElement, is.array(Proto));
};

createErrorBubbleWrap = function (handler, errorClass, code, reason, errorHandler) {
  'use strict';
  if (is.notExisty(errorHandler)) {
    errorHandler = function () {
      return { error: reason };
    };
  }

  return function (req, res) {
    try {
      handler(req, res);
    } catch (e) {
      if ((typeof errorClass === 'string' && e.name === errorClass) || e instanceof errorClass) {
        res.status(code);
        res.json(errorHandler(e));
      } else {
        throw e;
      }
    }
  };
};

addCheck = function (handler, check) {
  'use strict';
  return function (req, res) {
    check(req);
    handler(req, res);
  };
};

// Wraps the docs object of a route to add swagger compatible documentation
SwaggerDocs = function (docs, models) {
  'use strict';
  this.docs = docs;
  this.models = models;
};

extend(SwaggerDocs.prototype, {
  addNickname: function (httpMethod, match) {
    'use strict';
    this.docs.nickname = internal.constructNickname(httpMethod, match);
  },

  addPathParam: function (paramName, description, dataType) {
    'use strict';
    this.docs.parameters.push(internal.constructPathParamDoc(paramName, description, dataType));
  },

  addQueryParam: function (paramName, description, dataType, required, allowMultiple) {
    'use strict';
    this.docs.parameters.push(internal.constructQueryParamDoc(
      paramName,
      description,
      dataType,
      required,
      allowMultiple
    ));
  },

  addBodyParam: function (paramName, description, jsonSchema) {
    'use strict';
    this.models[jsonSchema.id] = jsonSchema;

    var param = _.find(this.docs.parameters, function (parameter) {
      return parameter.name === 'undocumented body';
    });

    if (_.isUndefined(param)) {
      this.docs.parameters.push({
        name: paramName,
        paramType: "body",
        description: description,
        dataType: jsonSchema.id
      });
    } else {
      param.name = paramName;
      param.description = description;
      param.dataType = jsonSchema.id;
    }
  },

  addSummary: function (summary) {
    'use strict';
    this.docs.summary = summary;
  },

  addNotes: function (notes) {
    'use strict';
    this.docs.notes = notes;
  }
});

////////////////////////////////////////////////////////////////////////////////
/// @fn JSF_foxx_RequestContext_initializer
/// @brief Context of a Request Definition
///
/// Used for documenting and constraining the routes.
////////////////////////////////////////////////////////////////////////////////

RequestContext = function (executionBuffer, models, route, rootElement) {
  'use strict';
  this.route = route;
  this.typeToRegex = {
    "int": "/[0-9]+/",
    "integer": "/[0-9]+/",
    "string": "/[^/]+/"
  };
  this.rootElement = rootElement;

  this.docs = new SwaggerDocs(this.route.docs, models);
  this.docs.addNickname(route.docs.httpMethod, route.url.match);

  executionBuffer.applyEachFunction(this);
};

extend(RequestContext.prototype, {

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_foxx_RequestContext_pathParam
///
/// If you defined a route "/foxx/:id", you can constrain which format a path
/// parameter (*/foxx/12*) can have by giving it a type.  We currently support
/// the following types:
///
/// * int
/// * string
///
/// You can also provide a description of this parameter.
///
/// @EXAMPLES
///
/// ```js
/// app.get("/foxx/:id", function {
///   // Do something
/// }).pathParam("id", {
///   description: "Id of the Foxx",
///   type: "int"
/// });
/// ```
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

  pathParam: function (paramName, attributes) {
    'use strict';
    var url = this.route.url,
      constraint = url.constraint || {};

    constraint[paramName] = this.typeToRegex[attributes.type];
    if (!constraint[paramName]) {
      throw new Error("Illegal attribute type: " + attributes.type);
    }
    this.route.url = internal.constructUrlObject(url.match, constraint, url.methods[0]);
    this.docs.addPathParam(paramName, attributes.description, attributes.type);
    return this;
  },

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_foxx_RequestContext_queryParam
///
/// `FoxxController::queryParam(id, options)`
///
/// Describe a query parameter:
///
/// If you defined a route "/foxx", you can constrain which format a query
/// parameter (*/foxx?a=12*) can have by giving it a type.  We currently support
/// the following types:
///
/// * int
/// * string
///
/// You can also provide a description of this parameter, if it is required and
/// if you can provide the parameter multiple times.
///
/// @EXAMPLES
///
/// ```js
/// app.get("/foxx", function {
///   // Do something
/// }).queryParam("id", {
///   description: "Id of the Foxx",
///   type: "int",
///   required: true,
///   allowMultiple: false
/// });
/// ```
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

  queryParam: function (paramName, attributes) {
    'use strict';
    this.docs.addQueryParam(
      paramName,
      attributes.description,
      attributes.type,
      attributes.required,
      attributes.allowMultiple
    );
    return this;
  },

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_foxx_RequestContext_bodyParam
///
/// `FoxxController::bodyParam(paramName, description, Model)`
///
/// Expect the body of the request to be a JSON with the attributes you annotated
/// in your model. It will appear alongside the provided description in your
/// Documentation.
/// This will initialize a *Model* with the data and provide it to you via the
/// params as *paramName*.
/// For information about how to annotate your models, see the Model section.
/// If you provide the Model in an array, the response will take multiple models
/// instead of one.
///
/// If you wrap the provided model in an array, the body param is always an array
/// and accordingly the return value of the *params* for the body call will also
/// return an array of models.
///
/// The behavior of *bodyParam* changes depending on the *rootElement* option
/// set in the [manifest](../Foxx/Manifest.md). If it is set to true, it is 
/// expected that the body is an
/// object with a key of the same name as the *paramName* argument.
/// The value of this object is either a single object or in the case of a multi
/// element an array of objects.
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

  bodyParam: function (paramName, description, Proto) {
    'use strict';
    var handler = this.route.action.callback;

    if (is.array(Proto)) {
      this.docs.addBodyParam(paramName, description, Proto[0].toJSONSchema(paramName));
    } else {
      this.docs.addBodyParam(paramName, description, Proto.toJSONSchema(paramName));
    }
    this.route.action.callback = createBodyParamBubbleWrap(handler, paramName, Proto, this.rootElement);

    return this;
  },

////////////////////////////////////////////////////////////////////////////////
/// `FoxxController::summary(description)`
///
/// Set the summary for this route in the documentation. Can't be longer than 60.
/// characters
////////////////////////////////////////////////////////////////////////////////

  summary: function (summary) {
    'use strict';
    if (summary.length > 60) {
      throw new Error("Summary can't be longer than 60 characters");
    }
    this.docs.addSummary(summary);
    return this;
  },

////////////////////////////////////////////////////////////////////////////////
/// `FoxxController::notes(description)`
///
/// Set the notes for this route in the documentation
////////////////////////////////////////////////////////////////////////////////

  notes: function (notes) {
    'use strict';
    this.docs.addNotes(notes);
    return this;
  },

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_foxx_RequestContext_errorResponse
///
/// `FoxxController::errorResponse(errorClass, code, description)`
///
/// Define a reaction to a thrown error for this route: If your handler throws an error
/// of the defined errorClass, it will be caught and the response will have the given
/// status code and a JSON with error set to your description as the body.
///
/// If you want more control over the returned JSON, you can give an optional fourth
/// parameter in form of a function. It gets the error as an argument, the return
/// value will transformed into JSON and then be used as the body.
/// The status code will be used as described above. The description will be used for
/// the documentation.
///
/// It also adds documentation for this error response to the generated documentation.
///
/// @EXAMPLES
///
/// ```js
/// /* define our own error type, FoxxyError */
/// var FoxxyError = function (message) {
///   this.message = "the following FoxxyError occurred: ' + message;
/// };
/// FoxxyError.prototype = new Error();
///
/// app.get("/foxx", function {
///   /* throws a FoxxyError */
///   throw new FoxxyError();
/// }).errorResponse(FoxxyError, 303, "This went completely wrong. Sorry!");
///
/// app.get("/foxx", function {
///   throw new FoxxyError("oops!");
/// }).errorResponse(FoxxyError, 303, "This went completely wrong. Sorry!", function (e) {
///   return {
///     code: 123,
///     desc: e.message
///   };
/// });
/// ```
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////
  errorResponse: function (errorClass, code, reason, errorHandler) {
    'use strict';
    var handler = this.route.action.callback;
    this.route.action.callback = createErrorBubbleWrap(handler, errorClass, code, reason, errorHandler);
    this.route.docs.errorResponses.push(internal.constructErrorResponseDoc(code, reason));
    return this;
  },

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_foxx_RequestContext_onlyIf
///
/// `FoxxController::onlyIf(check)`
///
/// Provide it with a function that throws an exception if the normal processing should
/// not be executed. Provide an `errorResponse` to define the behavior in this case.
/// This can be used for authentication or authorization for example.
///
/// @EXAMPLES
///
/// ```js
/// app.get("/foxx", function {
///   // Do something
/// }).onlyIf(aFunction).errorResponse(ErrorClass, 303, "This went completely wrong. Sorry!");
/// ```
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////
  onlyIf: function (check, ErrorClass) {
    'use strict';
    var handler = this.route.action.callback;
    this.route.action.callback = addCheck(handler, check);
    return this;
  },

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_foxx_RequestContext_onlyIfAuthenticated
///
/// `FoxxController::onlyIf(code, reason)`
///
/// Please activate authentification for this app if you want to use this function.
/// If the user is logged in, it will do nothing. Otherwise it will respond with
/// the status code and the reason you provided (the route handler won't be called).
/// This will also add the according documentation for this route.
///
/// @EXAMPLES
///
/// ```js
/// app.get("/foxx", function {
///   // Do something
/// }).onlyIfAuthenticated(401, "You need to be authenticated");
/// ```
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////
  onlyIfAuthenticated: function (code, reason) {
    'use strict';
    var check;

    check = function (req) {
      if (!(req.user && req.currentSession)) {
        throw new UnauthorizedError();
      }
    };

    if (is.notExisty(code)) {
      code = 401;
    }
    if (is.notExisty(reason)) {
      reason = "Not authorized";
    }

    this.onlyIf(check);
    this.errorResponse(UnauthorizedError, code, reason);

    return this;
  }
});

RequestContextBuffer = function () {
  'use strict';
  this.applyChain = [];
};

extend(RequestContextBuffer.prototype, {
  applyEachFunction: function (target) {
    'use strict';
    _.each(this.applyChain, function (x) {
      target[x.functionName].apply(target, x.argumentList);
    });
  }
});

_.each([
////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_foxx_RequestContextBuffer_errorResponse
///
/// `RequestContextBuffer::errorResponse(errorClass, code, description)`
///
/// Defines an *errorResponse* for all routes of this controller. For details on
/// *errorResponse* see the according method on routes.
///
/// @EXAMPLES
///
/// ```js
/// app.allroutes.errorResponse(FoxxyError, 303, "This went completely wrong. Sorry!");
///
/// app.get("/foxx", function {
///   // Do something
/// });
/// ```
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////
  "errorResponse",

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_foxx_RequestContextBuffer_onlyIf
///
/// `RequestContextBuffer::onlyIf(code, reason)`
///
/// Defines an *onlyIf* for all routes of this controller. For details on
/// *onlyIf* see the according method on routes.
///
/// @EXAMPLES
///
/// ```js
/// app.allroutes.onlyIf(myPersonalCheck);
///
/// app.get("/foxx", function {
///   // Do something
/// });
/// ```
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////
  "onlyIf",

////////////////////////////////////////////////////////////////////////////////
/// @startDocuBlock JSF_foxx_RequestContextBuffer_onlyIfAuthenticated
///
/// `RequestContextBuffer::errorResponse(errorClass, code, description)`
///
/// Defines an *onlyIfAuthenticated* for all routes of this controller. For details on
/// *onlyIfAuthenticated* see the according method on routes.
///
/// @EXAMPLES
///
/// ```js
/// app.allroutes.onlyIfAuthenticated(401, "You need to be authenticated");
///
/// app.get("/foxx", function {
///   // Do something
/// });
/// ```
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////
  "onlyIfAuthenticated"
], function (functionName) {
  'use strict';
  extend(RequestContextBuffer.prototype[functionName] = function () {
    this.applyChain.push({
      functionName: functionName,
      argumentList: arguments
    });
  });
});

exports.RequestContext = RequestContext;
exports.RequestContextBuffer = RequestContextBuffer;

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

/// Local Variables:
/// mode: outline-minor
/// outline-regexp: "/// @brief\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}\\|/\\*jslint"
/// End:
