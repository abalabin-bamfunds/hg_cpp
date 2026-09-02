add_executable(bg_container_benchmark container_benchmark.cpp)
target_link_libraries(bg_container_benchmark PRIVATE bg::bg)

add_executable(bg_view_benchmark view_benchmark.cpp)
target_link_libraries(bg_view_benchmark PRIVATE bg::bg)
