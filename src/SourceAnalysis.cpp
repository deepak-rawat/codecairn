#include "SourceAnalysis.h"
#include "Diagnostics.h"
#include "FileIO.h"
#include "LanguageRegistry.h"
#include "CompilerSymbolRecovery.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <memory>
#include <tree_sitter/api.h>

namespace {
TSNode field(TSNode node, const char *name) {
    return ts_node_child_by_field_name(node, name, static_cast<uint32_t>(qstrlen(name)));
}

TSNode descendant(TSNode node, const QSet<QByteArray> &types) {
    QVector<TSNode> stack{node};
    while (!stack.isEmpty()) {
        const auto current = stack.takeLast();
        if (ts_node_is_null(current))
            continue;
        if (types.contains(ts_node_type(current)))
            return current;
        for (uint32_t i = ts_node_named_child_count(current); i > 0; --i)
            stack.append(ts_node_named_child(current, i - 1));
    }
    return {};
}

int errorCount(TSNode node) {
    int errors = 0;
    QVector<TSNode> stack{node};
    while (!stack.isEmpty()) {
        const auto current = stack.takeLast();
        if (ts_node_is_missing(current) || qstrcmp(ts_node_type(current), "ERROR") == 0)
            ++errors;
        for (uint32_t i = 0; i < ts_node_child_count(current); ++i)
            if (ts_node_has_error(ts_node_child(current, i)))
                stack.append(ts_node_child(current, i));
    }
    return errors;
}

TSNode nameNode(TSNode node) {
    for (int depth = 0; depth < 64 && !ts_node_is_null(node); ++depth) {
        const QByteArray type(ts_node_type(node));
        if (type == "identifier" || type == "field_identifier" || type == "type_identifier" ||
            type == "namespace_identifier" || type == "operator_name" ||
            type == "destructor_name") {
            return node;
        }
        TSNode next = field(node, "declarator");
        if (ts_node_is_null(next))
            next = field(node, "name");
        if (ts_node_is_null(next) && ts_node_named_child_count(node) == 1)
            next = ts_node_named_child(node, 0);
        node = next;
    }
    return {};
}

bool isFunction(TSNode node) {
    for (int depth = 0; depth < 64 && !ts_node_is_null(node); ++depth) {
        if (qstrcmp(ts_node_type(node), "function_declarator") == 0) {
            const TSNode inner = field(node, "declarator");
            const QByteArray innerType(ts_node_is_null(inner) ? "" : ts_node_type(inner));
            return innerType != "parenthesized_declarator";
        }
        node = field(node, "declarator");
    }
    return false;
}

TSNode variableScope(TSNode node, bool parameter) {
    if (parameter) {
        while (!ts_node_is_null(node) && qstrcmp(ts_node_type(node), "parameter_declaration") != 0)
            node = ts_node_parent(node);
        if (ts_node_is_null(node))
            return {};
    }
    TSNode declarator{};
    for (auto scope = ts_node_parent(node); !ts_node_is_null(scope);
         scope = ts_node_parent(scope)) {
        const QByteArray type(ts_node_type(scope));
        if (parameter) {
            if (type == "function_declarator" && ts_node_is_null(declarator))
                declarator = scope;
            if (type == "function_definition")
                return scope;
            if (!ts_node_is_null(declarator) &&
                (type == "declaration" || type == "parameter_declaration" ||
                 type == "field_declaration" || type == "type_definition"))
                return declarator;
        } else if (type == "compound_statement" || type == "for_statement" ||
                   type == "for_range_loop" || type == "declaration_list" ||
                   type == "field_declaration_list" || type == "translation_unit") {
            return scope;
        }
    }
    return declarator;
}

bool constantName(const QByteArray &name) {
    bool uppercase = false;
    for (const char ch : name) {
        if (ch >= 'A' && ch <= 'Z')
            uppercase = true;
        else if (ch != '_' && !(ch >= '0' && ch <= '9'))
            return false;
    }
    return uppercase;
}

struct AnalysisContext {
    AnalysisContext(const QByteArray &source, const QString &fileName)
        : source(source), fileName(fileName) {}

