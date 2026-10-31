function(codecairn_grammar name)
    set(root "${tree_sitter_${name}_SOURCE_DIR}")
    file(GLOB scanner CONFIGURE_DEPENDS "${root}/src/scanner.c" "${root}/src/scanner.cc")
    add_library(${name}_grammar STATIC "${root}/src/parser.c" ${scanner})
    target_include_directories(${name}_grammar PRIVATE "${root}/src")
    set_target_properties(${name}_grammar PROPERTIES AUTOMOC OFF AUTORCC OFF)
    install(
        FILES "${root}/LICENSE"
        DESTINATION "${CODECAIRN_DATA_DIRECTORY}/licenses"
        RENAME tree-sitter-${name}.txt
    )
endfunction()

foreach(name c cpp python bash javascript go rust json yaml cmake)
    codecairn_grammar(${name})
endforeach()

foreach(variant typescript tsx)
    add_library(
        ${variant}_grammar STATIC "${tree_sitter_typescript_SOURCE_DIR}/${variant}/src/parser.c"
                                  "${tree_sitter_typescript_SOURCE_DIR}/${variant}/src/scanner.c"
    )
    target_include_directories(
        ${variant}_grammar PRIVATE "${tree_sitter_typescript_SOURCE_DIR}/${variant}/src"
    )
    set_target_properties(${variant}_grammar PROPERTIES AUTOMOC OFF AUTORCC OFF)
endforeach()
install(
    FILES "${tree_sitter_typescript_SOURCE_DIR}/LICENSE"
    DESTINATION "${CODECAIRN_DATA_DIRECTORY}/licenses"
    RENAME tree-sitter-typescript.txt
)
