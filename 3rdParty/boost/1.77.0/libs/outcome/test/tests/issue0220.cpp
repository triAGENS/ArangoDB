/* Unit testing for outcomes
(C) 2013-2021 Niall Douglas <http://www.nedproductions.biz/> (1 commit)


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

#include <boost/outcome/experimental/status_result.hpp>
#include <boost/outcome/try.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_monitor.hpp>

#ifndef BOOST_OUTCOME_SYSTEM_ERROR2_NOT_POSIX
namespace issues220
{
  namespace outcome_e = BOOST_OUTCOME_V2_NAMESPACE::experimental;

  template <class T, class E = outcome_e::error> using Result = outcome_e::status_result<T, E>;

  template <class T> using PosixResult = outcome_e::status_result<T, outcome_e::posix_code>;

  Result<int> convert(const PosixResult<int> &posix_result) { return Result<int>(posix_result); }
}  // namespace issues220
#endif


BOOST_OUTCOME_AUTO_TEST_CASE(issues_0220_test, "ubsan reports reference binding to null pointer")
{
#ifndef BOOST_OUTCOME_SYSTEM_ERROR2_NOT_POSIX
  using namespace issues220;
  BOOST_CHECK(convert(PosixResult<int>(0)).value() == 0);
#endif
}