    const QByteArray &source;
    const QString &fileName;
    SourceAnalysis result;
    LanguageId language = LanguageId::Plain;
    std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser{nullptr, ts_parser_delete};
    std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree{nullptr, ts_tree_delete};
    bool compilerRecoveryNeeded = false;

    struct Binding {
        int start;
        int end;
        int declaration;
        TextStyle style;
    };
    QHash<QByteArray, QVector<Binding>> bindings;
    QVector<TSNode> identifiers;
    QSet<QByteArray> constants;

    bool parseInput();
    void extractSyntax();
    void qualifyAndStyleSymbols();
    void styleBindings();
    void recoverCompilerSymbolsIfNeeded();
    QString text(TSNode node) const;
    void paint(TSNode node, TextStyle style);
    int addSymbol(TSNode node, TSNode name, const QString &kind, int parent);
    int addOutline(TSNode node, int parent, const QString &kind);
    void bind(TSNode name, bool parameter);
};
}

QString symbolCategory(const SourceSymbol &symbol) {
    if (symbol.category == "Enum")
        return "Enumerations";
    if (symbol.kind == "Type")
        return symbol.category == "Struct" || symbol.category == "Class" ||
                       symbol.category == "Union"
                   ? "Structures"
                   : "Type Definitions";
    if (symbol.kind == "Enum")
        return "Enumerations";
    if (symbol.kind == "Constant")
        return symbol.category == "Enum Constant" ? "Enum Constants" : "Constants";
    if (symbol.kind == "Variable")
        return symbol.category == "Constant" ? "Constants" : "Variables";
    if (symbol.kind == "Function")
        return symbol.definition ? "Functions" : "Function Prototypes";
    if (symbol.kind == "Macro")
        return symbol.category == "Constant" ? "Constants" : "Macros";
    if (symbol.kind == "Field")
        return "Data Members";
    return symbol.kind + 's';
}

QStringList symbolCategoryOrder() {
    return {"Structures",          "Type Definitions", "Enumerations", "Constants",
            "Enum Constants",      "Variables",        "Functions",    "Macros",
            "Function Prototypes", "Data Members"};
}

