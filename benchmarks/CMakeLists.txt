add_executable(bg_container_benchmark container_benchmark.cpp)
target_link_libraries(bg_container_benchmark PRIVATE bg::bg)

add_executable(bg_runtime_scheduling_benchmark runtime_scheduling_benchmark.cpp)
target_link_libraries(bg_runtime_scheduling_benchmark PRIVATE bg::bg)

add_executable(bg_view_benchmark view_benchmark.cpp)
target_link_libraries(bg_view_benchmark PRIVATE bg::bg)

add_executable(bg_scalar_dict_benchmark scalar_dict_benchmark.cpp)
target_link_libraries(bg_scalar_dict_benchmark PRIVATE bg::bg)

add_executable(bg_high_count_map_benchmark high_count_map_benchmark.cpp)
target_link_libraries(bg_high_count_map_benchmark PRIVATE bg::bg)

add_executable(bg_reduce_benchmark reduce_benchmark.cpp)
target_link_libraries(bg_reduce_benchmark PRIVATE bg::bg)

if(WIN32)
    target_link_libraries(bg_high_count_map_benchmark PRIVATE psapi)
    target_link_libraries(bg_reduce_benchmark PRIVATE psapi)
endif()

if(BUILD_TESTING)
    add_test(
        NAME bg_reduce_benchmark_smoke
        COMMAND bg_reduce_benchmark --size 10000 --samples 2 --lifecycle-rounds 1
    )

    add_test(
        NAME bg_high_count_map_benchmark_workload_manifest
        COMMAND bg_high_count_map_benchmark --verify-workload
    )
    add_test(NAME bg_high_count_map_benchmark_smoke COMMAND bg_high_count_map_benchmark --profile smoke --cycles 120)
endif()
