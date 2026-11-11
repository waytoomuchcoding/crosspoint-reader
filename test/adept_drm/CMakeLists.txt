find_package(EXPAT REQUIRED)

add_executable(AdeptDrmTest
  AdeptDrmTest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs/HostSyncFakes.cpp
  ${REPO_ROOT}/src/network/AdeptFulfiller.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(AdeptDrmTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/include
)

target_link_libraries(AdeptDrmTest PRIVATE
  crosspoint_test_common
  crosspoint_arduinojson
  EXPAT::EXPAT
  GTest::gtest_main
)

gtest_discover_tests(AdeptDrmTest)
