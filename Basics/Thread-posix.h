////////////////////////////////////////////////////////////////////////////////
/// @brief Abstract Base Class for Threads
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
/// @author Achim Brandt
/// @author Copyright 2008-2011, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_JUTLAND_BASICS_THREAD_POSIX_H
#define TRIAGENS_JUTLAND_BASICS_THREAD_POSIX_H 1

#include <Basics/Common.h>

////////////////////////////////////////////////////////////////////////////////
/// @defgroup Threads Threads, Locks, and Conditions
////////////////////////////////////////////////////////////////////////////////

namespace triagens {
  namespace basics {
    class ConditionVariable;

    ////////////////////////////////////////////////////////////////////////////////
    /// @ingroup Threads
    /// @brief abstract base class for threads
    ///
    /// Each subclass must implement this method. A thread can be started by start
    /// and is stopped either when the method run ends or when stop is called.
    ////////////////////////////////////////////////////////////////////////////////

    class Thread {
      public:
        Thread (Thread const&);
        Thread& operator= (Thread const&);

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief process id
        ////////////////////////////////////////////////////////////////////////////////

        typedef ::pid_t pid_t;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief thread process identifier
        ////////////////////////////////////////////////////////////////////////////////

        typedef int tpid_t;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief thread identifier
        ////////////////////////////////////////////////////////////////////////////////

        typedef intptr_t tid_t;

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief thread
        ////////////////////////////////////////////////////////////////////////////////

        typedef ::pthread_t thread_t;

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief thread local storage key
        ////////////////////////////////////////////////////////////////////////////////

        typedef pthread_key_t tlsKey;

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns the process id
        ////////////////////////////////////////////////////////////////////////////////

        static pid_t currentProcessId ();

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns the thread process id
        ////////////////////////////////////////////////////////////////////////////////

        static tpid_t currentThreadProcessId ();

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns the thread id
        ////////////////////////////////////////////////////////////////////////////////

        static tid_t currentThreadId ();

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief creates a thread local storage
        ////////////////////////////////////////////////////////////////////////////////

        static bool createKey (tlsKey& key, void (*destructor)(void*));

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief gets a thread local storage
        ////////////////////////////////////////////////////////////////////////////////

        static void* specific (tlsKey& key);

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief sets a thread local storage
        ////////////////////////////////////////////////////////////////////////////////

        static bool setSpecific (tlsKey& key, void*);

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief constructs a thread
        ////////////////////////////////////////////////////////////////////////////////

        explicit
        Thread (const string& name);

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief deletes the thread
        ////////////////////////////////////////////////////////////////////////////////

        virtual ~Thread ();

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief getter for running
        ////////////////////////////////////////////////////////////////////////////////

        bool isRunning ();

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief returns a thread identifier
        ////////////////////////////////////////////////////////////////////////////////

        tid_t threadId ();

      public:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief start the thread
        ////////////////////////////////////////////////////////////////////////////////

        bool start (ConditionVariable * _finishedCondition = 0);

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief stop the thread
        ////////////////////////////////////////////////////////////////////////////////

        void stop ();

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief join the thread
        ////////////////////////////////////////////////////////////////////////////////

        void join ();

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief send signal to thread
        ////////////////////////////////////////////////////////////////////////////////

        void sendSignal (int signal);

      protected:

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief allows the descendent object to perform cleanup
        ////////////////////////////////////////////////////////////////////////////////

        virtual void cleanup () {
        }

        ////////////////////////////////////////////////////////////////////////////////
        /// @brief the thread program
        ////////////////////////////////////////////////////////////////////////////////

        virtual void run () = 0;


        ////////////////////////////////////////////////////////////////////////////////
        /// @brief allows asynchrounous cancelation
        ////////////////////////////////////////////////////////////////////////////////

        void allowAsynchronousCancelation ();

      private:
        static void* startThread (void* arg);
        void runMe ();

      private:
        const string _name;
        bool _asynchronousCancelation;

        pthread_t _thread;

        ConditionVariable* _finishedCondition;
        volatile sig_atomic_t _started;
        volatile sig_atomic_t _running;
    };
  }
}

#endif
