/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// polymorphic_base.cpp   

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <boost/serialization/export.hpp>

#define POLYMORPHIC_BASE_EXPORT
#include "polymorphic_base.hpp"

template<class Archive>
POLYMORPHIC_BASE_DLL_DECL void polymorphic_base::serialize(
    Archive &ar,
    const unsigned int /* file_version */
){}

POLYMORPHIC_BASE_DLL_DECL
polymorphic_base::polymorphic_base(){}
POLYMORPHIC_BASE_DLL_DECL
polymorphic_base::~polymorphic_base(){}

#include <boost/archive/polymorphic_oarchive.hpp>
#include <boost/archive/polymorphic_iarchive.hpp>

template
POLYMORPHIC_BASE_DLL_DECL void polymorphic_base::serialize(
    boost::archive::polymorphic_oarchive &,
    const unsigned int /* file_version */
);
template POLYMORPHIC_BASE_DLL_DECL void polymorphic_base::serialize(
    boost::archive::polymorphic_iarchive &,
    const unsigned int
);
BOOST_CLASS_EXPORT_IMPLEMENT(polymorphic_base)
