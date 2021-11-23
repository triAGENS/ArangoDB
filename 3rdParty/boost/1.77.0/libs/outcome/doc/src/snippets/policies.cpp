/* Documentation snippet
(C) 2017-2021 Niall Douglas <http://www.nedproductions.biz/> (5 commits) and Andrzej Krzemienski <akrzemi1@gmail.com> (1 commit)
File Created: Mar 2017


Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#include "../../../include/boost/outcome.hpp"

namespace outcome = BOOST_OUTCOME_V2_NAMESPACE;

//! [abort_policy]
struct abort_policy : outcome::policy::base
{
  template <class Impl> static constexpr void wide_value_check(Impl &&self)
  {
    if(!base::_has_value(std::forward<Impl>(self)))
      std::abort();
  }

  template <class Impl> static constexpr void wide_error_check(Impl &&self)
  {
    if(!base::_has_error(std::forward<Impl>(self)))
      std::abort();
  }

  template <class Impl> static constexpr void wide_exception_check(Impl &&self)
  {
    if(!base::_has_exception(std::forward<Impl>(self)))
      std::abort();
  }
};
//! [abort_policy]

//! [throwing_policy]
template <typename T, typename EC, typename EP> struct throwing_policy : outcome::policy::base
{
  static_assert(std::is_convertible<EC, std::error_code>::value, "only EC = error_code");

  template <class Impl> static constexpr void wide_value_check(Impl &&self)
  {
    if(!base::_has_value(std::forward<Impl>(self)))
    {
      if(base::_has_error(std::forward<Impl>(self)))
        throw std::system_error(base::_error(std::forward<Impl>(self)));
      else
        std::rethrow_exception(base::_exception<T, EC, EP, throwing_policy>(std::forward<Impl>(self)));
    }
  }

  template <class Impl> static constexpr void wide_error_check(Impl &&self)
  {
    if(!base::_has_error(std::forward<Impl>(self)))
    {
      if(base::_has_exception(std::forward<Impl>(self)))
        std::rethrow_exception(base::_exception<T, EC, EP, throwing_policy>(std::forward<Impl>(self)));
      else
        base::_make_ub(std::forward<Impl>(self));
    }
  }

  template <class Impl> static constexpr void wide_exception_check(Impl &&self)
  {
    if(!base::_has_exception(std::forward<Impl>(self)))
      base::_make_ub(std::forward<Impl>(self));
  }
};
//! [throwing_policy]

//! [outcome_spec]
template <typename T>
using strictOutcome =  //
outcome::basic_outcome<T, std::error_code, std::exception_ptr, abort_policy>;
//! [outcome_spec]

template <typename T, typename EC = std::error_code>
using throwingOutcome =  //
outcome::basic_outcome<T, EC, std::exception_ptr, throwing_policy<T, EC, std::exception_ptr>>;

int main()
{
  try
  {
    throwingOutcome<int> i = std::error_code{};
    i.value();  // throws
    assert(false);
  }
  catch(std::system_error const &)
  {
  }

  strictOutcome<int> i = 1;
  assert(i.value() == 1);
  i.error();  // calls abort()
}
