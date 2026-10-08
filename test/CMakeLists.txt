# Host unit tests for lib/: cmake -S test -B test/build && cmake --build test/build && ctest --test-dir test/build
cmake_minimum_required(VERSION 3.16)
project(airmidi_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wpedantic -Werror -g)

option(SANITIZE "AddressSanitizer + UndefinedBehaviorSanitizer" ON)
if(SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(../lib lib)

add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)
target_compile_options(unity PRIVATE -Wno-pedantic)

enable_testing()
foreach(name ump midi1_in midi1_out translate)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} airmidi_core unity)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
