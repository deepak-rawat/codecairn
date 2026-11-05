include("${CMAKE_CURRENT_LIST_DIR}/ReleaseCommon.cmake")
if(NOT DEFINED ARCHIVE)
    set(ARCHIVE "${OUTPUT}/codecairn-${VERSION}-source.tar.gz")
endif()
if(NOT DEFINED LOCAL_ROOT)
    if("$ENV{HOME}" STREQUAL "")
        message(FATAL_ERROR "Specify -DLOCAL_ROOT=<absolute-directory> when HOME is unavailable.")
    endif()
    set(LOCAL_ROOT "$ENV{HOME}/local-releases")
endif()
get_filename_component(LOCAL_ROOT "${LOCAL_ROOT}" ABSOLUTE)
set(tap "${LOCAL_ROOT}/tap")
set(artifacts "${LOCAL_ROOT}/artifacts/codecairn")
find_program(GIT git REQUIRED)
set(changes "")

if(EXISTS "${tap}/.git")
    execute_process(
        COMMAND "${GIT}" -C "${tap}" rev-parse --show-toplevel
        OUTPUT_VARIABLE repository
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
    )
    file(REAL_PATH "${tap}" tap_path)
    file(REAL_PATH "${repository}" repository_path)
    if(NOT
       tap_path
       STREQUAL
       repository_path
    )
        message(FATAL_ERROR "Local tap must be its own Git repository: ${tap}")
    endif()
    execute_process(
        COMMAND "${GIT}" -C "${tap}" status --porcelain -- Formula/codecairn.rb
        OUTPUT_VARIABLE changes COMMAND_ERROR_IS_FATAL ANY
    )
endif()

string(
    RANDOM
    LENGTH 12
    random
)
set(work "${OUTPUT}/local-tap-work-${random}")
set(url "file://${artifacts}")
foreach(
    character
    "%"
    " "
    "#"
    "?"
    "\""
)
    if(character STREQUAL "%")
        set(encoded "%25")
    elseif(character STREQUAL " ")
        set(encoded "%20")
    elseif(character STREQUAL "#")
        set(encoded "%23")
    elseif(character STREQUAL "?")
        set(encoded "%3F")
    else()
        set(encoded "%22")
    endif()
    string(
        REPLACE "${character}"
                "${encoded}"
                url
                "${url}"
    )
endforeach()
set(arguments "-DARCHIVE=${ARCHIVE}" "-DOUTPUT=${work}" "-DRELEASE_URL=${url}")
run("${CMAKE_COMMAND}"
    ${arguments}
    -P
    "${CMAKE_CURRENT_LIST_DIR}/PrepareRecipes.cmake"
)
if(EXISTS "${tap}/Formula/codecairn.rb"
   AND (NOT EXISTS "${tap}/.git"
        OR NOT
           changes
           STREQUAL
           ""
       )
)
    file(SHA256 "${tap}/Formula/codecairn.rb" current)
    file(SHA256 "${work}/homebrew/Formula/codecairn.rb" generated)
    if(NOT
       current
       STREQUAL
       generated
    )
        message(FATAL_ERROR "Save or commit local edits to ${tap}/Formula/codecairn.rb first.")
    endif()
endif()

file(MAKE_DIRECTORY "${artifacts}" "${tap}/Formula")
copy_immutable("${ARCHIVE}" "${artifacts}/codecairn-${VERSION}-source.tar.gz")
checksum("${artifacts}/codecairn-${VERSION}-source.tar.gz")
configure_file("${work}/homebrew/Formula/codecairn.rb" "${tap}/Formula/codecairn.rb" COPYONLY)

if(COMMIT_TAP)
    if(NOT EXISTS "${tap}/.git")
        run("${GIT}" init "${tap}")
    endif()
    execute_process(
        COMMAND "${GIT}" -C "${tap}" status --porcelain -- Formula/codecairn.rb
        OUTPUT_VARIABLE changes COMMAND_ERROR_IS_FATAL ANY
    )
    if(NOT
       changes
       STREQUAL
       ""
    )
        run("${GIT}"
            -C
            "${tap}"
            add
            --
            Formula/codecairn.rb
        )
        run("${GIT}"
            -C
            "${tap}"
            -c
            "user.name=CodeCairn Release"
            -c
            "user.email=codecairn@localhost"
            commit
            --only
            -m
            "Update CodeCairn ${VERSION}"
            -m
            "Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
            --
            Formula/codecairn.rb
        )
    endif()
else()
    message(
        STATUS
            "Review ${tap}/Formula/codecairn.rb; use -DCOMMIT_TAP=ON to initialize/update its separate Git repository."
    )
endif()
message(STATUS "Generic local tap: ${tap}")
message(STATUS "Register once (not run): brew tap local/releases \"${tap}\"")
message(STATUS "Install (not run): brew install local/releases/codecairn")
file(REMOVE_RECURSE "${work}")
