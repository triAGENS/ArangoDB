////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
#include <Logger/LogMacros.h>
#include <tao/json/jaxn/to_string.hpp>
static bool debug = false;
#endif

#include "Validators.h"
#include <Basics/Exceptions.h>
#include <Basics/StaticStrings.h>

#include <validation/validation.hpp>

namespace arangodb {

std::string const&  to_string(ValidationLevel level) {
  switch (level) {
    case ValidationLevel::None:
      return StaticStrings::ValidatorLevelNone;
    case ValidationLevel::New:
      return StaticStrings::ValidatorLevelNew;
    case ValidationLevel::Moderate:
      return StaticStrings::ValidatorLevelModerate;
    case ValidationLevel::Strict:
      return StaticStrings::ValidatorLevelStrict;
  }
  TRI_ASSERT(false);
  return StaticStrings::ValidatorLevelStrict;  // <- avoids: reaching end of non-void function ....
}

//////////////////////////////////////////////////////////////////////////////

ValidatorBase::ValidatorBase(VPackSlice params)
    : _level(ValidationLevel::Strict) {
  // parse message
  auto msgSlice = params.get(StaticStrings::ValidatorParameterMessage);
  if (msgSlice.isString()) {
    this->_message = msgSlice.copyString();
  }

  // parse level
  auto levelSlice = params.get(StaticStrings::ValidatorParameterLevel);
  if (levelSlice.isString()) {
    if (levelSlice.compareString(StaticStrings::ValidatorLevelNone) == 0) {
      this->_level = ValidationLevel::None;
    } else if (levelSlice.compareString(StaticStrings::ValidatorLevelNew) == 0) {
      this->_level = ValidationLevel::New;
    } else if (levelSlice.compareString(StaticStrings::ValidatorLevelModerate) == 0) {
      this->_level = ValidationLevel::Moderate;
    } else if (levelSlice.compareString(StaticStrings::ValidatorLevelStrict) == 0) {
      this->_level = ValidationLevel::Strict;
    } else {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_VALIDATION_BAD_PARAMETER,
                                     "Valid validation levels are: " + StaticStrings::ValidatorLevelNone +
                                         ", " + StaticStrings::ValidatorLevelNew +
                                         ", " + StaticStrings::ValidatorLevelModerate +
                                         ", " + StaticStrings::ValidatorLevelStrict +
                                         "");
    }
  }
}

bool ValidatorBase::validate(VPackSlice newDoc, VPackSlice oldDoc, bool isInsert, VPackOptions const* options) const {
  // This function performs the validation depending on operation (Insert /
  // Update / Replace) and requested validation level (None / Insert / New /
  // Strict / Moderate).

  if (this->_level == ValidationLevel::None) {
    return true;
  }

  if (isInsert) {
    return this->validateDerived(newDoc, options);
  }

  /* update replace case */
  if (this->_level == ValidationLevel::New) {
    // Level NEW is for insert only.
    return true;
  }

  if (this->_level == ValidationLevel::Strict) {
    // Changed document must be good!
    return validateDerived(newDoc, options);
  }

  TRI_ASSERT(this->_level == ValidationLevel::Moderate);
  // Changed document must be good IIF the unmodified
  // document passed validation.
  return (this->validateDerived(newDoc, options) || !this->validateDerived(oldDoc, options));
}

void ValidatorBase::toVelocyPack(VPackBuilder& b) const {
  VPackObjectBuilder guard(&b);
  b.add(StaticStrings::ValidatorParameterMessage, VPackValue(_message));
  b.add(StaticStrings::ValidatorParameterLevel, VPackValue(to_string(this->_level)));
  b.add(StaticStrings::ValidatorParameterType, VPackValue(this->type()));
  this->toVelocyPackDerived(b);
}

/////////////////////////////////////////////////////////////////////////////

ValidatorBool::ValidatorBool(VPackSlice params) : ValidatorBase(params) {
  _result = params.get(StaticStrings::ValidatorParameterRule).getBool();
}
bool ValidatorBool::validateDerived(VPackSlice slice, VPackOptions const* options) const { return _result; }
void ValidatorBool::toVelocyPackDerived(VPackBuilder& b) const {
  b.add(StaticStrings::ValidatorParameterRule, VPackValue(_result));
}
std::string const ValidatorBool::type() const {
  return StaticStrings::ValidatorTypeBool;
}

/////////////////////////////////////////////////////////////////////////////

ValidatorJsonSchema::ValidatorJsonSchema(VPackSlice params) : ValidatorBase(params) {
  auto rule = params.get(StaticStrings::ValidatorParameterRule);
  VPackBuilder ruleBuilder;
  {
    VPackObjectBuilder guard(&ruleBuilder);
    ruleBuilder.add(VPackValue("properties"));
    ruleBuilder.add(rule);
  }

  auto taoRuleValue = validation::slice_to_value(ruleBuilder.slice());
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  LOG_DEVEL_IF(debug) << tao::json::jaxn::to_string(taoRuleValue,2);
#endif
  _schema = std::make_shared<tao::json::schema>(taoRuleValue);
  _builder.add(rule);
}
bool ValidatorJsonSchema::validateDerived(VPackSlice slice, VPackOptions const* options) const {
  return validation::validate(slice, options, *_schema);
}
void ValidatorJsonSchema::toVelocyPackDerived(VPackBuilder& b) const {
  b.add(StaticStrings::ValidatorParameterRule, _builder.slice());
}
std::string const ValidatorJsonSchema::type() const {
  return StaticStrings::ValidatorTypeJsonSchema;
}

}  // namespace arangodb
