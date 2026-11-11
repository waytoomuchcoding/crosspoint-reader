add_executable(TxtCoverMetaTest
  TxtCoverMetaTest.cpp
  ${REPO_ROOT}/src/util/BookCoverStore.cpp
  ${REPO_ROOT}/lib/Txt/Txt.cpp
  ${REPO_ROOT}/lib/Txt/TxtChapterDetector.cpp
  ${REPO_ROOT}/lib/Txt/TxtCoverMeta.cpp
  ${REPO_ROOT}/lib/Txt/TxtEncodingSupport.cpp
  ${REPO_ROOT}/lib/Txt/TxtToHtml.cpp
  ${REPO_ROOT}/lib/TextEncoding/TextEncoding.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TxtCoverMetaTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/test/book_metadata_cache/stubs
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/TextEncoding
  ${REPO_ROOT}/lib/Txt
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(TxtCoverMetaTest PRIVATE
  crosspoint_test_common
  crosspoint_arduinojson
  GTest::gtest_main
)

gtest_discover_tests(TxtCoverMetaTest)
