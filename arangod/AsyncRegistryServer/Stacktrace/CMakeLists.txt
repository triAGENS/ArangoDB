add_library(arango_async_registry_stacktrace INTERFACE
  depth_first.h
  forest.h)
target_include_directories(arango_async_registry_stacktrace
  INTERFACE
  ${PROJECT_SOURCE_DIR}/arangod)
