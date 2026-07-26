function(novacache_enable_sanitizers target)
    if(NOVACACHE_ENABLE_TSAN AND (NOVACACHE_ENABLE_ASAN OR NOVACACHE_ENABLE_UBSAN))
        message(FATAL_ERROR "ThreadSanitizer cannot be combined with ASan or UBSan")
    endif()

    if(NOT NOVACACHE_ENABLE_ASAN
       AND NOT NOVACACHE_ENABLE_UBSAN
       AND NOT NOVACACHE_ENABLE_TSAN)
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "NovaCache sanitizer presets require Clang or GCC")
    endif()

    set(sanitizers)
    if(NOVACACHE_ENABLE_ASAN)
        list(APPEND sanitizers address)
    endif()
    if(NOVACACHE_ENABLE_UBSAN)
        list(APPEND sanitizers undefined)
    endif()
    if(NOVACACHE_ENABLE_TSAN)
        list(APPEND sanitizers thread)
    endif()

    list(JOIN sanitizers "," sanitizer_list)
    target_compile_options(
        ${target}
        PRIVATE
            "-fsanitize=${sanitizer_list}"
            -fno-omit-frame-pointer
    )
    target_link_options(
        ${target}
        PRIVATE
            "-fsanitize=${sanitizer_list}"
            -fno-omit-frame-pointer
    )
endfunction()
