// Copyright (C) 2020 T. Zachary Laine
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// Warning! This file is autogenerated.
#ifndef BOOST_TEXT_DATA_LN_HPP
#define BOOST_TEXT_DATA_LN_HPP

#include <boost/text/string_view.hpp>


namespace boost { namespace text { namespace data { namespace ln {

inline string_view phonetic_collation_tailoring()
{
    return string_view((char const *)
u8R"(  
&E<ɛ<<<Ɛ
&O<<ɔ<<<Ɔ
&G<gb<<<gB<<<Gb<<<GB
&K<kp<<<kP<<<Kp<<<KP
&M<mb<<<mB<<<Mb<<<MB<mf<<<mF<<<Mf<<<MF<mp<<<mP<<<Mp<<<MP<mv<<<mV<<<Mv<<<MV
&N<nd<<<nD<<<Nd<<<ND<ng<<<nG<<<Ng<<<NG<ngb<<<ngB<<<nGb<<<nGB<<<Ngb<<<NgB<<<NGB<nk
  <<<nK<<<Nk<<<NK<ns<<<nS<<<Ns<<<NS<nt<<<nT<<<Nt<<<NT<ny<<<nY<<<Ny<<<NY<nz<<<nZ<<<Nz
  <<<NZ
&S<sh<<<sH<<<Sh<<<SH
&T<ts<<<tS<<<Ts<<<TS
  )");
}

inline string_view standard_collation_tailoring()
{
    return string_view((char const *)
u8R"(  
&E<ɛ<<<Ɛ
&O<<ɔ<<<Ɔ
  )");
}


}}}}

#endif
