////////////////////////////////////////////////////////////////////////////////
/// @brief a basis class for concrete implementations for a shell
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014-2015 ArangoDB GmbH, Cologne, Germany
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
/// @author Esteban Lombeyda
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2011-2014, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_UTILITIES_READLINE_SHELL_H
#define ARANGODB_UTILITIES_READLINE_SHELL_H 1

#include "Utilities/ShellImplementation.h"

#include "Utilities/Completer.h"

namespace arangodb {

// -----------------------------------------------------------------------------
// --SECTION--                                               class ReadlineShell
// -----------------------------------------------------------------------------

  class ReadlineShell : public ShellImplementation {

// -----------------------------------------------------------------------------
// --SECTION--                                             static public methods
// -----------------------------------------------------------------------------

    public:

////////////////////////////////////////////////////////////////////////////////
/// @brief return the currently active shell instance
////////////////////////////////////////////////////////////////////////////////

      static ReadlineShell* instance () {
        return _instance.load();
      }

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

    public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

      ReadlineShell (std::string const& history, Completer*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

      ~ReadlineShell ();

// -----------------------------------------------------------------------------
// --SECTION--                                       ShellImplementation methods
// -----------------------------------------------------------------------------

    public:

////////////////////////////////////////////////////////////////////////////////
/// @brief line editor open
////////////////////////////////////////////////////////////////////////////////

      bool open (bool autoComplete) override final;

////////////////////////////////////////////////////////////////////////////////
/// @brief line editor shutdown
////////////////////////////////////////////////////////////////////////////////

      bool close () override final;

////////////////////////////////////////////////////////////////////////////////
/// @brief get the history file path
///
/// The path is "$HOME" plus _historyFilename, if $HOME is set. Else
/// the local file _historyFilename.
////////////////////////////////////////////////////////////////////////////////

      std::string historyPath () override final;

////////////////////////////////////////////////////////////////////////////////
/// @brief add to history
////////////////////////////////////////////////////////////////////////////////

      void addHistory (const std::string&) override final;

////////////////////////////////////////////////////////////////////////////////
/// @brief save the history
////////////////////////////////////////////////////////////////////////////////

      bool writeHistory () override final;

////////////////////////////////////////////////////////////////////////////////
/// @brief read a line from the input
////////////////////////////////////////////////////////////////////////////////

      std::string getLine (const std::string& prompt, bool& eof) override final;

////////////////////////////////////////////////////////////////////////////////
/// @brief handle a signal
////////////////////////////////////////////////////////////////////////////////

      void signal () override final;

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

    public:

////////////////////////////////////////////////////////////////////////////////
/// @brief set the last input value
////////////////////////////////////////////////////////////////////////////////

      void setLastInput (const std::string& input) {
        _lastInput = input;
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the current input loop state
////////////////////////////////////////////////////////////////////////////////

      int getLoopState () const {
        return _loopState;
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the current input loop state
////////////////////////////////////////////////////////////////////////////////

      void setLoopState (int state) {
        _loopState = state;
      }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

    private:

////////////////////////////////////////////////////////////////////////////////
/// @brief current state of input loop (may be affected by out-of-band signals)
////////////////////////////////////////////////////////////////////////////////

      std::atomic<int> _loopState;

////////////////////////////////////////////////////////////////////////////////
/// @brief last value entered by user.
////////////////////////////////////////////////////////////////////////////////

      std::string _lastInput;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the input from the previous invocation was a CTRL-C
////////////////////////////////////////////////////////////////////////////////

      bool _lastInputWasEmpty;

////////////////////////////////////////////////////////////////////////////////
/// @brief system-wide instance of the ReadlineShell
////////////////////////////////////////////////////////////////////////////////

      static std::atomic<ReadlineShell*> _instance;
  };
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
