macro(get_cxx_std_flags FLAGS_VARIABLE)
  if( CMAKE_CXX_STANDARD_REQUIRED )
    set(${FLAGS_VARIABLE} ${CMAKE_CXX${CMAKE_CXX_STANDARD}_STANDARD_COMPILE_OPTION})
  else()
    set(${FLAGS_VARIABLE} ${CMAKE_CXX${CMAKE_CXX_STANDARD}_EXTENSION_COMPILE_OPTION})
  endif()
endmacro()
