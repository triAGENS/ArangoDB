# -*- mode: CMAKE; -*-
# these are the install targets for the client package.
# we can't use RUNTIME DESTINATION here.

install_debinfo(
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}/strip"
  "${CMAKE_PROJECT_NAME}"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}/"
  "${BIN_ARANGOBENCH}")

install_debinfo(
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}/strip"
  "${CMAKE_PROJECT_NAME}"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}"
  "${BIN_ARANGODUMP}")

install_debinfo(
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}/strip"
  "${CMAKE_PROJECT_NAME}/${CMAKE_INSTALL_BINDIR}"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}"
  "${BIN_ARANGOIMPORT}")

install_debinfo(
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}/strip"
  "${CMAKE_PROJECT_NAME}/${CMAKE_INSTALL_BINDIR}"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}"
  "${BIN_ARANGORESTORE}")

install_debinfo(
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}/strip"
  "${CMAKE_PROJECT_NAME}/${CMAKE_INSTALL_BINDIR}"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}"
  "${BIN_ARANGOEXPORT}")

install_debinfo(
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}/strip"
  "${CMAKE_PROJECT_NAME}/${CMAKE_INSTALL_BINDIR}"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_X}"
  "${BIN_ARANGOSH}")
