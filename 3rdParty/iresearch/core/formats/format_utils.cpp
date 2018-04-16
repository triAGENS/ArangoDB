////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 by EMC Corporation, All Rights Reserved
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "shared.hpp"
#include "format_utils.hpp"

#include "index/index_meta.hpp"

#include "formats/formats.hpp"

NS_ROOT

void validate_footer(iresearch::index_input& in) {
  const int64_t remain = in.length() - in.file_pointer();
  if (remain != format_utils::FOOTER_LEN) {
    // invalid position
    throw iresearch::index_error();
  }

  const int32_t magic = in.read_int();
  if (magic != format_utils::FOOTER_MAGIC) {
    // invalid magic number 
    throw iresearch::index_error();
  }

  const int32_t alg_id = in.read_int();
  if (alg_id != 0) {
    // invalid algorithm
    throw iresearch::index_error();
  }
}

NS_BEGIN(format_utils)

void write_header(index_output& out, const string_ref& format, int32_t ver) {
  out.write_int(FORMAT_MAGIC);
  write_string(out, format);
  out.write_int(ver);
}

void write_footer(index_output& out) {
  out.write_int(FOOTER_MAGIC);
  out.write_int(0);
  out.write_long(out.checksum());
}

int32_t check_header(
    index_input& in, 
    const string_ref& req_format,
    int32_t min_ver, int32_t max_ver) {
  const int32_t magic = in.read_int();
  if (FORMAT_MAGIC != magic) {
    // index format
    throw index_error();
  }

  const auto format = read_string<std::string>(in);
  if (compare(req_format, format) != 0) {
    // invalid format
    throw index_error();
  }

  const int32_t ver = in.read_int();
  if (ver < min_ver || ver > max_ver) {
    // invalid version
    throw index_error();
  }

  return ver;
}

NS_END
NS_END
