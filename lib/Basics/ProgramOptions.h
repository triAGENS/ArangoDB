////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef LIB_BASICS_PROGRAM_OPTIONS_H
#define LIB_BASICS_PROGRAM_OPTIONS_H 1

#include "Basics/Common.h"

#include "Basics/vector.h"
#include "Basics/ProgramOptionsDescription.h"

struct TRI_json_t;
struct TRI_program_options_s;
struct TRI_PO_section_s;

namespace arangodb {
namespace basics {

////////////////////////////////////////////////////////////////////////////////
/// @brief ProgramOptions
////////////////////////////////////////////////////////////////////////////////

class ProgramOptions {
 private:
  ProgramOptions(ProgramOptions const&) = delete;
  ProgramOptions& operator=(ProgramOptions const&) = delete;

 public:
  ProgramOptions();

  ~ProgramOptions();

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief parse command line
  //////////////////////////////////////////////////////////////////////////////

  bool parse(ProgramOptionsDescription const&, int argc, char** argv);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief parse configuration file
  //////////////////////////////////////////////////////////////////////////////

  bool parse(ProgramOptionsDescription const&, std::string const& filename);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief checks if option was given
  //////////////////////////////////////////////////////////////////////////////

  bool has(std::string const& name) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief checks if help option was given
  //////////////////////////////////////////////////////////////////////////////

  std::set<std::string> needHelp(std::string const& name) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the last error
  //////////////////////////////////////////////////////////////////////////////

  std::string lastError();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get global program options as JSON
  //////////////////////////////////////////////////////////////////////////////

  static struct TRI_json_t const* getJson();

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief generates description for the main section
  //////////////////////////////////////////////////////////////////////////////

  struct TRI_PO_section_s* setupDescription(
      ProgramOptionsDescription const& description);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief generates description for the sub-sections
  //////////////////////////////////////////////////////////////////////////////

  void setupSubDescription(ProgramOptionsDescription const& description,
                           struct TRI_PO_section_s* desc);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief result data
  //////////////////////////////////////////////////////////////////////////////

  bool extractValues(ProgramOptionsDescription const&,
                     struct TRI_program_options_s*, std::set<std::string> seen);

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief bool values
  //////////////////////////////////////////////////////////////////////////////

  std::map<std::string, bool*> _valuesBool;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief string values
  //////////////////////////////////////////////////////////////////////////////

  std::map<std::string, char**> _valuesString;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief vector values
  //////////////////////////////////////////////////////////////////////////////

  std::map<std::string, TRI_vector_string_t*> _valuesVector;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief options
  //////////////////////////////////////////////////////////////////////////////

  std::vector<std::string> _options;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief error messages
  //////////////////////////////////////////////////////////////////////////////

  std::string _errorMessage;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief help options
  //////////////////////////////////////////////////////////////////////////////

  std::set<std::string> _helpOptions;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief flags which are set
  //////////////////////////////////////////////////////////////////////////////

  std::set<std::string> _flags;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief flags which are already set
  //////////////////////////////////////////////////////////////////////////////

  std::set<std::string> _seen;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief program name if known
  //////////////////////////////////////////////////////////////////////////////

  std::string _programName;
};
}
}

#endif
