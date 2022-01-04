//
// cpp03/prefer_free_prefer.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2021 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <boost/asio/prefer.hpp>
#include <cassert>

template <int>
struct prop
{
  static const bool is_preferable = true;
};

template <int>
struct object
{
  template <int N>
  friend object<N> prefer(const object&, prop<N>)
  {
    return object<N>();
  }
};

namespace boost {
namespace asio {

template<int N, int M>
struct is_applicable_property<object<N>, prop<M> >
{
  static const bool value = true;
};

namespace traits {

template<int N, int M>
struct prefer_free<object<N>, prop<M> >
{
  static const bool is_valid = true;
  static const bool is_noexcept = true;
  typedef object<M> result_type;
};

} // namespace traits
} // namespace asio
} // namespace boost

int main()
{
  object<1> o1 = {};
  object<2> o2 = boost::asio::prefer(o1, prop<2>());
  object<3> o3 = boost::asio::prefer(o1, prop<2>(), prop<3>());
  object<4> o4 = boost::asio::prefer(o1, prop<2>(), prop<3>(), prop<4>());
  (void)o2;
  (void)o3;
  (void)o4;

  const object<1> o5 = {};
  object<2> o6 = boost::asio::prefer(o5, prop<2>());
  object<3> o7 = boost::asio::prefer(o5, prop<2>(), prop<3>());
  object<4> o8 = boost::asio::prefer(o5, prop<2>(), prop<3>(), prop<4>());
  (void)o6;
  (void)o7;
  (void)o8;
}
