#include "mellon/futures.h"

using namespace mellon;

detail::invalid_pointer_type detail::invalid_pointer_inline_value;
detail::invalid_pointer_type detail::invalid_pointer_future_abandoned;
detail::invalid_pointer_type detail::invalid_pointer_promise_abandoned;
detail::invalid_pointer_type detail::invalid_pointer_promise_fulfilled;

const char* promise_abandoned_error::what() const noexcept {
  return "promise abandoned";
}

#ifdef MELLON_RECORD_BACKTRACE
#define UNW_LOCAL_ONLY
#include <cxxabi.h>
#include <libunwind.h>
#include <iostream>

thread_local std::string* detail::current_backtrace_ptr;

auto detail::generate_backtrace_string() noexcept -> std::string try {

  std::string bt;

  unw_context_t context;
  unw_getcontext(&context);

  unw_cursor_t cursor;
  unw_init_local(&cursor, &context);

  std::size_t i = 1;
  while (unw_step(&cursor) > 0) {
    constexpr size_t kMax = 1024;
    char mangled[kMax];
    unw_word_t offset;
    unw_get_proc_name(&cursor, mangled, kMax, &offset);

    int ok;
    size_t len = kMax;
    char* demangled = abi::__cxa_demangle(mangled, nullptr, &len, &ok);

    bt += '#';
    bt += std::to_string(i++);
    bt += ' ';
    bt += (ok == 0 ? demangled : mangled);
    bt += "\n";
    free(demangled);
  }
  return bt;
} catch (...) {
  return std::string{};
}
#endif

#ifdef FUTURES_COUNT_ALLOC
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <sstream>

namespace {
template <typename T, std::size_t N>
std::ostream& operator<<(std::ostream& os, std::array<T, N> const& arr) {
  os << "[";
  for (std::size_t i = 0; i < N; i++) {
    if (i != 0) {
      os << ",";
    }
    os << arr[i];
  }
  os << "]";
  return os;
}
}  // namespace

struct allocation_printer {
  ~allocation_printer() {
    std::stringstream ss;
    ss << "[FUTURES]" << mellon::detail::message_prefix
       << " number_of_allocations=" << mellon::detail::number_of_allocations
       << " number_of_bytes_allocated=" << mellon::detail::number_of_bytes_allocated
       << " number_of_inline_value_placements=" << mellon::detail::number_of_inline_value_placements
       << " number_of_inline_value_allocs=" << mellon::detail::number_of_inline_value_allocs
       << " histogram_value_sizes=" << mellon::detail::histogram_value_sizes
       << " number_of_and_then_on_inline_future=" << mellon::detail::number_of_and_then_on_inline_future
       << " number_of_temporary_objects=" << mellon::detail::number_of_temporary_objects
       << " number_of_step_usage=" << mellon::detail::number_of_step_usage
       << " number_of_promises_created=" << mellon::detail::number_of_promises_created
       << " number_of_prealloc_usage=" << mellon::detail::number_of_prealloc_usage
       << " number_of_final_usage=" << mellon::detail::number_of_final_usage
       << " histogram_final_lambda_sizes=" << mellon::detail::histogram_final_lambda_sizes
       << std::endl;
    std::cout << ss.str();
  }
};

#ifdef FUTURES_COUNT_ALLOC
std::atomic<std::size_t> mellon::detail::number_of_allocations = 0;
std::atomic<std::size_t> mellon::detail::number_of_bytes_allocated = 0;
std::atomic<std::size_t> mellon::detail::number_of_inline_value_placements = 0;
std::atomic<std::size_t> mellon::detail::number_of_temporary_objects = 0;
std::atomic<std::size_t> mellon::detail::number_of_prealloc_usage = 0;

std::atomic<std::size_t> mellon::detail::number_of_inline_value_allocs = 0;
std::atomic<std::size_t> mellon::detail::number_of_final_usage = 0;
std::atomic<std::size_t> mellon::detail::number_of_step_usage = 0;
std::atomic<std::size_t> mellon::detail::number_of_promises_created = 0;

std::atomic<std::size_t> mellon::detail::number_of_and_then_on_inline_future = 0;

std::array<std::atomic<std::size_t>, 10> mellon::detail::histogram_value_sizes{};
std::array<std::atomic<std::size_t>, 10> mellon::detail::histogram_final_lambda_sizes{};
std::string mellon::detail::message_prefix;

allocation_printer printer;
#endif
#endif
