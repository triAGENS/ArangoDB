/* Unit testing for outcomes
(C) 2013-2019 Niall Douglas <http://www.nedproductions.biz/> (3 commits)


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

#include <boost/outcome/outcome.hpp>
#include <boost/outcome/try.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_monitor.hpp>

BOOST_OUTCOME_AUTO_TEST_CASE(issues_65_outcome, "BOOST_OUTCOME_TRY does not preserve the exception_ptr")
{
#ifndef BOOST_NO_EXCEPTIONS
  using namespace BOOST_OUTCOME_V2_NAMESPACE;
  auto g = []() -> outcome<int> {
    auto f = []() -> outcome<int> {
      try
      {
        throw std::runtime_error{"XXX"};
      }
      catch(...)
      {
        return boost::current_exception();
      }
    };
    BOOST_OUTCOME_TRY(ans, (f()));
    return ans;
  };
  outcome<int> o = g();
  BOOST_CHECK(!o);
  BOOST_CHECK(o.has_exception());
  try
  {
    o.value();
    BOOST_CHECK(false);
  }
  catch(const std::runtime_error &e)
  {
    BOOST_CHECK(!strcmp(e.what(), "XXX"));
  }
#endif
}
