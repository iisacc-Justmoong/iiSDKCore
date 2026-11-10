if(NOT TARGET_TYPE STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "Shared build requested but target type is ${TARGET_TYPE}")
endif()
