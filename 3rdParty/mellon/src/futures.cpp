#include "mellon/futures.h"

using namespace mellon;

detail::invalid_pointer_type detail::invalid_pointer_inline_value;
detail::invalid_pointer_type detail::invalid_pointer_future_abandoned;
detail::invalid_pointer_type detail::invalid_pointer_promise_abandoned;
detail::invalid_pointer_type detail::invalid_pointer_promise_fulfilled;

const char* promise_abandoned_error::what() const noexcept {
  return "promise abandoned";
}

#include <sys/types.h>
#include <unistd.h>

#include <iostream>

struct allocation_printer {
  ~allocation_printer() {
    std::cerr << "[FUTURES] number_of_allocations=" << mellon::detail::number_of_allocations
              << " number_of_bytes_allocated=" << mellon::detail::number_of_bytes_allocated
              << " number_of_inline_value_placements=" << mellon::detail::number_of_inline_value_placements
              << " number_of_temporary_objects=" << mellon::detail::number_of_temporary_objects
              << " number_of_prealloc_usage=" << mellon::detail::number_of_prealloc_usage
              << std::endl;
  }
};

#ifdef FUTURES_COUNT_ALLOC
std::atomic<std::size_t> mellon::detail::number_of_allocations = 0;
std::atomic<std::size_t> mellon::detail::number_of_bytes_allocated = 0;
std::atomic<std::size_t> mellon::detail::number_of_inline_value_placements = 0;
std::atomic<std::size_t> mellon::detail::number_of_temporary_objects = 0;
std::atomic<std::size_t> mellon::detail::number_of_prealloc_usage = 0;
allocation_printer printer;
#endif
