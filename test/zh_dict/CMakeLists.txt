# The fixture dictionary is built from CC-CEDICT (CC BY-SA 4.0) by
# scripts/zh/build_zh_dict.py --keys-from fixture/story.txt; the golden
# segmentation comes from scripts/zh/zhdict_ref.py. See fixture/README.md.
add_executable(ZhDictTest
  ZhDictTest.cpp
  ${REPO_ROOT}/lib/ZhDict/ZhDict.cpp
)

target_include_directories(ZhDictTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/ZhDict
  ${REPO_ROOT}/lib/Memory
)

target_compile_definitions(ZhDictTest PRIVATE ZH_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixture")

target_link_libraries(ZhDictTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ZhDictTest)
