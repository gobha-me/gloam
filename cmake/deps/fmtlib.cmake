find_package(fmt QUIET)

if (fmt_FOUND)
else ()
    if (NOT FMT_URI)
        set(FMT_URI https://github.com/fmtlib/fmt) 
    endif()

    if (NOT FMT_TAG)
        set(FMT_TAG 11.1.4)
    endif()

    # Don't let a fetched dependency install itself into our prefix. fmt's
    # FMT_INSTALL defaults ON even as a subproject, so without this line
    # `cmake --install` on this project also deposits libfmt.a, include/fmt/ and
    # fmt's whole cmake package into the user's prefix — a vendored copy that
    # shadows theirs. Catch2 and argparse gate their own install rules on being
    # the top-level project and need no equivalent.
    #
    # OFF unconditionally is right *because fmt is linked into src/bin*, an
    # executable, which is never exported. It is not the general answer: a
    # dependency linked into the library needs the opposite, tracking this
    # project's own install option, or install(EXPORT) cannot resolve it. Both
    # cases, and why visibility is not the deciding factor, are written up in
    # cmake/deps/catch2.cmake (the annotated recipe).
    #
    # Relies on CMP0077 being NEW under *fmt's* policy stack — a dependency's
    # own cmake_minimum_required() resets it, so this project's 3.28 floor is
    # not what decides it. fmt sets a high enough floor; a dependency that did
    # not would ignore this line entirely.
    set(FMT_INSTALL OFF)

    include(FetchContent)
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY ${FMT_URI}
        GIT_TAG ${FMT_TAG}
    )

    FetchContent_MakeAvailable(fmt)
endif()

