#!/bin/sh

if [ "x${enable_prof}" = "x1" ] ; then
  export MALLOC_CONF="prof:true,prof_active:true,lg_prof_sample:0,prof_bt_max:200"
fi
