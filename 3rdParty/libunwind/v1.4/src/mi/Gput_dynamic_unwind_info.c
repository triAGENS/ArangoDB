/* libunwind - a platform-independent unwind library
   Copyright (C) 2001-2002, 2005 Hewlett-Packard Co
        Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

This file is part of libunwind.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#include "libunwind_i.h"

HIDDEN void
unwi_put_dynamic_unwind_info (unw_addr_space_t as, unw_proc_info_t *pi,
                              void *arg)
{
  switch (pi->format)
    {
    case UNW_INFO_FORMAT_DYNAMIC:
#ifndef UNW_LOCAL_ONLY
# ifdef UNW_REMOTE_ONLY
      unwi_dyn_remote_put_unwind_info (as, pi, arg);
# else
      if (as != unw_local_addr_space)
        unwi_dyn_remote_put_unwind_info (as, pi, arg);
# endif
#endif
      break;

    case UNW_INFO_FORMAT_TABLE:
    case UNW_INFO_FORMAT_REMOTE_TABLE:
#ifdef tdep_put_unwind_info
      tdep_put_unwind_info (as, pi, arg);
      break;
#endif
      /* fall through */
    default:
      break;
    }
}