namespace {
bool AnalysisContext::parseInput() {
    if (source.size() > MaximumDocumentBytes) {
        result.error = "Analysis requires a document no larger than 4 MiB in this prototype.";
        return false;
    }
    result.styles = QByteArray(source.size(), static_cast<char>(TextStyle::Plain));
    language = languageForFile(fileName, source).id;
    const auto *grammar = languageById(language).grammar;
    if (!grammar)
        return false;
    result.supported = true;
    parser.reset(ts_parser_new());
    if (!parser || !ts_parser_set_language(parser.get(), grammar)) {
        result.error = "Cannot initialize Tree-sitter: check runtime/grammar ABI compatibility.";
        return false;
    }
    tree.reset(ts_parser_parse_string(parser.get(), nullptr, source.constData(),
                                      static_cast<uint32_t>(source.size())));
    if (!tree) {
        result.error = "Tree-sitter could not parse this document.";
        return false;
    }
    if (QFileInfo(fileName).suffix().toLower() == "h") {
        if (!ts_parser_set_language(parser.get(), languageById(LanguageId::Cpp).grammar))
            qCWarning(analysisLog) << "analysis.header_fallback_failed" << "path=" << fileName
                                   << "error=Cannot initialize the C++ header grammar.";
        std::unique_ptr<TSTree, decltype(&ts_tree_delete)> cpp(
            ts_parser_parse_string(parser.get(), nullptr, source.constData(),
                                   static_cast<uint32_t>(source.size())),
            ts_tree_delete);
        const int cErrors = errorCount(ts_tree_root_node(tree.get()));
        const int cppErrors = cpp ? errorCount(ts_tree_root_node(cpp.get())) : cErrors + 1;
        const bool cppSyntax =
            cpp && !ts_node_is_null(descendant(ts_tree_root_node(cpp.get()),
                                               {"class_specifier", "namespace_definition",
                                                "template_declaration", "reference_declarator",
                                                "access_specifier", "lambda_expression",
                                                "using_declaration", "alias_declaration"}));
        if (cpp && (cppErrors < cErrors || (cppErrors == cErrors && cppSyntax))) {
            tree = std::move(cpp);
            language = LanguageId::Cpp;
        }
    }
    result.language = language;
    const auto root = ts_tree_root_node(tree.get());
    result.syntaxErrors = ts_node_has_error(root);
    if (result.syntaxErrors && analysisLog().isWarningEnabled()) {
        QVector<TSNode> pending{root};
        int reported = 0;
        while (!pending.isEmpty() && reported < 8) {
            const auto node = pending.takeLast();
            if (ts_node_is_missing(node) || qstrcmp(ts_node_type(node), "ERROR") == 0) {
                const auto point = ts_node_start_point(node);
                qCWarning(analysisLog)
                    << "analysis.syntax_error" << "path=" << fileName
                    << "language=" << languageById(language).name << "line=" << point.row + 1
                    << "byteColumn=" << point.column + 1 << "start=" << ts_node_start_byte(node)
                    << "end=" << ts_node_end_byte(node) << "node=" << ts_node_type(node)
                    << "missing=" << ts_node_is_missing(node);
                ++reported;
            }
            for (uint32_t i = ts_node_child_count(node); i > 0; --i) {
                const auto child = ts_node_child(node, i - 1);
                if (ts_node_has_error(child) || ts_node_is_missing(child))
                    pending.append(child);
            }
        }
        if (!pending.isEmpty())
            qCWarning(analysisLog)
                << "analysis.syntax_limit" << "path=" << fileName << "limit=" << reported;
    }
    compilerRecoveryNeeded = result.syntaxErrors || source.contains("##");
    return true;
}

QString AnalysisContext::text(TSNode node) const {
    return QString::fromUtf8(
        source.mid(ts_node_start_byte(node), ts_node_end_byte(node) - ts_node_start_byte(node)));
}

void AnalysisContext::paint(TSNode node, TextStyle style) {
    const int start = static_cast<int>(ts_node_start_byte(node));
    const int end = static_cast<int>(ts_node_end_byte(node));
    std::fill(result.styles.begin() + start, result.styles.begin() + end, static_cast<char>(style));
}

int AnalysisContext::addSymbol(TSNode node, TSNode name, const QString &kind, int parent) {
    if (ts_node_is_null(name))
        return -1;
    const int index = static_cast<int>(result.symbols.size());
    result.symbols.push_back({text(name),
                              kind,
                              static_cast<int>(ts_node_start_byte(node)),
                              static_cast<int>(ts_node_end_byte(node)),
                              static_cast<int>(ts_node_start_byte(name)),
                              static_cast<int>(ts_node_end_byte(name)),
                              static_cast<int>(ts_node_start_point(name).row),
                              parent,
                              false,
                              {},
                              {},
                              false,
                              kind,
                              false,
                              false,
                              "syntax"});
    auto &symbol = result.symbols.last();
    auto qualified = ts_node_parent(name);
    while (!ts_node_is_null(qualified) &&
           qstrcmp(ts_node_type(qualified), "qualified_identifier") == 0) {
        symbol.qualifiedName = text(qualified);
        qualified = ts_node_parent(qualified);
    }
    symbol.definition =
        kind != "Function" || qstrcmp(ts_node_type(node), "function_definition") == 0;
    for (int ancestor = parent; ancestor >= 0; ancestor = result.symbols[ancestor].parent)
        if (result.symbols[ancestor].kind == "Function")
            symbol.local = true;
    if (!isCFamily(language))
        symbol.definition = kind != "Function" || !ts_node_is_null(field(node, "body")) ||
                            language == LanguageId::Bash || language == LanguageId::CMake;
    if (kind == "Function") {
        const auto body = field(node, "body");
        const int end =
            ts_node_is_null(body) ? symbol.end : static_cast<int>(ts_node_start_byte(body));
        symbol.signature =
            QString::fromUtf8(source.mid(symbol.start, qMin(end - symbol.start, 512))).simplified();
    }
    return index;
}

int AnalysisContext::addOutline(TSNode node, int parent, const QString &kind) {
    const int start = static_cast<int>(ts_node_start_byte(node));
    const int rangeEnd = static_cast<int>(ts_node_end_byte(node));
    const auto newline = source.indexOf('\n', start);
    const int end = newline >= 0 ? std::min(rangeEnd, static_cast<int>(newline)) : rangeEnd;
    const int index = static_cast<int>(result.symbols.size());
    result.symbols.push_back({QString::fromUtf8(source.mid(start, end - start)).trimmed(),
                              kind,
                              start,
                              rangeEnd,
                              start,
                              end,
                              static_cast<int>(ts_node_start_point(node).row),
                              parent,
                              true,
                              {},
                              {},
                              false,
                              kind,
                              false,
                              false,
                              "syntax"});
    return index;
}

void AnalysisContext::bind(TSNode name, bool parameter) {
    if (ts_node_is_null(name))
        return;
    const auto scope = variableScope(name, parameter);
    if (ts_node_is_null(scope))
        return;
    const int declaration = static_cast<int>(ts_node_start_byte(name));
    bindings[text(name).toUtf8()].append(
        {parameter ? static_cast<int>(ts_node_start_byte(scope)) : declaration,
         static_cast<int>(ts_node_end_byte(scope)), declaration,
         parameter ? TextStyle::Parameter : TextStyle::Variable});
    paint(name, parameter ? TextStyle::Parameter : TextStyle::VariableDeclaration);
}

void AnalysisContext::extractSyntax() {
    struct Pending {
        TSNode node;
        int parent;
        bool inFunction;
    };
    QVector<Pending> pending{{ts_tree_root_node(tree.get()), -1, false}};
    static const QSet<QByteArray> keywords = {
        "auto",       "bool",      "break",    "case",    "catch",     "char",      "class",
        "const",      "constexpr", "continue", "default", "delete",    "do",        "double",
        "else",       "enum",      "explicit", "extern",  "false",     "float",     "for",
        "friend",     "if",        "inline",   "int",     "long",      "namespace", "new",
        "noexcept",   "nullptr",   "operator", "private", "protected", "public",    "return",
        "short",      "signed",    "sizeof",   "static",  "struct",    "switch",    "template",
        "this",       "throw",     "true",     "try",     "typedef",   "typename",  "union",
        "unsigned",   "using",     "virtual",  "void",    "volatile",  "while",     "def",
        "async",      "await",     "import",   "from",    "as",        "lambda",    "with",
        "yield",      "pass",      "None",     "True",    "False",     "and",       "or",
        "not",        "in",        "is",       "elif",    "except",    "raise",     "global",
        "nonlocal",   "function",  "let",      "var",     "export",    "interface", "extends",
        "implements", "package",   "func",     "range",   "defer",     "go",        "select",
        "chan",       "map",       "fn",       "pub",     "impl",      "trait",     "mod",
        "use",        "mut",       "match",    "self",    "Self",      "where",     "fi",
        "then",       "esac",      "done",     "local",   "case",      "do"};
    while (!pending.isEmpty()) {
        auto [node, parent, inFunction] = pending.takeLast();
        const QByteArray type(ts_node_type(node));
        const bool cFamily = isCFamily(language);
        if (cFamily && type == "identifier")
            identifiers.append(node);
        if (type == "comment" || type == "line_comment" || type == "block_comment" ||
            type == "bracket_comment") {
            paint(node, TextStyle::Comment);
            continue;
        }
        if (type == "string_literal" || type == "raw_string_literal" || type == "char_literal" ||
            type == "system_lib_string" || type == "string" || type == "raw_string" ||
            type == "heredoc_body" || type == "quoted_argument") {
            paint(node, TextStyle::String);
            continue;
        }
        if (cFamily && (type == "null" || type == "true" || type == "false")) {
            paint(node, TextStyle::Constant);
            continue;
        } else if (type == "number_literal")
            paint(node, TextStyle::Number);
        else if (type == "type_identifier")
            paint(node, TextStyle::TypeReference);
        else if (type == "primitive_type" || type == "sized_type_specifier")
            paint(node, TextStyle::Primitive);
        else if (type == "field_identifier")
            paint(node, TextStyle::Field);
        else if (cFamily && (type == "const" || type == "sizeof" || type == "static"))
            paint(node, TextStyle::Primitive);
        else if (keywords.contains(type))
            paint(node, TextStyle::Keyword);
        else if (type.startsWith('#'))
            paint(node, TextStyle::Preprocessor);
        else if (!ts_node_is_named(node) && ts_node_child_count(node) == 0)
            paint(node, cFamily && (type == "(" || type == ")" || type == "{" || type == "}" ||
                                    type == "[" || type == "]" || type == "," || type == ";" ||
                                    type == "?" || type == ":")
                            ? TextStyle::Punctuation
                            : TextStyle::Operator);

        if (cFamily && type == "parameter_declaration")
            bind(nameNode(field(node, "declarator")), true);

        int newParent = parent;
        if (qstrcmp(ts_node_type(node), "ERROR") == 0 || ts_node_is_missing(node)) {
            if (result.diagnostics.size() < 12) {
                const auto point = ts_node_start_point(node);
                result.diagnostics.append(
                    QString("%1 at %2:%3")
                        .arg(ts_node_is_missing(node) ? "Missing token" : "Syntax recovery")
                        .arg(point.row + 1)
                        .arg(point.column + 1));
            }
        }
        if (!isCFamily(language)) {
            TSNode name = field(node, "name");
            QString kind;
            if (type == "function_definition" || type == "function_declaration" ||
                type == "generator_function_declaration" || type == "method_definition" ||
                type == "method_declaration" || type == "function_item" ||
                type == "function_signature_item")
                kind = "Function";
            else if (type == "class_definition" || type == "class_declaration" ||
                     type == "interface_declaration" || type == "type_alias_declaration" ||
                     type == "type_spec" || type == "struct_item" || type == "trait_item")
                kind = "Type";
            else if (type == "enum_item" || type == "enum_declaration")
                kind = "Enum";
            else if (type == "enum_variant" || type == "enum_assignment")
                kind = "Constant";
            else if (type == "mod_item")
                kind = "Namespace";
            else if (type == "macro_definition")
                kind = "Macro";
            else if (type == "field_definition") {
                kind = "Field";
                name = field(node, "property");
            } else if (type == "field_declaration" || type == "public_field_definition" ||
                       type == "property_signature")
                kind = "Field";
            else if (type == "variable_declarator" || type == "variable_assignment" ||
                     type == "const_spec" || type == "var_spec" || type == "let_declaration" ||
                     type == "const_item" || type == "static_item" || type == "assignment" ||
                     type == "short_var_declaration") {
                kind = "Variable";
                if (ts_node_is_null(name))
                    name = field(node, "pattern");
                if (ts_node_is_null(name))
                    name = field(node, "left");
                if (!ts_node_is_null(name) && qstrcmp(ts_node_type(name), "identifier_list") == 0)
                    name = ts_node_named_child(name, 0);
                const auto value = field(node, "value");
                if (!ts_node_is_null(value) &&
                    (qstrcmp(ts_node_type(value), "arrow_function") == 0 ||
                     qstrcmp(ts_node_type(value), "function_expression") == 0))
                    kind = "Function";
            } else if (type == "pair" || type == "block_mapping_pair" || type == "flow_pair") {
                kind = "Field";
                name = field(node, "key");
            } else if (type == "impl_item") {
                name = field(node, "type");
                kind = "Implementation";
            } else if (language == LanguageId::CMake &&
                       (type == "function_def" || type == "macro_def")) {
                kind = type == "function_def" ? "Function" : "Macro";
                name = descendant(node, {"argument", "unquoted_argument"});
            } else if (language == LanguageId::CMake && type == "normal_command") {
                const auto command = ts_node_named_child(node, 0);
                const auto commandName = text(command).toLower();
                if (commandName == "set" || commandName == "option")
                    kind = "Variable";
                if (commandName == "add_library" || commandName == "add_executable")
                    kind = "Type";
                name = descendant(node, {"argument", "unquoted_argument"});
            }
            if (!kind.isEmpty() && !ts_node_is_null(name)) {
                const int index = addSymbol(node, name, kind, parent);
                auto &symbol = result.symbols[index];
                if (kind == "Function" && type == "variable_declarator")
                    symbol.definition = true;
                if (language == LanguageId::Go && type == "method_declaration") {
                    const auto receiver = descendant(field(node, "receiver"), {"type_identifier"});
                    if (!ts_node_is_null(receiver))
                        symbol.qualifiedName = text(receiver) + "::" + symbol.name;
                }
                if ((symbol.name.startsWith('"') && symbol.name.endsWith('"')) ||
                    (symbol.name.startsWith('\'') && symbol.name.endsWith('\''))) {
                    symbol.name = symbol.name.mid(1, symbol.name.size() - 2);
                    ++symbol.nameStart;
                    --symbol.nameEnd;
                }
                symbol.category =
                    type == "class_definition" || type == "class_declaration" ? "Class"
                    : type == "struct_item"                                   ? "Struct"
                    : type == "type_alias_declaration"                        ? "Alias"
                    : type == "enum_variant" || type == "enum_assignment"     ? "Enum Constant"
                    : type == "const_spec" || type == "const_item"            ? "Constant"
                                                                              : kind;
                symbol.outlineOnly = kind == "Implementation";
                if (kind == "Function" || kind == "Type" || kind == "Enum" || kind == "Namespace" ||
                    kind == "Field" || kind == "Implementation")
                    newParent = index;
                if (kind == "Function")
                    inFunction = true;
            }
            for (uint32_t i = ts_node_child_count(node); i > 0; --i)
                pending.push_back({ts_node_child(node, i - 1), newParent, inFunction});
            continue;
        }
        if (type == "preproc_if" || type == "preproc_ifdef" || type == "preproc_elif" ||
            type == "preproc_else") {
            newParent = addOutline(node, parent, "Conditional");
        } else if (type == "preproc_include") {
            addOutline(node, parent, "Include");
        } else if (type == "#endif") {
            const int outer = parent >= 0 ? result.symbols[parent].parent : -1;
            addOutline(node, outer, "Conditional");
        } else if (type == "function_definition") {
            const auto declarator = field(node, "declarator");
            if (ts_node_is_null(descendant(declarator, {"function_declarator"})))
                compilerRecoveryNeeded = true;
            newParent = addSymbol(node, nameNode(declarator), "Function", parent);
            inFunction = true;
        } else if (type == "struct_specifier" || type == "class_specifier" ||
                   type == "union_specifier" || type == "enum_specifier" ||
                   type == "namespace_definition") {
            const QString kind = type == "namespace_definition" ? "Namespace"
                                 : type == "enum_specifier"     ? "Enum"
                                                                : "Type";
            const bool hasBody = !ts_node_is_null(field(node, "body"));
            const auto enclosing = ts_node_parent(node);
            const bool forward = !ts_node_is_null(enclosing) &&
                                 qstrcmp(ts_node_type(enclosing), "declaration") == 0 &&
                                 ts_node_is_null(field(enclosing, "declarator"));
            const auto following = ts_node_next_sibling(node);
            const bool standaloneForward =
                !ts_node_is_null(following) && qstrcmp(ts_node_type(following), ";") == 0;
            const bool typedefForward = !ts_node_is_null(enclosing) &&
                                        qstrcmp(ts_node_type(enclosing), "type_definition") == 0 &&
                                        ts_node_eq(field(enclosing, "type"), node);
            const int index = (hasBody || forward || standaloneForward || typedefForward)
                                  ? addSymbol(node, field(node, "name"), kind, parent)
                                  : -1;
            if (index >= 0) {
                result.symbols[index].definition = hasBody;
                result.symbols[index].category = type == "class_specifier"    ? "Class"
                                                 : type == "struct_specifier" ? "Struct"
                                                 : type == "union_specifier"  ? "Union"
                                                                              : kind;
            }
            if (index >= 0)
                newParent = index;
        } else if (type == "enumerator" || type == "preproc_def" ||
                   type == "preproc_function_def") {
            const int index = addSymbol(node, field(node, "name"),
                                        type == "enumerator" ? "Constant" : "Macro", parent);
            if (index >= 0)
                result.symbols[index].category = type == "enumerator"    ? "Enum Constant"
                                                 : type == "preproc_def" ? "Constant"
                                                                         : "Macro";
            if (type == "enumerator")
                constants.insert(text(field(node, "name")).toUtf8());
        } else if (type == "alias_declaration") {
            newParent = addSymbol(node, field(node, "name"), "Type", parent);
            if (newParent >= 0)
                result.symbols[newParent].category = "Alias";
        } else if (type == "type_definition" || type == "field_declaration" ||
                   type == "declaration") {
            const auto count = ts_node_child_count(node);
            bool anonymousContainerNamed = false;
            for (uint32_t i = 0; i < count; ++i) {
                const char *fieldName = ts_node_field_name_for_child(node, i);
                if (!fieldName || qstrcmp(fieldName, "declarator") != 0)
                    continue;
                const TSNode declarator = ts_node_child(node, i);
                const QString kind = type == "type_definition"     ? "Type"
                                     : isFunction(declarator)      ? "Function"
                                     : type == "field_declaration" ? "Field"
                                                                   : "Variable";
                const int index = addSymbol(node, nameNode(declarator), kind, parent);
                if (index >= 0) {
                    if (kind == "Variable")
                        bind(nameNode(declarator), false);
                    if (kind == "Variable" && text(node).trimmed().startsWith("extern "))
                        result.symbols[index].definition = false;
                    if (type == "type_definition") {
                        result.symbols[index].category = "Alias";
                        const auto underlying = field(node, "type");
                        if (!ts_node_is_null(underlying) &&
                            ts_node_is_null(field(underlying, "name"))) {
                            if (qstrcmp(ts_node_type(underlying), "enum_specifier") == 0 &&
                                !ts_node_is_null(field(underlying, "body")) &&
                                qstrcmp(ts_node_type(declarator), "type_identifier") == 0)
                                result.symbols[index].category = "Enum";
                            if (!anonymousContainerNamed)
                                newParent = index;
                            if (qstrcmp(ts_node_type(declarator), "type_identifier") == 0)
                                anonymousContainerNamed = true;
                        }
                    }
                }
            }
        }
        for (uint32_t i = ts_node_child_count(node); i > 0; --i)
            pending.push_back({ts_node_child(node, i - 1), newParent, inFunction});
    }
}

void AnalysisContext::qualifyAndStyleSymbols() {
    for (auto &symbol : result.symbols) {
        if (symbol.outlineOnly)
            continue;
        QStringList containers;
        for (int parent = symbol.parent; parent >= 0; parent = result.symbols[parent].parent) {
            const auto &scope = result.symbols[parent];
            if (!scope.outlineOnly || scope.kind == "Implementation")
                containers.prepend(scope.name);
        }
        containers.append(symbol.qualifiedName.isEmpty() ? symbol.name : symbol.qualifiedName);
        symbol.qualifiedName = containers.join(
            language == LanguageId::Python || language == LanguageId::JavaScript ||
                    language == LanguageId::TypeScript || language == LanguageId::Tsx
                ? "."
                : "::");
        const auto style = symbol.kind == "Function"                        ? TextStyle::Function
                           : symbol.kind == "Type" || symbol.kind == "Enum" ? TextStyle::Type
                           : symbol.kind == "Macro" ? TextStyle::Preprocessor
                           : isCFamily(language) && symbol.kind == "Constant" ? TextStyle::Constant
                           : isCFamily(language) && symbol.kind == "Variable"
                               ? TextStyle::VariableDeclaration
                               : TextStyle::Field;
        std::fill(result.styles.begin() + symbol.nameStart, result.styles.begin() + symbol.nameEnd,
                  static_cast<char>(style));
    }
}

void AnalysisContext::styleBindings() {
    // Resolve syntax-local bindings before applying naming conventions for external symbols.
    static const QSet<QByteArray> libraryFunctions = {
        "abort",   "abs",     "atoi",   "atol",    "bsearch",  "calloc",  "exit",     "fclose",
        "fopen",   "fprintf", "fread",  "free",    "fwrite",   "malloc",  "memcmp",   "memcpy",
        "memmove", "memset",  "printf", "puts",    "qsort",    "realloc", "snprintf", "sprintf",
        "sscanf",  "strcat",  "strchr", "strcmp",  "strcpy",   "strlen",  "strncmp",  "strncpy",
        "strstr",  "strtod",  "strtol", "strtoul", "vsnprintf"};
    for (const auto identifier : identifiers) {
        const int position = static_cast<int>(ts_node_start_byte(identifier));
        if (result.styles[position] != static_cast<char>(TextStyle::Plain))
            continue;
        const auto name = text(identifier).toUtf8();
        const Binding *binding = nullptr;
        const auto found = bindings.constFind(name);
        if (found != bindings.cend()) {
            for (const auto &candidate : *found)
                if (position >= candidate.start && position < candidate.end &&
                    (!binding || candidate.start > binding->start ||
                     (candidate.start == binding->start && candidate.end < binding->end)))
                    binding = &candidate;
        }
        if (binding) {
            paint(identifier, binding->style);
        } else {
            const auto enclosing = ts_node_parent(identifier);
            if (!ts_node_is_null(enclosing) &&
                qstrcmp(ts_node_type(enclosing), "call_expression") == 0 &&
                ts_node_eq(field(enclosing, "function"), identifier))
                paint(identifier, libraryFunctions.contains(name) ? TextStyle::LibraryFunction
                                                                  : TextStyle::FunctionReference);
            else if (constants.contains(name) || constantName(name))
                paint(identifier, TextStyle::Constant);
        }
    }
}

void AnalysisContext::recoverCompilerSymbolsIfNeeded() {
    if (isCFamily(language) && compilerRecoveryNeeded && !fileName.startsWith(":/"))
        recoverCompilerSymbols(result, source, fileName);
}
}

SourceAnalysis analyzeSource(const QByteArray &source, const QString &fileName) {
    qCInfo(analysisLog) << "analysis.started" << "path=" << fileName << "bytes=" << source.size();
    AnalysisContext context{source, fileName};
    if (context.parseInput()) {
        context.extractSyntax();
        context.qualifyAndStyleSymbols();
        context.styleBindings();
        // Recovery merges symbols without repainting the syntax-derived styles.
        context.recoverCompilerSymbolsIfNeeded();
    }
    const auto &result = context.result;
    if (!result.error.isEmpty() || result.syntaxErrors || !result.diagnostics.isEmpty())
        qCWarning(analysisLog) << "analysis.anomaly" << "path=" << fileName
                               << "language=" << languageById(context.language).name
                               << "syntaxErrors=" << result.syntaxErrors
                               << "recovered=" << result.compilerEnriched
                               << "error=" << result.error << "diagnostics=" << result.diagnostics;
    else
        qCDebug(analysisLog) << "analysis.completed" << "path=" << fileName
                             << "symbols=" << result.symbols.size()
                             << "supported=" << result.supported;
    return std::move(context.result);
}
