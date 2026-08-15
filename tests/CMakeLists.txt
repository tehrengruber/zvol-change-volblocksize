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
  test_holds.cpp
  test_align.cpp
  test_lthash.cpp
  test_readonly.cpp)
target_link_libraries(zvol_tests PRIVATE zcvb_common)

# Each test is its own process (creates a throwaway pool on a loop file); it
# returns 77 to signal SKIP when not run as root / without ZFS.  RUN_SERIAL keeps
# concurrent pools from contending.
set(cvb_tests
  content_larger content_smaller single_snapshot no_swap
  verify_all verify_one verify_detects_mismatch
  holes volsize_increases volsize_decreases volsize_shrink_then_grow
  volsize_unaligned properties
  head_is_newest_snapshot rejects_divergent_head
  snapdev_hidden_restored reservation_preserved refuses_silent_decrypt
  resume_from verify_only holds_transferred
  align_guids_deterministic align_guids_no_fingerprint_file
  align_guids_to_fingerprint align_guids_volsize_change
  fingerprint_standalone
  align_guids_no_collision align_guids_volsize_shrink align_guids_holes
  align_resume_works align_guids_across_replica align_guids_to_resume
  align_guids_to_allow_missing
  align_resume_volsize_change align_resume_deleted_intermediate
  align_fingerprint_matches_full_read align_fingerprint_shrink_matches_full_read
  align_fingerprint_free_regrow_matches_full_read
  fingerprint_verify_larger_cellsize_partial_free
  fingerprint_only_matches_full_read align_resume_overwrite_matches_full_read
  fingerprint_only_resume
  fingerprint_malformed
  lthash_add_remove_is_identity lthash_order_independent lthash_position_matters
  lthash_zero_cells_are_free lthash_incremental_matches_scratch
  lthash_content_sensitive lthash_sub_seed_inverse lthash_sub_seed_matches_remove
  lthash_hole_seed_is_zero lthash_serialize_roundtrip
  resume_refuses_written_dest dest_lock_blocks_concurrent)
foreach(t IN LISTS cvb_tests)
  add_test(NAME ${t}
           COMMAND zvol_tests ${t} $<TARGET_FILE:zvol-change-volblocksize>)
  set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
endforeach()
