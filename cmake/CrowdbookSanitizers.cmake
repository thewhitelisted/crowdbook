# CROWDBOOK_SANITIZE instruments every target defined after this file is included, third-party
# code such as GoogleTest included. Mixing instrumented and uninstrumented code can produce false
# reports (e.g. AddressSanitizer container-overflow checks), so the flags are applied globally
# instead of per target.
if(CROWDBOOK_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "CROWDBOOK_SANITIZE is only supported with GCC and Clang")
    endif()
    add_compile_options(
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
