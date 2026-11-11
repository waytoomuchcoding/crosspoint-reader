add_executable(TextEncodingTest
  TextEncodingTest.cpp
  ${REPO_ROOT}/lib/TextEncoding/TextEncoding.cpp
  ${REPO_ROOT}/lib/Txt/TxtChapterDetector.cpp
  ${REPO_ROOT}/lib/Txt/TxtEncodingSupport.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TextEncodingTest PRIVATE
  ${REPO_ROOT}/test/txt_cover_meta/stubs
  ${REPO_ROOT}/test/book_metadata_cache/stubs
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/TextEncoding
  ${REPO_ROOT}/lib/Txt
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(TextEncodingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextEncodingTest)
