add_neug_test(runtime_wait_test test_runtime_wait.cc)
add_neug_test(operation_gate_test test_operation_gate.cc)
add_neug_test(read_view_publication_test test_read_view_publication.cc)

if (BUILD_HTTP_SERVER)
	add_neug_test(
		transaction_test
		test_mvcc_insert_transaction.cc
		test_snapshot_cow_write_transaction.cc
		test_in_place_compaction_transaction.cc
		test_transaction_release_order.cc
		test_wal_replay.cc
		test_acid.cc)
endif()

	# ---------------------------------------------------------------------
	# concurrent_compaction_test: row-group compaction concurrency
	# regression. Drives concurrent MvccInsertTransaction commits against a
	# compacted group's retreated per-group frontier (GroupFrontier::
	# AllocateNext) and asserts vid uniqueness / PK integrity. For the
	# race-detecting run:
	#   cmake -DENABLE_THREAD_SANITIZER=ON -DWITH_MIMALLOC=OFF -DBUILD_TEST=ON ..
	#   TSAN_OPTIONS=suppressions=<repo>/tests/tsan_suppressions.txt \
	#     tests/transaction/concurrent_compaction_test --gtest_repeat=5
	# ---------------------------------------------------------------------
	add_neug_test(concurrent_compaction_test test_concurrent_compaction.cc)
endif()
