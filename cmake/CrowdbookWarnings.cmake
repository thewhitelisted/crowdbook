# crowdbook_set_warnings(<target>)
#
# Applies the project's warning flags to one of our own targets. Third-party code is fetched with
# SYSTEM include directories, so its headers don't trip these warnings.
function(crowdbook_set_warnings target)
    if(MSVC)
        set(warnings /W4 /permissive-)
        if(CROWDBOOK_WARNINGS_AS_ERRORS)
            list(APPEND warnings /WX)
        endif()
    else()
        set(warnings
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Woverloaded-virtual
            -Wdouble-promotion
            -Wimplicit-fallthrough
            -Wformat=2)
        if(CROWDBOOK_WARNINGS_AS_ERRORS)
            list(APPEND warnings -Werror)
        endif()
    endif()
    target_compile_options(${target} PRIVATE ${warnings})
endfunction()
