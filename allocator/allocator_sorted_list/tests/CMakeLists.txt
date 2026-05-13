add_executable(
        sys_prog_allctr_allctr_srtd_lst_tests
        allocator_sorted_list_tests.cpp)

target_link_libraries(
        sys_prog_allctr_allctr_srtd_lst_tests
        PRIVATE
        gtest_main)
target_link_libraries(
        sys_prog_allctr_allctr_srtd_lst_tests
        PRIVATE
        sys_prog_allctr_allctr_srtd_lst)
add_test(
        NAME sys_prog_allctr_allctr_srtd_lst_tests
        COMMAND sys_prog_allctr_allctr_srtd_lst_tests)