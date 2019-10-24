//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2004-2019. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////
#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <boost/container/stable_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/container/deque.hpp>

#include <boost/container/detail/container_or_allocator_rebind.hpp>

#include "set_test.hpp"
#include <set>

using namespace boost::container;

template<class VoidAllocatorOrContainer>
struct GetSetContainer
{
   template<class ValueType>
   struct apply
   {
      typedef flat_set < ValueType
                       , std::less<ValueType>
                       , typename boost::container::dtl::container_or_allocator_rebind<VoidAllocatorOrContainer, ValueType>::type
                        > set_type;

      typedef flat_multiset < ValueType
                            , std::less<ValueType>
                            , typename boost::container::dtl::container_or_allocator_rebind<VoidAllocatorOrContainer, ValueType>::type
                            > multiset_type;
   };
};

int main()
{
   using namespace boost::container::test;

   ////////////////////////////////////
   //    Testing sequence container implementations
   ////////////////////////////////////
   {
      typedef std::set<int>                                     MyStdSet;
      typedef std::multiset<int>                                MyStdMultiSet;

      if (0 != test::set_test
         < GetSetContainer<vector<int> >::apply<int>::set_type
         , MyStdSet
         , GetSetContainer<vector<int> >::apply<int>::multiset_type
         , MyStdMultiSet>()) {
         std::cout << "Error in set_test<vector<int> >" << std::endl;
         return 1;
      }

      if (0 != test::set_test
         < GetSetContainer<small_vector<int, 7> >::apply<int>::set_type
         , MyStdSet
         , GetSetContainer<small_vector<int, 7> >::apply<int>::multiset_type
         , MyStdMultiSet>()) {
         std::cout << "Error in set_test<small_vector<int, 7> >" << std::endl;
         return 1;
      }

       if (0 != test::set_test
         < GetSetContainer<static_vector<int, MaxElem * 10> >::apply<int>::set_type
         , MyStdSet
         , GetSetContainer<static_vector<int, MaxElem * 10> >::apply<int>::multiset_type
         , MyStdMultiSet>()) {
         std::cout << "Error in set_test<static_vector<int, MaxElem * 10> >" << std::endl;
         return 1;
      }

      if (0 != test::set_test
         < GetSetContainer<stable_vector<int> >::apply<int>::set_type
         , MyStdSet
         , GetSetContainer<stable_vector<int> >::apply<int>::multiset_type
         , MyStdMultiSet>()) {
         std::cout << "Error in set_test<stable_vector<int> >" << std::endl;
         return 1;
      }


      if (0 != test::set_test
         < GetSetContainer<deque<int> >::apply<int>::set_type
         , MyStdSet
         , GetSetContainer<deque<int> >::apply<int>::multiset_type
         , MyStdMultiSet>()) {
         std::cout << "Error in set_test<deque<int> >" << std::endl;
         return 1;
      }
   }

   return 0;
}
