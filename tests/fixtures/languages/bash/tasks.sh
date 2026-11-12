#!/usr/bin/env bash
set -eu

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/greetings.sh"

main() {
    greet "CodeCairn"
}

main
