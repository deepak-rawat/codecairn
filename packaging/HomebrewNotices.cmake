function(
    collect_source_notices
    package
    prefix
    destination
    cache
)
    execute_process(
        COMMAND "${BREW}" info --json=v2 "${package}" OUTPUT_VARIABLE metadata
                                                                      COMMAND_ERROR_IS_FATAL ANY
    )
    string(
        JSON
        version
        GET
        "${metadata}"
        formulae
        0
        versions
        stable
    )
    string(
        JSON
        url
        GET
        "${metadata}"
        formulae
        0
        urls
        stable
        url
    )
    string(
        JSON
        hash
        GET
        "${metadata}"
        formulae
        0
        urls
        stable
        checksum
    )
    file(REAL_PATH "${prefix}" installed)
    get_filename_component(installed_version "${installed}" NAME)
    string(
        REGEX
        REPLACE "_[0-9]+$"
                ""
                installed_version
                "${installed_version}"
    )
    if(NOT
       installed_version
       STREQUAL
       version
    )
        message(
            FATAL_ERROR
                "Cannot collect ${package} notices: installed ${installed_version}, source ${version}"
        )
    endif()
    set(archive "${cache}/${package}-${hash}.archive")
    file(MAKE_DIRECTORY "${cache}")
    if(NOT EXISTS "${archive}")
        file(
            DOWNLOAD "${url}" "${archive}"
            EXPECTED_HASH "SHA256=${hash}"
            TLS_VERIFY ON
        )
    endif()
    file(SHA256 "${archive}" actual)
    if(NOT
       actual
       STREQUAL
       hash
    )
        message(FATAL_ERROR "Source checksum mismatch for ${package}")
    endif()
    set(source "${cache}/${package}-${hash}")
    file(
        ARCHIVE_EXTRACT
        INPUT
        "${archive}"
        DESTINATION
        "${source}"
    )
    file(
        GLOB_RECURSE files
        LIST_DIRECTORIES FALSE
        "${source}/*"
    )
    set(found FALSE)
    foreach(file IN LISTS files)
        get_filename_component(name "${file}" NAME)
        string(TOUPPER "${name}" upper)
        if(upper MATCHES "^(LICENSE|LICENCE|COPYING|NOTICE|COPYRIGHT)" OR file MATCHES "/LICENSES/")
            file(
                RELATIVE_PATH
                relative
                "${source}"
                "${file}"
            )
            get_filename_component(directory "${relative}" DIRECTORY)
            file(MAKE_DIRECTORY "${destination}/${directory}")
            file(COPY "${file}" DESTINATION "${destination}/${directory}")
            set(found TRUE)
        endif()
    endforeach()
    if(NOT found AND package STREQUAL "sqlite")
        file(GLOB_RECURSE headers "${source}/sqlite3.h")
        list(LENGTH headers count)
        if(count EQUAL 1)
            list(
                GET
                headers
                0
                header
            )
            file(STRINGS "${header}" notice LIMIT_COUNT 20)
            string(
                REPLACE ";"
                        "\n"
                        notice
                        "${notice}"
            )
            if(notice MATCHES "disclaims copyright")
                file(MAKE_DIRECTORY "${destination}")
                file(WRITE "${destination}/PUBLIC-DOMAIN.txt" "${notice}\n")
                set(found TRUE)
            endif()
        endif()
    endif()
    if(NOT found)
        message(FATAL_ERROR "No dependency notices found in verified ${package} source")
    endif()
    file(WRITE "${destination}/SOURCE.txt"
         "package=${package}\nversion=${version}\nurl=${url}\nsha256=${hash}\n"
    )
endfunction()
