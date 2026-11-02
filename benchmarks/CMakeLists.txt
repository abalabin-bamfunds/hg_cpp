add_executable(hg_container_benchmark container_benchmark.cpp)
target_link_libraries(hg_container_benchmark PRIVATE hg::hg)

add_executable(hg_runtime_scheduling_benchmark runtime_scheduling_benchmark.cpp)
target_link_libraries(hg_runtime_scheduling_benchmark PRIVATE hg::hg)

add_executable(hg_queue_realtime_benchmark queue_realtime_benchmark.cpp)
target_link_libraries(hg_queue_realtime_benchmark PRIVATE hg::hg Threads::Threads)

add_executable(hg_evaluation_context_benchmark evaluation_context_benchmark.cpp)
target_link_libraries(hg_evaluation_context_benchmark PRIVATE hg::hg)

add_executable(hg_view_benchmark view_benchmark.cpp)
target_link_libraries(hg_view_benchmark PRIVATE hg::hg)

add_executable(hg_scalar_dict_benchmark scalar_dict_benchmark.cpp)
target_link_libraries(hg_scalar_dict_benchmark PRIVATE hg::hg)

add_executable(hg_tsd_selected_reference_benchmark tsd_selected_reference_benchmark.cpp)
target_link_libraries(hg_tsd_selected_reference_benchmark PRIVATE hg::hg)

add_executable(hg_tsd_membership_benchmark tsd_membership_benchmark.cpp)
target_link_libraries(hg_tsd_membership_benchmark PRIVATE hg::library)

add_executable(hg_high_count_map_benchmark high_count_map_benchmark.cpp)
target_link_libraries(hg_high_count_map_benchmark PRIVATE hg::hg)

add_executable(hg_reduce_benchmark reduce_benchmark.cpp)
target_link_libraries(hg_reduce_benchmark PRIVATE hg::hg)

add_executable(hg_string_regex_benchmark string_regex_benchmark.cpp)
target_link_libraries(hg_string_regex_benchmark PRIVATE hg::library)

if(WIN32)
    target_link_libraries(hg_high_count_map_benchmark PRIVATE psapi)
    target_link_libraries(hg_reduce_benchmark PRIVATE psapi)

    get_property(benchmark_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(benchmark_target IN LISTS benchmark_targets)
        add_custom_command(
            TARGET ${benchmark_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${benchmark_target}>
                    $<TARGET_FILE_DIR:${benchmark_target}>
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endforeach()
endif()

if(BUILD_TESTING)
    add_test(
        NAME hg_reduce_benchmark_smoke
        COMMAND hg_reduce_benchmark --size 10000 --samples 2 --lifecycle-rounds 1
    )

    add_test(
        NAME hg_high_count_map_benchmark_workload_manifest
        COMMAND hg_high_count_map_benchmark --verify-workload
    )
    add_test(NAME hg_high_count_map_benchmark_smoke COMMAND hg_high_count_map_benchmark --profile smoke --cycles 120)
endif()
