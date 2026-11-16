add_executable(BleShareTest
  BleShareTest.cpp
  ${REPO_ROOT}/lib/BleShare/src/BleShareProtocol.cpp
  ${REPO_ROOT}/lib/BleShare/src/BleShareSession.cpp
)

target_include_directories(BleShareTest PRIVATE
  ${REPO_ROOT}/lib/BleShare/include
  ${REPO_ROOT}/src
)

target_link_libraries(BleShareTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BleShareTest)
