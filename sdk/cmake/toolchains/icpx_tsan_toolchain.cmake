if (UNIX)
  set(CMAKE_C_COMPILER icx)
  set(CMAKE_CXX_COMPILER icpx)
endif()

if (WIN32)
  set(CMAKE_C_COMPILER icx)
  set(CMAKE_CXX_COMPILER icx)
endif()

set(CMAKE_CXX_FLAGS_DEBUG_INIT "-fsanitize=thread -fno-omit-frame-pointer -fsanitize-recover=all")
set(CMAKE_C_FLAGS_DEBUG_INIT "-fsanitize=thread -fno-omit-frame-pointer -fsanitize-recover=all")
