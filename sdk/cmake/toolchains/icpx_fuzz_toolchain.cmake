if (UNIX)
  set(CMAKE_C_COMPILER icx)
  set(CMAKE_CXX_COMPILER icpx)
endif()

if (WIN32)
  set(CMAKE_C_COMPILER icx)
  set(CMAKE_CXX_COMPILER icx)
endif()

set(CMAKE_CXX_FLAGS_DEBUG_INIT "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls\
                                -fsanitize-ignorelist=${PROJECT_SOURCE_DIR}/fuzz/ubsan-ignore.txt")
set(CMAKE_C_FLAGS_DEBUG_INIT "-fsanitize=address,undefined\
                                -fsanitize-ignorelist=${PROJECT_SOURCE_DIR}/fuzz/ubsan-ignore.txt")
