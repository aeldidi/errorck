add_executable(errorck_test_runner test_runner.cpp)
add_dependencies(errorck_test_runner errorck)

find_package(Threads REQUIRED)
target_link_libraries(errorck_test_runner PRIVATE sqlite3 Threads::Threads)

file(GLOB test_dirs RELATIVE ${CMAKE_CURRENT_LIST_DIR} CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_LIST_DIR}/pass_*
     ${CMAKE_CURRENT_LIST_DIR}/fail_*)
list(SORT test_dirs)

foreach(test_dir IN LISTS test_dirs)
  add_test(
      NAME ${test_dir}
      COMMAND errorck_test_runner
          --build-dir ${CMAKE_BINARY_DIR}
          --test-dir ${CMAKE_CURRENT_LIST_DIR}/${test_dir})
endforeach()
