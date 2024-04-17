/* iowin32.c -- IO base function header for compress/uncompress .zip
     Version 1.1, February 14h, 2010
     part of the MiniZip project - (
   http://www.winimage.com/zLibDll/minizip.html )

         Copyright (C) 1998-2010 Gilles Vollant (minizip) (
   http://www.winimage.com/zLibDll/minizip.html )

         Modifications for Zip64 support
         Copyright (C) 2009-2010 Mathias Svensson ( http://result42.com )

     For more info read MiniZip_info.txt

*/
#include <unicode/unistr.h>
#include <stdlib.h>

#include "zlib.h"
#include "ioapi.h"
#include "iowin32.h"

#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE (0xFFFFFFFF)
#endif

#ifndef INVALID_SET_FILE_POINTER
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#endif

voidpf ZCALLBACK win32_open_file_func OF((voidpf opaque, char const* filename,
                                          int mode));
uLong ZCALLBACK win32_read_file_func OF((voidpf opaque, voidpf stream,
                                         void* buf, uLong size));
uLong ZCALLBACK win32_write_file_func OF((voidpf opaque, voidpf stream,
                                          const void* buf, uLong size));
ZPOS64_T ZCALLBACK win32_tell64_file_func OF((voidpf opaque, voidpf stream));
long ZCALLBACK win32_seek64_file_func OF((voidpf opaque, voidpf stream,
                                          ZPOS64_T offset, int origin));
int ZCALLBACK win32_close_file_func OF((voidpf opaque, voidpf stream));
int ZCALLBACK win32_error_file_func OF((voidpf opaque, voidpf stream));

typedef struct {
  HANDLE hf;
  int error;
} WIN32FILE_IOWIN;

static void win32_translate_open_mode(int mode, DWORD* lpdwDesiredAccess,
                                      DWORD* lpdwCreationDisposition,
                                      DWORD* lpdwShareMode,
                                      DWORD* lpdwFlagsAndAttributes) {
  *lpdwDesiredAccess = *lpdwShareMode = *lpdwFlagsAndAttributes =
      *lpdwCreationDisposition = 0;

  if ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) == ZLIB_FILEFUNC_MODE_READ) {
    *lpdwDesiredAccess = GENERIC_READ;
    *lpdwCreationDisposition = OPEN_EXISTING;
    *lpdwShareMode = FILE_SHARE_READ;
  } else if (mode & ZLIB_FILEFUNC_MODE_EXISTING) {
    *lpdwDesiredAccess = GENERIC_WRITE | GENERIC_READ;
    *lpdwCreationDisposition = OPEN_EXISTING;
  } else if (mode & ZLIB_FILEFUNC_MODE_CREATE) {
    *lpdwDesiredAccess = GENERIC_WRITE | GENERIC_READ;
    *lpdwCreationDisposition = CREATE_ALWAYS;
  }
}

static voidpf win32_build_iowin(HANDLE hFile) {
  voidpf ret = NULL;

  if ((hFile != NULL) && (hFile != INVALID_HANDLE_VALUE)) {
    WIN32FILE_IOWIN w32fiow;
    w32fiow.hf = hFile;
    w32fiow.error = 0;
    ret = malloc(sizeof(WIN32FILE_IOWIN));

    if (ret == NULL)
      CloseHandle(hFile);
    else
      *((WIN32FILE_IOWIN*)ret) = w32fiow;
  }
  return ret;
}

voidpf ZCALLBACK win32_open64_file_func(voidpf opaque, const void* filename,
                                        int mode) {
  DWORD dwDesiredAccess, dwCreationDisposition, dwShareMode,
      dwFlagsAndAttributes;
  HANDLE hFile = NULL;

  win32_translate_open_mode(mode, &dwDesiredAccess, &dwCreationDisposition,
                            &dwShareMode, &dwFlagsAndAttributes);

  if ((filename != NULL) && (dwDesiredAccess != 0))
    hFile = CreateFile((LPCTSTR)filename, dwDesiredAccess, dwShareMode, NULL,
                       dwCreationDisposition, dwFlagsAndAttributes, NULL);

  return win32_build_iowin(hFile);
}

voidpf ZCALLBACK win32_open64_file_funcA(voidpf opaque, const void* filename,
                                         int mode) {
  DWORD dwDesiredAccess, dwCreationDisposition, dwShareMode,
      dwFlagsAndAttributes;
  HANDLE hFile = NULL;

  win32_translate_open_mode(mode, &dwDesiredAccess, &dwCreationDisposition,
                            &dwShareMode, &dwFlagsAndAttributes);

  if ((filename != NULL) && (dwDesiredAccess != 0)) {
    icu_64_64::UnicodeString fn((LPSTR)filename);
    hFile = CreateFileW((wchar_t*)fn.getTerminatedBuffer(), dwDesiredAccess,
                        dwShareMode, NULL, dwCreationDisposition,
                        dwFlagsAndAttributes, NULL);
  }
  return win32_build_iowin(hFile);
}

