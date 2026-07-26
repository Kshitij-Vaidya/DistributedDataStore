function(novacache_set_project_warnings target)
    if(MSVC)
        set(warnings /W4 /permissive-)
        if(NOVACACHE_WARNINGS_AS_ERRORS)
            list(APPEND warnings /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        set(
            warnings
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Woverloaded-virtual
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
        )
        if(NOVACACHE_WARNINGS_AS_ERRORS)
            list(APPEND warnings -Werror)
        endif()
    else()
        message(WARNING "No warning set configured for ${CMAKE_CXX_COMPILER_ID}")
    endif()

    target_compile_options(${target} PRIVATE ${warnings})
endfunction()
