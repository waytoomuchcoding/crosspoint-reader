find_package(EXPAT REQUIRED)

add_executable(Phase3TextEncodingWhiteboxTest
  Phase3TextEncodingWhiteboxTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(Phase3TextEncodingWhiteboxTest PRIVATE
  ${REPO_ROOT}/lib/TextEncoding
  ${REPO_ROOT}/lib/Utf8
)
target_link_libraries(Phase3TextEncodingWhiteboxTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(Phase3TextEncodingWhiteboxTest)

add_executable(Phase3CoreBugProofsTest
  Phase3CoreBugProofsTest.cpp
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
  ${REPO_ROOT}/src/util/BookCoverStore.cpp
  ${REPO_ROOT}/lib/ZhDict/ZhDict.cpp
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
target_include_directories(Phase3CoreBugProofsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs_core
  ${REPO_ROOT}/test/txt_cover_meta/stubs
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/test/book_metadata_cache/stubs
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/ZhDict
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/TextEncoding
  ${REPO_ROOT}/lib/Txt
  ${REPO_ROOT}/lib/Utf8
)
target_link_libraries(Phase3CoreBugProofsTest PRIVATE
  crosspoint_test_common
  crosspoint_arduinojson
  GTest::gtest_main
)
gtest_discover_tests(Phase3CoreBugProofsTest)

add_executable(Phase3OtaBugProofTest
  Phase3OtaBugProofTest.cpp
  ${REPO_ROOT}/src/network/OtaUpdater.cpp
  ${REPO_ROOT}/lib/JsonParser/ReleaseJsonParser.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
)
target_include_directories(Phase3OtaBugProofTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs_ota
  ${REPO_ROOT}/test/txt_cover_meta/stubs
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/network
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
  ${REPO_ROOT}/lib/Memory
)
target_compile_definitions(Phase3OtaBugProofTest PRIVATE
  CROSSPOINT_VERSION="2.0.0-slim"
)
target_link_libraries(Phase3OtaBugProofTest PRIVATE
  crosspoint_test_common
  crosspoint_arduinojson
  GTest::gtest_main
)
gtest_discover_tests(Phase3OtaBugProofTest)

add_executable(Phase3NetBugProofsTest
  Phase3NetBugProofsTest.cpp
  ${REPO_ROOT}/test/adept_drm/stubs/HostSyncFakes.cpp
  ${REPO_ROOT}/src/TelegramCredentialStore.cpp
  ${REPO_ROOT}/src/network/TelegramBookSync.cpp
  ${REPO_ROOT}/src/network/LibbyClient.cpp
  ${REPO_ROOT}/src/network/AdeptFulfiller.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Txt/TxtCoverMeta.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(Phase3NetBugProofsTest PRIVATE
  ${REPO_ROOT}/test/adept_drm/stubs
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/SdCardFont
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Txt
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/include
)
target_link_libraries(Phase3NetBugProofsTest PRIVATE
  crosspoint_test_common
  crosspoint_arduinojson
  EXPAT::EXPAT
  GTest::gtest_main
)
gtest_discover_tests(Phase3NetBugProofsTest)

add_executable(Phase3UiBugProofsTest
  Phase3UiBugProofsTest.cpp
  ${REPO_ROOT}/src/MappedInputManager.cpp
)
target_include_directories(Phase3UiBugProofsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs_ui
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs_core
  ${REPO_ROOT}/test/txt_cover_meta/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/BlePageTurner/include
  ${REPO_ROOT}/freeink-sdk/libs/network/BleKeyboardHost/include
)
target_link_libraries(Phase3UiBugProofsTest PRIVATE
  crosspoint_test_common
  crosspoint_arduinojson
  GTest::gtest_main
)
gtest_discover_tests(Phase3UiBugProofsTest)

add_executable(Phase3WrappedTextTest
  Phase3WrappedTextTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/TtfEpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(Phase3WrappedTextTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs_gfx
  ${REPO_ROOT}/test/sd_card_font/stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)
target_link_libraries(Phase3WrappedTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(Phase3WrappedTextTest)
