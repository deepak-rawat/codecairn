#include "LanguageRegistry.h"
#include <QFileInfo>

extern "C" {
const TSLanguage *tree_sitter_c();
const TSLanguage *tree_sitter_cpp();
const TSLanguage *tree_sitter_python();
const TSLanguage *tree_sitter_bash();
const TSLanguage *tree_sitter_javascript();
const TSLanguage *tree_sitter_typescript();
const TSLanguage *tree_sitter_tsx();
const TSLanguage *tree_sitter_go();
const TSLanguage *tree_sitter_rust();
const TSLanguage *tree_sitter_json();
const TSLanguage *tree_sitter_yaml();
const TSLanguage *tree_sitter_cmake();
}

const Language &languageById(LanguageId id) {
    static const Language languages[] = {
        {LanguageId::Plain, "Plain text", "plaintext", nullptr, {}},
        {LanguageId::C, "C", "c", tree_sitter_c(), {"clangd"}},
        {LanguageId::Cpp, "C++", "cpp", tree_sitter_cpp(), {"clangd"}},
        {LanguageId::Python,
         "Python",
         "python",
         tree_sitter_python(),
         {"basedpyright-langserver", "--stdio"}},
        {LanguageId::Bash,
         "Bash",
         "shellscript",
         tree_sitter_bash(),
         {"bash-language-server", "start"}},
        {LanguageId::JavaScript,
         "JavaScript",
         "javascript",
         tree_sitter_javascript(),
         {"typescript-language-server", "--stdio"}},
        {LanguageId::TypeScript,
         "TypeScript",
         "typescript",
         tree_sitter_typescript(),
         {"typescript-language-server", "--stdio"}},
        {LanguageId::Tsx,
         "TSX",
         "typescriptreact",
         tree_sitter_tsx(),
         {"typescript-language-server", "--stdio"}},
        {LanguageId::Go, "Go", "go", tree_sitter_go(), {"gopls"}},
        {LanguageId::Rust, "Rust", "rust", tree_sitter_rust(), {"rust-analyzer"}},
        {LanguageId::Json, "JSON", "json", tree_sitter_json(), {}},
        {LanguageId::Yaml, "YAML", "yaml", tree_sitter_yaml(), {}},
        {LanguageId::CMake, "CMake", "cmake", tree_sitter_cmake(), {}}};
    return languages[static_cast<int>(id)];
}

bool isCFamily(LanguageId id) {
    return id == LanguageId::C || id == LanguageId::Cpp;
}

const Language &languageForFile(const QString &path, const QByteArray &source) {
    const QFileInfo file(path);
    const auto suffix = file.suffix();
    if (suffix == "C")
        return languageById(LanguageId::Cpp);
    const auto ext = suffix.toLower();
    if (ext == "c")
        return languageById(LanguageId::C);
    if (ext == "h") {
        // Ambiguous .h files are scored with both C-family grammars by SourceAnalysis.
        return languageById(LanguageId::C);
    }
    if (QStringList{"cpp", "cc", "cxx", "hpp", "hxx", "hh", "ipp", "inl"}.contains(ext))
        return languageById(LanguageId::Cpp);
    if (ext == "py" || ext == "pyi")
        return languageById(LanguageId::Python);
    if (QStringList{"sh", "bash"}.contains(ext) || file.fileName() == ".bashrc" ||
        file.fileName() == ".bash_profile")
        return languageById(LanguageId::Bash);
    if (QStringList{"js", "jsx", "mjs", "cjs"}.contains(ext))
        return languageById(LanguageId::JavaScript);
    if (ext == "ts")
        return languageById(LanguageId::TypeScript);
    if (ext == "tsx")
        return languageById(LanguageId::Tsx);
    if (ext == "go")
        return languageById(LanguageId::Go);
    if (ext == "rs")
        return languageById(LanguageId::Rust);
    if (ext == "json")
        return languageById(LanguageId::Json);
    if (ext == "yml" || ext == "yaml")
        return languageById(LanguageId::Yaml);
    if (ext == "cmake" || file.fileName().compare("CMakeLists.txt", Qt::CaseInsensitive) == 0)
        return languageById(LanguageId::CMake);
    if (source.startsWith("#!")) {
        const auto line =
            source.left(source.indexOf('\n') >= 0 ? source.indexOf('\n') : source.size());
        if (line.contains("python"))
            return languageById(LanguageId::Python);
        if (line.contains("bash") || line.contains("/sh") || line.contains(" sh"))
            return languageById(LanguageId::Bash);
        if (line.contains("node"))
            return languageById(LanguageId::JavaScript);
    }
    return languageById(LanguageId::Plain);
}
