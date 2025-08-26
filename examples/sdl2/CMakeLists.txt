add_executable(sdl2_example main.c)

find_package(SDL2 REQUIRED)

target_link_libraries(sdl2_example PRIVATE ff_player SDL2::SDL2)