cmake_minimum_required(VERSION 3.20)
project(pocketpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(pocketpp_lib
  src/compiler.cpp
  src/environment.cpp
  src/interpreter.cpp
  src/lexer.cpp
  src/parser.cpp
  src/pocketpp.cpp
  src/vm.cpp
)

target_include_directories(pocketpp_lib
  PUBLIC  include
  PRIVATE src
)

target_compile_features(pocketpp_lib PUBLIC cxx_std_20)

add_executable(pocketpp src/main.cpp)
target_link_libraries(pocketpp PRIVATE pocketpp_lib)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
  )
  FetchContent_MakeAvailable(Catch2)

  set(TESTS_LANG_DIR "${CMAKE_SOURCE_DIR}/tests/lang")

  add_executable(pocketpp_tests tests/pocketpp_tests.cpp)
  target_link_libraries(pocketpp_tests PRIVATE pocketpp_lib Catch2::Catch2WithMain)
  target_compile_definitions(pocketpp_tests PRIVATE
    TESTS_LANG_DIR="${TESTS_LANG_DIR}"
  )

  include(Catch)
  catch_discover_tests(pocketpp_tests)
endif()