voidpf ZCALLBACK win32_open64_file_funcW(voidpf opaque, const void* filename,
                                         int mode) {
  DWORD dwDesiredAccess, dwCreationDisposition, dwShareMode,
      dwFlagsAndAttributes;
  HANDLE hFile = NULL;

  win32_translate_open_mode(mode, &dwDesiredAccess, &dwCreationDisposition,
                            &dwShareMode, &dwFlagsAndAttributes);

  if ((filename != NULL) && (dwDesiredAccess != 0))
    hFile = CreateFileW((LPCWSTR)filename, dwDesiredAccess, dwShareMode, NULL,
                        dwCreationDisposition, dwFlagsAndAttributes, NULL);

  return win32_build_iowin(hFile);
}

voidpf ZCALLBACK win32_open_file_func(voidpf opaque, char const* filename,
                                      int mode) {
  DWORD dwDesiredAccess, dwCreationDisposition, dwShareMode,
      dwFlagsAndAttributes;
  HANDLE hFile = NULL;

  win32_translate_open_mode(mode, &dwDesiredAccess, &dwCreationDisposition,
                            &dwShareMode, &dwFlagsAndAttributes);

  if ((filename != NULL) && (dwDesiredAccess != 0))
    hFile = CreateFile((LPCTSTR)filename, dwDesiredAccess, dwShareMode, NULL,
                       dwCreationDisposition, dwFlagsAndAttributes, NULL);

  return win32_build_iowin(hFile);
}

uLong ZCALLBACK win32_read_file_func(voidpf opaque, voidpf stream, void* buf,
                                     uLong size) {
  uLong ret = 0;
  HANDLE hFile = NULL;
  if (stream != NULL) hFile = ((WIN32FILE_IOWIN*)stream)->hf;

  if (hFile != NULL) {
    if (!ReadFile(hFile, buf, size, &ret, NULL)) {
      DWORD dwErr = GetLastError();
      if (dwErr == ERROR_HANDLE_EOF) dwErr = 0;
      ((WIN32FILE_IOWIN*)stream)->error = (int)dwErr;
    }
  }

  return ret;
}

uLong ZCALLBACK win32_write_file_func(voidpf opaque, voidpf stream,
                                      const void* buf, uLong size) {
  uLong ret = 0;
  HANDLE hFile = NULL;
  if (stream != NULL) hFile = ((WIN32FILE_IOWIN*)stream)->hf;

  if (hFile != NULL) {
    if (!WriteFile(hFile, buf, size, &ret, NULL)) {
      DWORD dwErr = GetLastError();
      if (dwErr == ERROR_HANDLE_EOF) dwErr = 0;
      ((WIN32FILE_IOWIN*)stream)->error = (int)dwErr;
    }
  }

  return ret;
}

long ZCALLBACK win32_tell_file_func(voidpf opaque, voidpf stream) {
  long ret = -1;
  HANDLE hFile = NULL;
  if (stream != NULL) hFile = ((WIN32FILE_IOWIN*)stream)->hf;
  if (hFile != NULL) {
    DWORD dwSet = SetFilePointer(hFile, 0, NULL, FILE_CURRENT);
    if (dwSet == INVALID_SET_FILE_POINTER) {
      DWORD dwErr = GetLastError();
      ((WIN32FILE_IOWIN*)stream)->error = (int)dwErr;
      ret = -1;
    } else
      ret = (long)dwSet;
  }
  return ret;
}

ZPOS64_T ZCALLBACK win32_tell64_file_func(voidpf opaque, voidpf stream) {
  ZPOS64_T ret = (ZPOS64_T)-1;
  HANDLE hFile = NULL;
  if (stream != NULL) hFile = ((WIN32FILE_IOWIN*)stream)->hf;

  if (hFile) {
    LARGE_INTEGER li;
    li.QuadPart = 0;
    li.u.LowPart =
        SetFilePointer(hFile, li.u.LowPart, &li.u.HighPart, FILE_CURRENT);
    if ((li.LowPart == 0xFFFFFFFF) && (GetLastError() != NO_ERROR)) {
      DWORD dwErr = GetLastError();
      ((WIN32FILE_IOWIN*)stream)->error = (int)dwErr;
      ret = (ZPOS64_T)-1;
    } else
      ret = li.QuadPart;
  }
  return ret;
}

long ZCALLBACK win32_seek_file_func(voidpf opaque, voidpf stream, uLong offset,
                                    int origin) {
  DWORD dwMoveMethod = 0xFFFFFFFF;
  HANDLE hFile = NULL;

  long ret = -1;
  if (stream != NULL) hFile = ((WIN32FILE_IOWIN*)stream)->hf;
  switch (origin) {
    case ZLIB_FILEFUNC_SEEK_CUR:
      dwMoveMethod = FILE_CURRENT;
      break;
    case ZLIB_FILEFUNC_SEEK_END:
      dwMoveMethod = FILE_END;
      break;
    case ZLIB_FILEFUNC_SEEK_SET:
      dwMoveMethod = FILE_BEGIN;
      break;
    default:
      return -1;
  }

  if (hFile != NULL) {
    DWORD dwSet = SetFilePointer(hFile, offset, NULL, dwMoveMethod);
    if (dwSet == INVALID_SET_FILE_POINTER) {
      DWORD dwErr = GetLastError();
      ((WIN32FILE_IOWIN*)stream)->error = (int)dwErr;
      ret = -1;
    } else
      ret = 0;
  }
  return ret;
}

