cmake_minimum_required(VERSION 3.20)
project(StazeCpp23Compiler VERSION 0.6.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(stazec
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/semantic.cpp
    src/sir.cpp
    src/llvm_x64.cpp
    src/toolchain.cpp
)
target_include_directories(stazec PRIVATE include)
if (MSVC)
    target_compile_options(stazec PRIVATE /W4 /permissive-)
else()
    target_compile_options(stazec PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME check_core COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/arithmetic_control.stz3 --check)
add_test(NAME check_hello COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/hello.stz3 --check)
add_test(NAME reject_immutable_set COMMAND stazec ${CMAKE_SOURCE_DIR}/tests/negative_immutable_set.stz3 --check)
set_tests_properties(reject_immutable_set PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_unhandled_overflow COMMAND stazec ${CMAKE_SOURCE_DIR}/tests/negative_unhandled_overflow.stz3 --check)
set_tests_properties(reject_unhandled_overflow PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_delete_one COMMAND stazec ${CMAKE_SOURCE_DIR}/tests/negative_delete_exact_one.stz3 --check)
set_tests_properties(reject_delete_one PROPERTIES WILL_FAIL TRUE)
find_program(STAZE_TEST_CLANG NAMES clang clang-17)
find_program(STAZE_TEST_LLD_LINK NAMES lld-link lld-link-17)
if (STAZE_TEST_CLANG AND STAZE_TEST_LLD_LINK)
    add_test(NAME build_pe COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/arithmetic_control.stz3 -o ${CMAKE_BINARY_DIR}/arithmetic_control.exe)
    set_tests_properties(build_pe PROPERTIES ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
endif()


# SIR-C serialization is a real compiler boundary. This fixture first emits a
# standalone .sirc file, then a second compiler invocation parses/verifies it
# without touching Staze source/AST/SSL.
add_test(NAME emit_sirc_fixture COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/arithmetic_control.stz3 --check --emit-sir-c ${CMAKE_BINARY_DIR}/arithmetic_control.sirc)
set_tests_properties(emit_sirc_fixture PROPERTIES FIXTURES_SETUP detached_sirc)
add_test(NAME check_detached_sirc COMMAND stazec --from-sir-c ${CMAKE_BINARY_DIR}/arithmetic_control.sirc --check)
set_tests_properties(check_detached_sirc PROPERTIES FIXTURES_REQUIRED detached_sirc)
if (STAZE_TEST_CLANG AND STAZE_TEST_LLD_LINK)
    add_test(NAME build_pe_from_detached_sirc COMMAND stazec --from-sir-c ${CMAKE_BINARY_DIR}/arithmetic_control.sirc -o ${CMAKE_BINARY_DIR}/arithmetic_control_from_sirc.exe)
    set_tests_properties(build_pe_from_detached_sirc PROPERTIES FIXTURES_REQUIRED detached_sirc ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
endif()
add_test(NAME reject_tampered_sirc COMMAND stazec --from-sir-c ${CMAKE_SOURCE_DIR}/tests/invalid_sirc_bad_fault_edge.sirc --check)
set_tests_properties(reject_tampered_sirc PROPERTIES WILL_FAIL TRUE)


# Compiler 0.4 Rich Semantic SSA fixtures retained under the 0.5 representation backend.
add_test(NAME check_rich_cardinality COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/rich_cardinality.stz3 --check)
add_test(NAME check_rich_resources COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/rich_resources.stz3 --check)
add_test(NAME check_effect_regions COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/effect_regions_native.stz3 --check)
add_test(NAME reject_use_after_move COMMAND stazec ${CMAKE_SOURCE_DIR}/tests/negative_use_after_move.stz3 --check)
set_tests_properties(reject_use_after_move PROPERTIES WILL_FAIL TRUE)

add_test(NAME emit_rich_sirs_fixture COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/rich_resources.stz3 --check --emit-sir-s ${CMAKE_BINARY_DIR}/rich_resources.sirs --emit-sir-c ${CMAKE_BINARY_DIR}/rich_resources.source.sirc)
set_tests_properties(emit_rich_sirs_fixture PROPERTIES FIXTURES_SETUP rich_sirs)
add_test(NAME check_detached_rich_sirs COMMAND stazec --from-sir-s ${CMAKE_BINARY_DIR}/rich_resources.sirs --check --emit-sir-c ${CMAKE_BINARY_DIR}/rich_resources.detached.sirc)
set_tests_properties(check_detached_rich_sirs PROPERTIES FIXTURES_REQUIRED rich_sirs)
add_test(NAME check_detached_rich_sirc COMMAND stazec --from-sir-c ${CMAKE_BINARY_DIR}/rich_resources.detached.sirc --check)
set_tests_properties(check_detached_rich_sirc PROPERTIES FIXTURES_REQUIRED rich_sirs)

if (STAZE_TEST_CLANG AND STAZE_TEST_LLD_LINK)
    add_test(NAME build_effect_regions_pe COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/effect_regions_native.stz3 -o ${CMAKE_BINARY_DIR}/effect_regions_native.exe --emit-sir-c ${CMAKE_BINARY_DIR}/effect_regions_native.sirc)
    set_tests_properties(build_effect_regions_pe PROPERTIES ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
    add_test(NAME build_effect_regions_from_sirc COMMAND stazec --from-sir-c ${CMAKE_BINARY_DIR}/effect_regions_native.sirc -o ${CMAKE_BINARY_DIR}/effect_regions_native_from_sirc.exe)
    set_tests_properties(build_effect_regions_from_sirc PROPERTIES DEPENDS build_effect_regions_pe ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
endif()
add_test(NAME reject_bad_effect_token COMMAND stazec --from-sir-c ${CMAKE_SOURCE_DIR}/tests/invalid_sirc_bad_effect_token.sirc --check)
set_tests_properties(reject_bad_effect_token PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_missing_move_authority COMMAND stazec --from-sir-c ${CMAKE_SOURCE_DIR}/tests/invalid_sirc_missing_move_authority.sirc --check)
set_tests_properties(reject_missing_move_authority PROPERTIES WILL_FAIL TRUE)


# Compiler 0.5 Concrete Rich Representation Lowering fixtures.
add_test(NAME check_heap_allocation COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/heap_allocation.stz3 --check)
add_test(NAME check_fault_payload_native COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/fault_payload_native.stz3 --check)
add_test(NAME check_heap_scope_reclaim COMMAND stazec ${CMAKE_SOURCE_DIR}/tests/negative_heap_scope_reclaim.stz3 --check)
add_test(NAME reject_zero_concrete_size COMMAND stazec --from-sir-c ${CMAKE_SOURCE_DIR}/tests/invalid_sirc_zero_concrete_size.sirc --check)
set_tests_properties(reject_zero_concrete_size PROPERTIES WILL_FAIL TRUE)

add_test(NAME emit_rich_cardinality_sirc_fixture COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/rich_cardinality.stz3 --check --emit-sir-c ${CMAKE_BINARY_DIR}/rich_cardinality.sirc)
set_tests_properties(emit_rich_cardinality_sirc_fixture PROPERTIES FIXTURES_SETUP rich_cardinality_sirc)
add_test(NAME check_detached_rich_cardinality_sirc COMMAND stazec --from-sir-c ${CMAKE_BINARY_DIR}/rich_cardinality.sirc --check)
set_tests_properties(check_detached_rich_cardinality_sirc PROPERTIES FIXTURES_REQUIRED rich_cardinality_sirc)

if (STAZE_TEST_CLANG AND STAZE_TEST_LLD_LINK)
    add_test(NAME build_rich_cardinality_pe COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/rich_cardinality.stz3 -o ${CMAKE_BINARY_DIR}/rich_cardinality.exe)
    set_tests_properties(build_rich_cardinality_pe PROPERTIES ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
    add_test(NAME build_rich_cardinality_from_sirc COMMAND stazec --from-sir-c ${CMAKE_BINARY_DIR}/rich_cardinality.sirc -o ${CMAKE_BINARY_DIR}/rich_cardinality_from_sirc.exe)
    set_tests_properties(build_rich_cardinality_from_sirc PROPERTIES FIXTURES_REQUIRED rich_cardinality_sirc ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
    add_test(NAME build_rich_resources_pe COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/rich_resources.stz3 -o ${CMAKE_BINARY_DIR}/rich_resources.exe)
    set_tests_properties(build_rich_resources_pe PROPERTIES ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
    add_test(NAME build_heap_allocation_pe COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/heap_allocation.stz3 -o ${CMAKE_BINARY_DIR}/heap_allocation.exe)
    set_tests_properties(build_heap_allocation_pe PROPERTIES ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
    add_test(NAME build_fault_payload_native_pe COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/fault_payload_native.stz3 -o ${CMAKE_BINARY_DIR}/fault_payload_native.exe)
    set_tests_properties(build_fault_payload_native_pe PROPERTIES ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
endif()
add_test(NAME check_rich_caller_storage COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/rich_caller_storage.stz3 --check)
if (STAZE_TEST_CLANG AND STAZE_TEST_LLD_LINK)
    add_test(NAME build_rich_caller_storage_pe COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/rich_caller_storage.stz3 -o ${CMAKE_BINARY_DIR}/rich_caller_storage.exe)
    set_tests_properties(build_rich_caller_storage_pe PROPERTIES ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
endif()


# Compiler 0.6 Lifetime & Resource Realization proof/conformance fixtures.
add_test(NAME check_lifetime_cleanup COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/lifetime_cleanup.stz3 --check)
add_test(NAME check_loop_scope_cleanup COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/loop_scope_cleanup.stz3 --check)
add_test(NAME check_dynamic_many COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/dynamic_many.stz3 --check)
add_test(NAME check_addressable_fields COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/addressable_fields.stz3 --check)
add_test(NAME check_static_pool COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/static_pool.stz3 --check)
add_test(NAME check_transaction_cleanup COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/transaction_cleanup.stz3 --check)
add_test(NAME check_borrow_narrowing COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/borrow_narrowing.stz3 --check)
add_test(NAME check_owned_heap_transfer COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/owned_heap_transfer.stz3 --check)
add_test(NAME check_dynamic_many_transfer COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/dynamic_many_transfer.stz3 --check)

add_test(NAME reject_widened_borrow_sirs COMMAND stazec --from-sir-s ${CMAKE_SOURCE_DIR}/tests/invalid_sirs_widened_borrow.sirs --check)
set_tests_properties(reject_widened_borrow_sirs PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_borrow_return COMMAND stazec ${CMAKE_SOURCE_DIR}/tests/negative_borrow_return.stz3 --check)
set_tests_properties(reject_borrow_return PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_implicit_owned_return COMMAND stazec ${CMAKE_SOURCE_DIR}/tests/negative_implicit_owned_return.stz3 --check)
set_tests_properties(reject_implicit_owned_return PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_missing_transfer_discharge COMMAND stazec --from-sir-c ${CMAKE_SOURCE_DIR}/tests/invalid_sirc_missing_transfer_discharge.sirc --check)
set_tests_properties(reject_missing_transfer_discharge PROPERTIES WILL_FAIL TRUE)

add_test(NAME emit_owned_transfer_sirc_fixture COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/owned_heap_transfer.stz3 --check --emit-sir-c ${CMAKE_BINARY_DIR}/owned_heap_transfer.sirc)
set_tests_properties(emit_owned_transfer_sirc_fixture PROPERTIES FIXTURES_SETUP owned_transfer_sirc)
add_test(NAME check_detached_owned_transfer_sirc COMMAND stazec --from-sir-c ${CMAKE_BINARY_DIR}/owned_heap_transfer.sirc --check)
set_tests_properties(check_detached_owned_transfer_sirc PROPERTIES FIXTURES_REQUIRED owned_transfer_sirc)

add_test(NAME emit_dynamic_many_transfer_sirc_fixture COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/dynamic_many_transfer.stz3 --check --emit-sir-c ${CMAKE_BINARY_DIR}/dynamic_many_transfer.sirc)
set_tests_properties(emit_dynamic_many_transfer_sirc_fixture PROPERTIES FIXTURES_SETUP dynamic_many_transfer_sirc)
add_test(NAME check_detached_dynamic_many_transfer_sirc COMMAND stazec --from-sir-c ${CMAKE_BINARY_DIR}/dynamic_many_transfer.sirc --check)
set_tests_properties(check_detached_dynamic_many_transfer_sirc PROPERTIES FIXTURES_REQUIRED dynamic_many_transfer_sirc)

if (STAZE_TEST_CLANG AND STAZE_TEST_LLD_LINK)
    foreach(staze06_example IN ITEMS lifetime_cleanup loop_scope_cleanup dynamic_many addressable_fields static_pool transaction_cleanup owned_heap_transfer dynamic_many_transfer)
        add_test(NAME build_${staze06_example}_pe COMMAND stazec ${CMAKE_SOURCE_DIR}/examples/${staze06_example}.stz3 -o ${CMAKE_BINARY_DIR}/${staze06_example}.exe)
        set_tests_properties(build_${staze06_example}_pe PROPERTIES ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
    endforeach()
    add_test(NAME build_owned_transfer_from_sirc COMMAND stazec --from-sir-c ${CMAKE_BINARY_DIR}/owned_heap_transfer.sirc -o ${CMAKE_BINARY_DIR}/owned_heap_transfer_from_sirc.exe)
    set_tests_properties(build_owned_transfer_from_sirc PROPERTIES FIXTURES_REQUIRED owned_transfer_sirc ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
    add_test(NAME build_dynamic_many_transfer_from_sirc COMMAND stazec --from-sir-c ${CMAKE_BINARY_DIR}/dynamic_many_transfer.sirc -o ${CMAKE_BINARY_DIR}/dynamic_many_transfer_from_sirc.exe)
    set_tests_properties(build_dynamic_many_transfer_from_sirc PROPERTIES FIXTURES_REQUIRED dynamic_many_transfer_sirc ENVIRONMENT "STAZE_CLANG=${STAZE_TEST_CLANG};STAZE_LLD_LINK=${STAZE_TEST_LLD_LINK}")
endif()
