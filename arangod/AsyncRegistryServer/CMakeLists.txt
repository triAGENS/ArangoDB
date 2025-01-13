target_sources(arangoserver PRIVATE
  Feature.cpp
  RestHandler.cpp)
target_link_libraries(arangoserver
  arango_async_registry_stacktrace)

add_subdirectory(Stacktrace)
