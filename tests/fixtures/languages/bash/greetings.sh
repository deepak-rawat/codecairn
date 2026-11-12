greet() {
    local name="${1:-world}"
    printf 'Hello, %s\n' "$name"
}
