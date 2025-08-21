macro(gs_add_test test)
    add_executable(${test} ${test}.cc)
    target_include_directories(${test} PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/libqbox/include
        ${PROJECT_SOURCE_DIR}/systemc-components/common/include
        ${PROJECT_SOURCE_DIR}/systemc-components/common/include/tests
    )
    target_link_libraries(${test} PRIVATE gtest gmock router pass ${TARGET_LIBS})
    add_test(NAME ${test} COMMAND ${test})
    set_tests_properties(${test} PROPERTIES TIMEOUT 10)
endmacro()
gs_add_test(router-tests)
gs_add_test(router-tests-extended)
gs_add_test(router-addressmap-tests)
gs_add_test(router-cache-bench)
gs_add_test(router-advanced-overlap-tests)
gs_add_test(router-shadowing-warning-test)

# For router-tests-new, enable death tests (ASSERT_EXIT)
# This check is crucial because ASSERT_EXIT behaves differently based on GTEST_HAS_DEATH_TEST
if (GTEST_HAS_DEATH_TEST)
    gs_add_test(router-tests-new)
else()
    add_executable(router-tests-new router-tests-new.cc)
    target_include_directories(router-tests-new PRIVATE ${PROJECT_SOURCE_DIR}/tests/libqbox/include)
    target_link_libraries(router-tests-new PRIVATE gtest gmock router pass ${TARGET_LIBS})
    add_test(NAME router-tests-new COMMAND router-tests-new)
    set_tests_properties(router-tests-new PROPERTIES TIMEOUT 10 SKIP_TEST TRUE)
endif()