long ZCALLBACK win32_seek64_file_func(voidpf opaque, voidpf stream,
                                      ZPOS64_T offset, int origin) {
  DWORD dwMoveMethod = 0xFFFFFFFF;
  HANDLE hFile = NULL;
  long ret = -1;

  if (stream != NULL) hFile = ((WIN32FILE_IOWIN*)stream)->hf;

  switch (origin) {
    case ZLIB_FILEFUNC_SEEK_CUR:
      dwMoveMethod = FILE_CURRENT;
      break;
    case ZLIB_FILEFUNC_SEEK_END:
      dwMoveMethod = FILE_END;
      break;
    case ZLIB_FILEFUNC_SEEK_SET:
      dwMoveMethod = FILE_BEGIN;
      break;
    default:
      return -1;
  }

  if (hFile) {
    LARGE_INTEGER* li = (LARGE_INTEGER*)&offset;
    DWORD dwSet =
        SetFilePointer(hFile, li->u.LowPart, &li->u.HighPart, dwMoveMethod);
    if (dwSet == INVALID_SET_FILE_POINTER) {
      DWORD dwErr = GetLastError();
      ((WIN32FILE_IOWIN*)stream)->error = (int)dwErr;
      ret = -1;
    } else
      ret = 0;
  }
  return ret;
}

int ZCALLBACK win32_close_file_func(voidpf opaque, voidpf stream) {
  int ret = -1;

  if (stream != NULL) {
    HANDLE hFile;
    hFile = ((WIN32FILE_IOWIN*)stream)->hf;
    if (hFile != NULL) {
      CloseHandle(hFile);
      ret = 0;
    }
    free(stream);
  }
  return ret;
}

int ZCALLBACK win32_error_file_func(voidpf opaque, voidpf stream) {
  int ret = -1;
  if (stream != NULL) {
    ret = ((WIN32FILE_IOWIN*)stream)->error;
  }
  return ret;
}

void fill_win32_filefunc(zlib_filefunc_def* pzlib_filefunc_def) {
  pzlib_filefunc_def->zopen_file = win32_open_file_func;
  pzlib_filefunc_def->zread_file = win32_read_file_func;
  pzlib_filefunc_def->zwrite_file = win32_write_file_func;
  pzlib_filefunc_def->ztell_file = win32_tell_file_func;
  pzlib_filefunc_def->zseek_file = win32_seek_file_func;
  pzlib_filefunc_def->zclose_file = win32_close_file_func;
  pzlib_filefunc_def->zerror_file = win32_error_file_func;
  pzlib_filefunc_def->opaque = NULL;
}

void fill_win32_filefunc64(zlib_filefunc64_def* pzlib_filefunc_def) {
  pzlib_filefunc_def->zopen64_file = win32_open64_file_func;
  pzlib_filefunc_def->zread_file = win32_read_file_func;
  pzlib_filefunc_def->zwrite_file = win32_write_file_func;
  pzlib_filefunc_def->ztell64_file = win32_tell64_file_func;
  pzlib_filefunc_def->zseek64_file = win32_seek64_file_func;
  pzlib_filefunc_def->zclose_file = win32_close_file_func;
  pzlib_filefunc_def->zerror_file = win32_error_file_func;
  pzlib_filefunc_def->opaque = NULL;
}

void fill_win32_filefunc64A(zlib_filefunc64_def* pzlib_filefunc_def) {
  pzlib_filefunc_def->zopen64_file = win32_open64_file_funcA;
  pzlib_filefunc_def->zread_file = win32_read_file_func;
  pzlib_filefunc_def->zwrite_file = win32_write_file_func;
  pzlib_filefunc_def->ztell64_file = win32_tell64_file_func;
  pzlib_filefunc_def->zseek64_file = win32_seek64_file_func;
  pzlib_filefunc_def->zclose_file = win32_close_file_func;
  pzlib_filefunc_def->zerror_file = win32_error_file_func;
  pzlib_filefunc_def->opaque = NULL;
}

void fill_win32_filefunc64W(zlib_filefunc64_def* pzlib_filefunc_def) {
  pzlib_filefunc_def->zopen64_file = win32_open64_file_funcW;
  pzlib_filefunc_def->zread_file = win32_read_file_func;
  pzlib_filefunc_def->zwrite_file = win32_write_file_func;
  pzlib_filefunc_def->ztell64_file = win32_tell64_file_func;
  pzlib_filefunc_def->zseek64_file = win32_seek64_file_func;
  pzlib_filefunc_def->zclose_file = win32_close_file_func;
  pzlib_filefunc_def->zerror_file = win32_error_file_func;
  pzlib_filefunc_def->opaque = NULL;
}
