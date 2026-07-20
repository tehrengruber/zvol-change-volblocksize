add_executable(zvol_tests
  testing.cpp
  helpers.cpp
  test_content.cpp
  test_volsize.cpp
  test_holes.cpp
  test_properties.cpp
  test_head.cpp
  test_snapdev.cpp
  test_reservation.cpp
  test_encryption.cpp
  test_resume.cpp
  test_verify_only.cpp
  test_holds.cpp)
target_link_libraries(zvol_tests PRIVATE zcvb_common)

# Each test is its own process (creates a throwaway pool on a loop file); it
# returns 77 to signal SKIP when not run as root / without ZFS.  RUN_SERIAL keeps
# concurrent pools from contending.
set(cvb_tests
  content_larger content_smaller single_snapshot no_swap
  verify_all verify_detects_mismatch
  holes volsize_increases volsize_decreases volsize_shrink_then_grow
  volsize_unaligned properties
  head_is_newest_snapshot rejects_divergent_head
  snapdev_hidden_restored reservation_preserved refuses_silent_decrypt
  resume_from verify_only holds_transferred)
foreach(t IN LISTS cvb_tests)
  add_test(NAME ${t}
           COMMAND zvol_tests ${t} $<TARGET_FILE:zvol-change-volblocksize>)
  set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
endforeach()
