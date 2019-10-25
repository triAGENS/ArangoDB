/* Unit testing for outcomes
(C) 2013-2019 Niall Douglas <http://www.nedproductions.biz/> (9 commits)


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
#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_monitor.hpp>

BOOST_OUTCOME_AUTO_TEST_CASE(works_outcome_constexpr, "Tests that outcome works as intended in a constexpr evaluation context")
{
  using namespace BOOST_OUTCOME_V2_NAMESPACE;

  static_assert(std::is_literal_type<result<int, void, void>>::value, "result<int, void, void> is not a literal type!");
  static_assert(std::is_literal_type<outcome<int, void, void>>::value, "outcome<int, void, void> is not a literal type!");

  // Unfortunately result<T> can never be a literal type as error_code can never be literal
  //
  // It can however be trivially destructible as error_code is trivially destructible. That
  // makes possible lots of compiler optimisations
  static_assert(std::is_trivially_destructible<result<int>>::value, "result<int> is not trivially destructible!");
  static_assert(std::is_trivially_destructible<result<void>>::value, "result<void> is not trivially destructible!");

  // outcome<T> default has no trivial operations, but if configured it can become so
  static_assert(std::is_trivially_destructible<outcome<int, boost::system::error_code, void>>::value, "outcome<int, boost::system::error_code, void> is not trivially destructible!");

  {
    // Test compatible results can be constructed from one another
    constexpr result<int, long> g(in_place_type<int>, 5);
    constexpr result<long, int> g2(g);
    static_assert(g.has_value(), "");
    static_assert(!g.has_error(), "");
    static_assert(g.assume_value() == 5, "");  // value() with UDT E won't compile
    static_assert(g2.has_value(), "");
    static_assert(!g2.has_error(), "");
    static_assert(g2.assume_value() == 5, "");  // value() with UDT E won't compile
    constexpr result<void, int> g3(in_place_type<void>);
    constexpr result<long, int> g4(g3);
    constexpr result<int, void> g5(in_place_type<void>);
    constexpr result<long, int> g6(g5);
    (void) g4;
    (void) g6;

    // Test void
    constexpr result<void, int> h(in_place_type<void>);
    static_assert(h.has_value(), "");
    constexpr result<int, void> h2(in_place_type<void>);
    static_assert(!h2.has_value(), "");
    static_assert(h2.has_error(), "");

    // Test const
    constexpr result<const int, void> i(5);
    constexpr result<const int, void> i2(i);
    (void) i2;
  }
  {
    // Test compatible outcomes can be constructed from one another
    constexpr outcome<int, long, char *> g(in_place_type<int>, 5);
    constexpr outcome<long, int, const char *> g2(g);
    static_assert(g.has_value(), "");
    static_assert(!g.has_error(), "");
    static_assert(!g.has_exception(), "");
    static_assert(g.assume_value() == 5, "");  // value() with UDT E won't compile
    static_assert(g2.has_value(), "");
    static_assert(!g2.has_error(), "");
    static_assert(!g2.has_exception(), "");
    static_assert(g2.assume_value() == 5, "");  // value() with UDT E won't compile
    constexpr outcome<void, int, char *> g3(in_place_type<void>);
    constexpr outcome<long, int, const char *> g4(g3);
    constexpr outcome<int, void, char *> g5(in_place_type<void>);
    constexpr outcome<long, int, const char *> g6(g5);
    (void) g4;
    (void) g6;
  }
}
