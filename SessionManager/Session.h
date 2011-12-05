////////////////////////////////////////////////////////////////////////////////
/// @brief session management
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2011 triagens GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
/// @author Copyright 2011, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_BORNHOLM_SESSION_MANAGER_SESSION_H
#define TRIAGENS_BORNHOLM_SESSION_MANAGER_SESSION_H 1

#include <Basics/Common.h>

#include <Admin/Right.h>
#include <Basics/Mutex.h>

namespace triagens {
  namespace admin {
    class User;

    ////////////////////////////////////////////////////////////////////////////////
    /// @brief session management
    ////////////////////////////////////////////////////////////////////////////////

    class Session {
      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief maximal number of open sessions
        ////////////////////////////////////////////////////////////////////////////////

        static size_t const MAXIMAL_OPEN_SESSION = 10;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief length of session identifier
        ////////////////////////////////////////////////////////////////////////////////

        static size_t const SID_LENGTH = 10;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief length of session characters
        ////////////////////////////////////////////////////////////////////////////////

        static string const SID_CHARACTERS;

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief access lock
        ////////////////////////////////////////////////////////////////////////////////

        static basics::Mutex lock;

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns a role by name
        ////////////////////////////////////////////////////////////////////////////////

        static Session* lookup (string const& sid);

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief creates a session
        ////////////////////////////////////////////////////////////////////////////////

        static Session* create ();

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief removes a session
        ///
        /// Remove will also delete the session object.
        ////////////////////////////////////////////////////////////////////////////////

        static bool remove (Session*);

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief sets a anonymous rights
        ////////////////////////////////////////////////////////////////////////////////

        static void setAnonymousRights (vector<right_t> const&);

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief gets anonymous rights
        ////////////////////////////////////////////////////////////////////////////////

        static set<right_t> const& anonymousRights ();

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns the session identifier
        ////////////////////////////////////////////////////////////////////////////////

        string const& getSid () const {
          return _sid;
        }


        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns the user of a session
        ////////////////////////////////////////////////////////////////////////////////

        User* getUser () const {
          return _user;
        }


      public:


        ////////////////////////////////////////////////////////////////////////////////
        /// @brief logs a user in
        ////////////////////////////////////////////////////////////////////////////////

        bool login (string const& username, string const& password, string& reason);

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief logs a user out
        ////////////////////////////////////////////////////////////////////////////////

        bool logout ();


        ////////////////////////////////////////////////////////////////////////////////
        /// @brief checks for a right
        ////////////////////////////////////////////////////////////////////////////////

        bool hasRight (right_t);

      protected:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief creates a new sessions
        ////////////////////////////////////////////////////////////////////////////////

        Session (string const& sid);

      private:
        static set<right_t> _rights;

      private:
        string const _sid;

        User* _user;

    };
  }
}

#endif
