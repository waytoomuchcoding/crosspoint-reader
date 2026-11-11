find_package(EXPAT REQUIRED)

add_executable(TelegramSyncTest
  TelegramSyncTest.cpp
  ${REPO_ROOT}/test/adept_drm/stubs/HostSyncFakes.cpp
  ${REPO_ROOT}/src/TelegramCredentialStore.cpp
  ${REPO_ROOT}/src/network/TelegramBookSync.cpp
  ${REPO_ROOT}/src/network/AdeptFulfiller.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Txt/TxtCoverMeta.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TelegramSyncTest PRIVATE
  ${REPO_ROOT}/test/adept_drm/stubs
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/SdCardFont
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Txt
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/include
)

target_link_libraries(TelegramSyncTest PRIVATE
  crosspoint_test_common
  crosspoint_arduinojson
  EXPAT::EXPAT
  GTest::gtest_main
)

gtest_discover_tests(TelegramSyncTest)
