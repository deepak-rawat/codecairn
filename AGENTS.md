# CodeCairn agent instructions

These instructions apply throughout the CodeCairn repository.

## Read the relevant guide

- [README](README.md): setup, platform status, launch paths, troubleshooting.
- [User guide](docs/USER_GUIDE.md): features, option meanings, and expected behavior.
- [Developer guide](docs/DEVELOPER_GUIDE.md): architecture, ownership, storage,
  build/deployment, tests, and change contracts.

Use these as the sources of truth rather than duplicating their full contents.
Read the relevant implementation and tests before changing behavior.

## UI style and interaction

- Keep the compact Qt Widgets interface: flat controls, square borders,
  restrained spacing, dense lists, and existing splitter/panel structure.
  Do not replace it with cards, oversized controls, or web-style chrome.
- Reuse UiAppearance, configured colors/fonts/dimensions, platform font
  fallbacks, and existing vector icons. Keep icon sizes in logical pixels;
  do not use a high-DPI bitmap's physical size as its UI size.
- Reuse one QAction across menus/toolbars/shortcuts and maintain enablement,
  focus, hover, selection, disabled states, and keyboard behavior.
- Use concise labels, tooltips, and inline validation. Do not add per-dialog
  Help buttons or F1 help popups unless explicitly requested; explain options
  in the user guide instead.
- Preview must not steal focus or silently pin a document. Explicit activation
  and editing have different behavior; preserve that distinction.
- Keep source, context, and results roles distinct. Follow the documented
  filter, source-link, result replacement, reset-on-close, and session behavior.

## Build and run

Use Bash/zsh syntax for command examples, including MSYS2 UCRT64 on Windows.
Run commands from this project directory. Use out-of-source CMake builds;
never change an existing build directory to a different compiler/toolchain.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel
```

Use all available CPUs for CodeCairn builds; do not impose a two-job cap.
Release scripts detect the CPU count automatically.

On Windows use MSYS2 UCRT64, adding `-DCMAKE_C_COMPILER=gcc` and
`-DCMAKE_CXX_COMPILER=g++` on first configure. Do not mix dependency prefixes.
The default bundled Windows build deploys runtime files; building only the
codecairn target does not. See the README for other platforms and managed builds.

The Windows build runs as `./build/bin/codecairn.exe`; keep DLLs/plugins/resources
with it. If a running executable prevents linking, ask the user to save/close it.
Do not terminate their app or discard unsaved work.

## Testing and validation

- Build the affected targets and run the smallest relevant existing suites.
  Add regression coverage for behavior changes and test the visible outcome.
- Use CTest selectors together; expand coverage when shared behavior warrants it.
  For example:

```sh
ctest --test-dir build --output-on-failure -R '^(interactions|references)$'
```

- CTest configures offscreen GUI tests. Direct QtTest runs need
  `QT_QPA_PLATFORM=offscreen` unless intentionally checking native input.
- Use isolated temporary files/profiles/state. Never run tests against user
  preferences, dirty workspaces, or source files without explicit permission.
- Check actual pass/fail/skip results. Do not claim native-platform certification
  from offscreen tests or app-wide performance from small fixtures.
- Fix warnings introduced by changes; do not blanket-suppress warnings.
  Report validation limitations instead of presenting unchecked work as passed.

## Diagnosing failures

- Read the affected local diagnostic session before guessing; follow the
  [crash investigation workflow](docs/DEVELOPER_GUIDE.md#diagnostics-and-crash-investigation)
  and [logging contract](docs/USER_GUIDE.md#logs-and-crash-reports).
- Preserve logs, matching binaries/symbols, and source revision before rebuilding
  or clearing state. An unclean marker is not proof of a crash or a call stack.
- Use disposable workspace/profile/state paths for reproductions. Do not remove
  live locks, alter OS crash collection/registry settings without approval, or
  automatically upload logs, source, reports, or memory dumps.

## Code and ownership

- Use C++20, Qt Widgets, and upstream Scintilla; follow existing service/facade
  boundaries rather than introducing another toolkit or generic framework.
- Follow [.clang-format](.clang-format) and [.cmake-format.json](.cmake-format.json):
  four spaces, attached C++ braces, Qt pointer/reference placement, 100-column
  target. Do not sort includes or reflow comments contrary to the config.
  Format changed implementation files only; preserve fixture/example layout
  and the exclusions in [.clang-format-ignore](.clang-format-ignore).
- Keep comments sparse and explain non-obvious rationale. Prefer clear types,
  guards, and existing helpers over casts, duplicated logic, or broad catches.
- Workers exchange snapshots/results, not widget pointers. Accept results only
  for current revisions/generations and recheck lifetimes after modal operations.
- Source offsets are UTF-8 bytes; QString/LSP positions use UTF-16 conventions.
  Use the shared conversions and validate hashes before navigating old ranges.
- Styling is not editing. Preserve source bytes, BOM/line endings, undo, dirty
  buffers, context lock, and explicit syntax-versus-semantic provenance.
- Settings/index changes must cover defaults, validation, serialization,
  invalidation/migration, errors, and tests. Never silently overwrite corrupt or
  externally changed state.

## Documentation and scope

Update the user guide for options/workflows and the developer guide for
implementation/build/test contracts. Keep README as the entry point and preserve
its single first-line inspiration credit without adding comparisons elsewhere.

Preserve unrelated worktree changes. Do not commit generated build outputs,
dependency trees, runtime binaries, or temporary captures. Approved application
assets follow their existing generation process. Commit only when requested.
Keep commit messages short and concise; summarize the change without unnecessary detail.
