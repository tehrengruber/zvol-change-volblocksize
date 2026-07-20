add_executable(zvol_tests
  testing.cpp
  helpers.cpp
  test_content.cpp
  test_volsize.cpp
  test_holes.cpp
  test_properties.cpp
  test_head.cpp)
target_link_libraries(zvol_tests PRIVATE zcvb_common)

# Each test is its own process (creates a throwaway pool on a loop file); it
# returns 77 to signal SKIP when not run as root / without ZFS.  RUN_SERIAL keeps
# concurrent pools from contending.
set(cvb_tests
  content_larger content_smaller single_snapshot no_swap
  verify_all verify_detects_mismatch
  holes volsize_increases volsize_decreases properties
  head_is_newest_snapshot rejects_divergent_head)
foreach(t IN LISTS cvb_tests)
  add_test(NAME ${t}
           COMMAND zvol_tests ${t} $<TARGET_FILE:zvol-change-volblocksize>)
  set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
endforeach()
