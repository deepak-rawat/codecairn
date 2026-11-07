# CodeCairn user guide

CodeCairn is a desktop source browser and editor. This guide explains its
features, controls, and options. For installation, use the
[README build instructions](../README.md#build-and-run). For implementation and
contributing, use the [developer guide](DEVELOPER_GUIDE.md).

[Workspace](#opening-and-managing-a-workspace) | [Navigation](#browsing-and-navigation) |
[Search](#search-and-references) | [Results](#working-with-search-results) |
[Appearance](#appearance-and-editor-options) | [Panels](#panel-options) |
[Preferences](#advanced-preferences) | [Logs](#logs-and-crash-reports) |
[Troubleshooting](#troubleshooting)

## Installing and launching

Standalone releases target Windows 11 x64, macOS 26+ ARM64, and Ubuntu 24.04+
x64 Linux. Homebrew, MSYS2, and Arch managed packages are validated separately.
Use the [release instructions](RELEASING.md) for artifacts, package-manager
installation, and unsigned macOS limitations.

Use `cairn .`, `cairn "project folder"`, or `cairn source.cpp` from a terminal.
The command returns after starting the application; each invocation starts a
new process, not a request to an existing window. Relative paths are resolved
from the terminal's current directory. `cairn --help` and `cairn --version`
print directly in the terminal. `--wait` is not supported.

Keep the whole portable distribution together. Adding its launcher directory
to PATH is optional and manual. Portable downloads still store preferences and
caches in normal per-user locations. Package-manager installations rely on
their installed dependencies and must not be moved as standalone applications.

## Opening and managing a workspace

Open a folder through **Project > Open Folder**, or open a source file. Opening
a first file without a workspace uses its containing directory. An outside file
can be opened without changing the workspace; the application asks before using
its folder as a new workspace.

The index discovers source files and symbols in the background. Opening a folder
does not open an arbitrary file or example automatically. Browsing/indexing does
not require writing metadata into the source directory.

### Project commands

| Command | Meaning | When to use it |
|---|---|---|
| Open Folder | Select the workspace root | Start browsing a project |
| Close Project | Save its session, close its documents, and clear the startup workspace hint | Finish with a project without deleting its saved session |
| Refresh File List | Refresh discovery/index state and resume automatic updates | New, deleted, or renamed files; after cancelling indexing |
| Rebuild Symbol Index | Reparse the known file catalog | Suspected stale symbol extraction |
| Rebuild Files and Symbols | Rediscover files and reparse | Broader catalog/exclusion changes |
| Cancel Background Indexing | Stop current work and pause automatic refresh | Temporarily stop a large scan |
| Compilation Database Directory | Select a directory containing compile_commands.json | Configure reliable C/C++ semantic navigation |
| Restart Language Service | Restart semantic services | Compilation settings or server installation changed |
| Excluded Directory Names | Skip matching directory names at any depth | Omit dependencies, generated output, or large irrelevant trees |

Exclusions are exact directory names, not wildcard patterns. Defaults include
`.git`, `.svn`, `build`, `node_modules`, and `target`. Remove an exclusion if that
directory contains source you want indexed. Symlinks are not followed.

Filesystem notifications trigger background checks; periodic checks recover
missed events. The default interval is six hours. Unchanged files reuse cached
symbols, and unchanged automatic checks stay quiet. Warnings are available in
the status bar and Project Files tooltip.

### Sessions and source-file safety

The workspace session remembers open files, tab order, active file, selections,
and scroll positions. Startup without explicit paths restores the last available
workspace; explicit command-line paths take precedence. Missing files are
reported and skipped.

Unsaved source text is **not** a session backup. Normal save/discard/cancel
prompts still apply. Source files must be UTF-8 and within the 4 MiB limit.
Existing BOM and line endings are preserved. Saves are atomic and check for
external changes.

Clean open files can reload after external edits. Dirty buffers are preserved
instead of being silently replaced. Review external changes before saving.

## Browsing and navigation

### Window regions

| Region | Purpose |
|---|---|
| Left Symbol Window | Current-file outline, or searched queries while Search Results is active |
| Center source editor | Editable source documents and the separate Search Results page |
| Upper-right Context Window | Read-only source/definition preview |
| Lower-right Project Files | File names, paths, and optional sizes |
| Project Symbols | Workspace-wide symbols, including unopened files |
| Folders | Hierarchical file browsing |
| Symbol Categories | Symbols grouped by declaration category |

The left outline follows the editor's selection, choosing the corresponding or
innermost enclosing visible symbol. It expands/reveals the row without moving
the source caret or taking focus. Clicking an outline row performs navigation
in the other direction.

### Preview, open, pin, and lock

- **Single-click or Up/Down in a browser:** browse while retaining panel focus.
  Project file/symbol navigation can reuse a clean preview tab.
- **Enter or double-click:** open explicitly, pin the document, and focus source.
- **Edit a preview:** pin it automatically; dirty documents are not reused.
- **Double-click a document tab:** keep a preview as a normal document.
- **Lock context:** freeze the right-hand preview until unlocked. This is
  different from pinning an editor tab.
- **Go Back / Go Forward:** navigate editor history. Context has its own
  independent back/forward history.

An identifier can be followed with Ctrl-click in source, or plain link
hover/click in context. **Jump to Definition** is also available from Search.
Definition ambiguity may require selecting a candidate.

Syntax-index candidates are labeled unverified. A same-named symbol is not
necessarily the definition of the selected identifier.

### Temporary panel filters

Typing in the Symbol Window, Project Files, or Project Symbols uses that
panel's filter box. Text remains while moving between the box and its list,
so rows do not shift while you browse.

The filter clears when you:

- Leave the panel.
- Confirm a result with Enter or double-click.
- Press Escape in the filter or list.
- Navigate back or forward.

The full list returns while preserving the chosen item. Project-symbol
confirmation waits for the current query; Escape or leaving cancels queued
confirmation rather than allowing an old result to move source later.

File/outline filters accept case-insensitive fragments. Project Symbols also
supports unordered fragments such as `doc write`, a trailing `(` to restrict
to functions, or a leading `?` for a case-insensitive RE2 expression, such as
`?^parse.*`. These browser queries are separate from the search methods below.

### Symbol categories

| Category | Includes |
|---|---|
| Structures | Structs, classes, unions |
| Type Definitions | Typedefs and type aliases |
| Enumerations | Enum declarations, including anonymous enums named by a typedef |
| Constants | Constants and object-like macros |
| Enum Constants | Named enumerators |
| Variables | Variable declarations/definitions |
| Functions | Function definitions |
| Macros | Function-like macros |
| Function Prototypes | Function declarations without bodies |
| Data Members | Class/struct member data |

Empty categories are omitted; language-specific groups such as Namespaces can
follow. Pointer typedefs and aliases of existing enums remain Type Definitions.
Selecting a category filters Project Symbols; **Show All Symbol Categories**
removes that restriction.

Counts and category children cover the workspace or live buffers, independent
of the Project Symbols text filter. Open buffers override their disk symbols.

## Search and references

**Find** searches the active source file for case-sensitive literal text.
Find Next/Previous repeat it. Use **Lookup References** for a selected source
symbol, or **Search Project** for general project-wide text/pattern searches.
The latter two share an engine but remember their own options.

The lookup dialog has Find and Search In controls, an Options group, and
**Search / Close** buttons. Search starts the operation; Close dismisses the
dialog without closing or clearing an existing Search Results tab.

### Choosing a search method

| Method | What it means | Example |
|---|---|---|
| Simple String | Literal text; substrings are allowed when Whole Words Only is off | `addr` can find `address` and `rtc_addr` |
| Regular Expression | RE2 pattern matching | `^#include` or `item[0-9]+` |
| Keyword Expression | Boolean combinations within a line-distance window | `(send OR receive) AND packet !error` |
| Look Up Reference | With smart matching, semantic uses of the exact selected symbol | Find a particular member without unrelated same-named members |
| Text Occurrences (Whole Identifiers) | Identifier text, not symbol identity | `count` matches `count`, not `counter` or `item_count` |

Choose smart lookup for "where is this symbol used?", whole identifiers for
"where does this exact name occur?", and Simple String for arbitrary text.
Regex supports line anchors but not lookaround or backreferences.

Smart matching requires an originating symbol selected in source or a symbol
browser. Changing the query to a different name requires selecting that symbol
again, or explicitly choosing a text search. Provider failure never silently
switches to text matches.

### Lookup dialog options

| Control | Meaning and applicability |
|---|---|
| Find | Query/pattern. Prefilled from the selected symbol; its dropdown offers recent completed queries |
| Search In: All Project Files | Project catalog plus captured open-buffer overlays |
| Search In: Current File | Only the originating source file; requires one |
| Search In: Selected Project Files | Files selected in Project Files |
| File Types | Optional filename wildcards, e.g. `*.cpp;*.h`; blank means all types in the chosen scope |
| Search Method | Select one of the methods above |
| Case Sensitive | Match letter case in text modes. Smart matching instead follows provider/language semantics |
| Whole Words Only | Prevent partial-word matches in literal/regex searches. Forced on for reference/identifier modes |
| Skip Inactive Code | Skip known inactive C/C++ regions using compiler configuration |
| Skip Comments | Exclude comments from text searches |
| Search Only Comments | Search comments rather than normal code; mutually exclusive with Skip Comments |
| Smart Reference Matching | Qualify exact symbol identity through a language server; only available for Look Up Reference |
| Include Declarations | Include declarations alongside uses in smart mode |
| Include Strings | Permit matches in string literals in text modes |
| Replace all results | Replace existing searches only after the full new search succeeds; off on each dialog opening |
| Touch files and cause recompile | After the full search, request confirmation to update matching files' modification timestamps; off on each opening |
| Lines of Context | Keyword-only maximum distance between matching lines: 0 requires the same line |
| Find Word Variations | Keyword-only common English suffix forms, such as open/opens/opened/opening; not a dictionary |
| Find Word Fragments | Keyword-only substring matching inside words/identifiers |
| Include in Results... | Choose displayed fields and source-link creation |

Leave **both comment boxes unchecked** to include comments. This does not
automatically include strings: Include Strings is independent.
Comment/string filters need a syntax classifier; unsupported files produce
warnings rather than pretending to classify their text.

Disabled controls do not promise capabilities the chosen method lacks. Standard
semantic references do not guarantee inclusion of comments, strings, or inactive
branches. Coverage depends on the language server and its index/build settings.

Inactive-code filtering is conservative. Macro-dependent conditions require an
exact compilation command; without one, only literal `#if 0`/`#if 1` conditions
are eligible. Compiler errors, unsupported languages, and uncertain regions
produce warnings and retain text. Captured workspace header buffers are included.

Timestamp-touching does not rewrite contents or run a build. It can cause your
build system to recompile files later. Missing, changed, stale, or unsaved targets
are rejected. Confirmation is still required after checking the box.

### Keyword expression syntax

| Syntax | Example | Meaning |
|---|---|---|
| Space, AND, or `+` | `socket buffer` | Both terms must occur within the selected line distance |
| OR | `send OR receive` | Either term |
| NOT, `!`, or `-` | `socket !error` | Exclude windows containing the unwanted term |
| Parentheses | `(save OR write) AND file` | Group Boolean expressions |
| Quoted text | `"connection lost"` | Treat the phrase as a term |
| `=` prefix | `=Socket` | Force case sensitivity for that term |
| `?` followed by a quoted regex | `? "^parse" AND error` | Use a regex term |

For example, with Lines of Context = 3, terms on lines 10 and 13 can qualify.
Overlapping keyword context blocks are coalesced. A per-file budget of one
million term/line matches bounds memory; narrow expressions that exceed it.

### Include in Results options

| Option | Effect |
|---|---|
| Container function or class | Include the enclosing declaration name when available |
| File names | Include the target filename/path |
| Line numbers | Include one-based source line numbers |
| Source text | Include a source excerpt |
| Create source links | Show clickable link icons and enable linked navigation |
| Project-relative file names | Show names relative to the workspace instead of full paths |

When file names are on but line numbers and source text are off, each matching
file is listed once per search, including across pages. Disabling source links
retains result text but removes linked navigation for those rows.

## Working with search results

Results are read-only text with a caret, not an editable source document.

- Click text to position the caret and preview its referenced line in context.
- Use arrows, Home/End, Page Up/Down, Ctrl for word/document movement, and Shift
  to extend selection.
- Drag to select, double-click to select a word, and use Ctrl+C or Edit > Copy.
- Click the red link icon, press Enter, or use Ctrl+L to open source.
- Click the matching source-margin icon or use Ctrl+L on a linked source line
  to return to the result.
- Use main-toolbar previous/next icons, or the reference shortcuts, from source.
- Hovering a link icon adds a red outline; source and result icons share logical
  sizing at high DPI.

Source and context highlight the entire referenced line in teal by default.
Source is still editable: typing replaces its selection as with any normal
selection. Collapse the selection before editing if you do not intend to
replace the line. Context/results remain read-only.

While results are active, the left Symbol Window lists queries rather than file
declarations. Click or use Up/Down to reveal a query and preview its first match.
Delete, or the query's context menu, removes that query's groups only.

### Result actions

Right-click the results to access these operations:

| Action | Effect |
|---|---|
| Copy | Copy selected result text |
| Cancel Search | Stop pending work without deleting completed groups |
| Load More | Fetch the next page, at most 10,000 matches |
| Remove Selected | Remove selected result rows/groups |
| Export... | Save visible result information as UTF-8 text |
| Touch Files... | Confirm timestamp updates for matching files |
| Clear Results | Cancel work and clear history without closing the tab |
| First / Previous / Next / Jump | Navigate source links |

Group headings describe the search; after manual row removal, treat their
original summary/count metadata accordingly. Source text/ranges are snapshots.
Edits before/after a match adjust its link, including undo/redo. Editing inside
the match makes it stale, even if a later undo restores the text. External edits
still require hash validation; rerun stale searches rather than guessing offsets.

### Result lifecycle

| Operation | What happens to history |
|---|---|
| Search, Replace unchecked | Append a group to existing searches |
| Search, Replace checked | Stage all pages, then save/commit the replacement together |
| Failure, Cancel Search, or replacement save error | Keep previous completed searches |
| Successful zero-match replacement | Replace old matches with an empty group for the new query |
| Explicitly close Search Results | Cancel work, clear groups/link icons, and save the empty history |
| Quit with Search Results still open | Save completed groups for restoration with the workspace |

For a paged replacement, old results stay visible until all pages complete.
Use Load More or cancel the pending replacement. Closing the results tab is
always an explicit reset, including during replacement; the next lookup starts
fresh. Search > Search Results focuses retained results while history exists.

History is bounded to 1,000 groups / 100,000 rows and a 32 MiB workspace-session
file. Oversized or invalid state is reported, not silently overwritten.
Previously scanned/pruned files are revalidated between pages; changed buffers
or files require a fresh search instead of silently shifting page offsets.

## Appearance and editor options

Use **Options > Source Appearance** for common controls. Source/context share
the profile; the context scale is an additional multiplier.

| Control | Meaning | Default |
|---|---|---|
| UI font | Font used by menus/panels/controls; empty configuration chooses a platform font | Platform default |
| UI font size | Logical pixel size of interface text | 11 px |
| Source font | Source/context base family; empty selects a platform monospace font | Automatic monospace |
| Base size | Source font size in 96-DPI reference points | 8.25 pt (11 logical px) |
| Declaration scale | Additional size multiplier for function/type declaration names | 1.2 |
| Same size for all syntax styles | Ignore per-style/declaration size variation | Off |
| Show spaces and tabs | Display whitespace markers, not insert characters | Off |
| Highlight current line | Background emphasis for the caret's line | Off |
| Color role / Color | Choose a named theme or syntax foreground role and change its color | Profile palette |
| Save Configuration | Atomically save the complete current profile | Explicit action |
| Restore Defaults | Restore embedded defaults in memory; save to persist | Explicit action |

**View > Line Numbers** controls the source gutter and defaults off.
Zoom In/Out and Reset Source Zoom are temporary; they do not rewrite the profile.
Scintilla uses a uniform line height per view, so larger declaration styles can
increase spacing on other lines too.

Source sizes use the same 96-DPI reference on every OS: 8.25 pt renders at
11 logical pixels before display scaling, matching the Windows default.
macOS converts to its native 72-DPI points internally, so source and context
no longer appear smaller than the panels. This also applies to existing saved
profiles; no settings reset is needed. Custom sizes, syntax emphasis, context
scale, and temporary zoom remain available.
Source and context fonts update automatically when you move the window between
monitors or change the current display's scaling; restarting is not required.

### Panel options

The Symbol Window, Project Files, and Project Symbols have options buttons or
matching Options-menu entries:

| Option | Meaning | Default |
|---|---|---|
| List font | Override the UI font for this panel | Inherit |
| Font size | Panel logical pixel size; 0 means inherit UI size | 0 |
| Text color / Background | Panel color overrides; empty means inherit | Inherit |
| Show column headings | Show the table header | Off for outline; on for files/symbols |
| Show file/path column | Show locations in file/symbol lists | On |
| Show file sizes | Show byte sizes in Project Files | On |
| Show class/struct members | Include member rows in the outline | On |
| Show enum constants | Include enumerator rows in the outline | On |
| Show local declarations | Include function-local declarations | Off |
| Bold functions at least (lines) | Emphasize long functions; 0 disables the threshold | 50 |

OK applies/saves panel choices; Cancel leaves them unchanged. Restore Defaults
resets that panel's options. Outline sort and expand/collapse controls change
tree presentation rather than source contents.

### Context Window options

| Option | Meaning | Default |
|---|---|---|
| Track: selected | Resolve the identifier at the source selection | Selected |
| Track: enclosing | Show the containing function/declaration | Available mode |
| Track: off | Disable automatic context tracking | Available mode |
| Track identifiers in comments | Permit automatic lookup within comment text | Off |
| Track identifiers in strings | Permit automatic lookup within string literals | Off |
| Source font scale | Multiply context font size relative to source; range 0.5-2 | 1.0 |

Comment/string tracking does not turn that text into semantic code. Unresolved
names can still produce candidates or errors. Context lock overrides automatic
replacement. Unlock/refresh a stale preview before following its old links.

## Advanced preferences

The active profile path is shown in Source Appearance. Use
**Open UI Configuration File** to edit it and **Reload UI Configuration** to
validate/apply changes. `--config <file>` selects a portable profile, not a new
session-storage location.

Missing keys inherit [embedded defaults](../config/default-settings.json).
Unknown keys, wrong types, and invalid ranges/colors are errors. An invalid
reload retains the last valid appearance; invalid/external files are not
overwritten automatically. Explicit saved values remain in effect when defaults
change; use Restore Defaults to adopt a new palette.

### Editor settings available in JSON

| Key under `editor` | Meaning | Default |
|---|---|---|
| `tabWidth` | Display width of a tab, in space columns | 4 |
| `indentWidth` | Width of one indentation level | 4 |
| `useTabs` | Use tabs rather than spaces for indentation | false |
| `extraAscent` | Extra pixels above the text baseline | 0 |
| `extraDescent` | Extra pixels below the text baseline | 0 |

Font, declaration scale, whitespace, line-number, uniform-size, and current-line
settings correspond to the controls described above. Source font sizes are
96-DPI reference points (1 pt = 4/3 logical px); UI dimensions are logical pixels.

### Syntax style settings

Each entry under `syntax` supports `foreground`, optional `background`,
`fontFamily`, `scale`, `bold`, and `italic`. Empty family/background values
inherit the source defaults; uniform sizing overrides style scale differences.

| Role | Intended text |
|---|---|
| `plain`, `comment`, `keyword` | Base source, comments, language keywords |
| `string`, `number`, `punctuation`, `operator` | Literals and syntax punctuation/operators |
| `type`, `primitive`, `typeReference` | Type declarations, builtin types, type uses |
| `function`, `functionReference`, `libraryFunction` | Function declarations, calls, recognized library calls |
| `field`, `parameter` | Members and parameters |
| `variable`, `variableDeclaration` | Variable uses and declaration sites |
| `constant`, `preprocessor` | Constants and preprocessor text |

These are syntax-based hints, not compiler semantic tokens. C/C++ defaults use
yellow types/punctuation, amber parameters, blue variables, green members/calls,
red constants/numbers, and enlarged white function declarations.

### Theme color roles

Colors use `#RRGGBB` strings. Select them by role rather than adding arbitrary
widget-specific colors.

| Role or family | Meaning |
|---|---|
| `source.background`, `source.foreground` | Source base colors |
| `source.selectionBackground` | Normal source selection |
| `source.referenceSelection` | Full-line reference selection, including inactive context |
| `source.additionalSelection` | Additional selection background |
| `source.caret`, `source.currentLine`, `source.lineNumbers` | Caret, current-line, and line-number colors |
| `ui.*` | Panel/control surfaces, text, borders, tabs, inputs, hover/checked/selection states, scrollbars, tree branches |
| `menu.*` | Menu bar/popups, text, selection, borders, disabled items |
| `tooltip.*` | Tooltip foreground/background |

### UI geometry and timing

| JSON key | Meaning | Default |
|---|---|---|
| `ui.rowHeight` | Minimum compact list-row height | 14 |
| `ui.bandHeight` | Menu/control/toolbar band sizing | 23 |
| `ui.iconSize` | Main UI and reference-link icon size | 16 |
| `ui.symbolIconSize` | Outline/browser symbol icon size | 12 |
| `ui.splitterWidth` | Draggable divider width | 5 |
| `ui.scrollbarWidth` | Scrollbar width/height | 16 |
| `ui.treeIndent` | Tree nesting indentation | 13 |
| `layout.windowWidth`, `layout.windowHeight` | Initial window size before saved geometry | 1446, 742 |
| `layout.columns` | Initial left/editor/right splitter sizes | [252, 660, 524] |
| `layout.rightPanes` | Initial context/browser splitter sizes | [330, 320] |
| `behavior.parseDebounceMs` | Pause before reparsing edited source | 180 ms |
| `behavior.contextDebounceMs` | Pause before automatic context/hover work | 90 ms |
| `behavior.searchDebounceMs` | Pause before project-symbol filtering | 100 ms |
| `behavior.workspaceRefreshMinutes` | Periodic content check; range 1-35791 | 360 min |

Sizes may be increased by font/icon metrics. Saved window geometry takes
precedence over initial layout values. **Reset Reference Layout** reapplies
configured splitter sizes. These preferences do not change text or undo state.

## Language services, storage, and limits

Syntax support includes C/C++, Python, Bash, JavaScript/JSX, TypeScript/TSX, Go,
Rust, JSON, YAML, and CMake. Semantic C/C++ navigation uses clangd. Optional
servers for other languages must be installed separately; see
[server setup](#configuring-clangd). JSON/YAML/CMake have syntax
support only, and non-clangd integration is not certified.

For reliable semantic results, install the project's SDK/compiler headers and
provide its compilation database. An index rebuild cannot repair missing
compiler flags or guarantee that a server has finished indexing.

Preferences, window state, workspace sessions, generated indexes, and workspace
options are separate files in per-user storage. See the
[location table](#per-user-locations) and
[storage reference](DEVELOPER_GUIDE.md#storage-and-settings).
No source-directory metadata is required.

Concurrent external changes can cause saves to be refused. Do not remove live
writer locks. Close the app, back up/repair only the affected state file, and
restart after conflicts. Removing session state is not a source-file recovery
mechanism and does not recover unsaved buffers.

### Configuring clangd

Tree-sitter supplies syntax structure; clangd resolves C/C++ meanings such as
overloads, includes, and references. Syntax candidates are labeled **unverified**,
not presented as compiler-resolved definitions.

Build the project you are browsing with compilation database export enabled.
CodeCairn searches for `compile_commands.json` in the workspace root, `build`,
`build/ucrt64`, and `build/desktop`. Or choose **Project > Compilation Database
Directory**, then **Restart Language Service** after changing compilation settings.
CodeCairn's own build database does not describe other projects you open.

CodeCairn creates a normalized copy in the workspace's per-user `clangd`
directory. It does not run a project build, enable clangd project/user
configuration, or run query-driver probes. Your project's compiler/SDK headers
must already be installed. Without a database, clangd uses fallback flags;
without clangd, syntax indexing still works.

Other optional servers are discovered on `PATH`, not installed automatically:

| Language | Command |
|---|---|
| Python | `basedpyright-langserver --stdio` |
| Bash | `bash-language-server start` |
| JavaScript / TypeScript / TSX | `typescript-language-server --stdio` |
| Go | `gopls` |
| Rust | `rust-analyzer` |

Only clangd has integration-test coverage.

### Per-user locations

The application name is `CodeCairn`; the organization component is empty.
For Qt 6.7+ the default locations are:

| OS | UI preferences | Application state directory |
|---|---|---|
| Windows | `%LOCALAPPDATA%\CodeCairn\settings.json` | `%LOCALAPPDATA%\CodeCairn\State` |
| Linux | `~/.config/CodeCairn/settings.json` | `~/.local/state/CodeCairn` |
| macOS | `~/Library/Preferences/CodeCairn/settings.json` | `~/Library/Preferences/CodeCairn/State` |

Linux honors `XDG_CONFIG_HOME` and `XDG_STATE_HOME`. Qt 6.5/6.6 instead stores
state under `AppLocalDataLocation/state`: local app data on Windows,
`~/.local/share/CodeCairn/state` on Linux (`XDG_DATA_HOME` overrides the base),
and `~/Library/Application Support/CodeCairn/state` on macOS.
No earlier settings, sessions, or workspace metadata are imported or cleaned up.

## Logs and crash reports

**Help > Open Logs Folder** opens the configured diagnostic directory. These
logs are separate from preferences, workspace indexes, and saved tabs:

| Platform | Default directory |
|---|---|
| Windows | `%LOCALAPPDATA%\CodeCairn\Logs` |
| Linux | `$XDG_STATE_HOME/CodeCairn/logs` if set to an absolute path; otherwise `~/.local/state/CodeCairn/logs` |
| macOS | `~/Library/Logs/CodeCairn` |

Each launch creates a `session-<UTC-time>-<PID>-<UUID>` directory containing:

| File | Meaning |
|---|---|
| `codecairn.jsonl` | Current log; one JSON object per line |
| `codecairn.1.jsonl` through `codecairn.4.jsonl` | Rotated logs, with larger numbers older |
| `session.json` | Diagnostic environment, start/end status, and exit code when available |
| `session.lock` | Protects an active instance; do not remove a live lock |

Normal (`info`) detail includes startup/shutdown, analysis-start breadcrumbs,
parse anomalies, file read/save and settings/state failures, index/search errors,
language-service status/errors, and emitted Qt warnings/errors. Syntax-error
records identify the file, language, line, UTF-8 byte column, and parser node,
without copying its source text (at most eight locations per analysis).
Language-server warning/error diagnostics include file, line, and UTF-16 column
(at most twelve per notification). Logged lines/columns are one-based.
Normal no-match searches, cancellation, stale analysis, and first-run missing
optional settings are not warning conditions. Binary/oversized non-source assets
skipped during cataloging are debug-only; unreadable files and invalid source
documents still produce warnings.

`--log-level debug` additionally records successful analysis,
revision acceptance/discard, and navigation/request detail. For example, from
the project directory in Bash/zsh:

```sh
./build/codecairn --log-level debug --log-dir "$HOME/CodeCairnDiagnostics"
```

On Windows use `./build/bin/codecairn.exe` in MSYS2 UCRT64; on macOS use
`./build/codecairn.app/Contents/MacOS/codecairn`.

`--log-dir` requires an absolute path and affects only diagnostics, not workspace
state or preferences. The defaults limit each session to five files of 5 MiB
each and retain ten recent sessions. Active sessions, newly detected unclean
sessions, and unrecognized/malformed data are protected, so retention can exceed
that count. Copy an important session before a later launch can prune it.

### Unclean shutdown versus crash report

On the next successful logging startup, an unlocked session without a normal
completion marker is marked `unclean`. A notice names the affected directories
and offers **Open Logs Folder**. This can mean a crash, forced termination,
power loss, or failure to save the completion marker. It is not proof of a crash,
does not restore unsaved source, and does not save a call stack.

CodeCairn does not install a signal/exception crash handler. A Qt fatal message
can be logged before aborting, but an arbitrary native crash may bypass logging.
Call stacks require an attached debugger or OS crash collection. Windows WER
local dumps require opt-in configuration; Linux core dumps depend on system
settings; macOS reports can be viewed in Console. See
[crash investigation](DEVELOPER_GUIDE.md#diagnostics-and-crash-investigation)
for setup and collection.

### Privacy and failures

Logging is local only, with no automatic upload. Records can contain project
paths, symbol names, compiler diagnostics, and language-server stderr. Complete
source buffers and LSP request/response payloads are not intentionally logged,
but diagnostic text can contain code fragments. Review logs before sharing.
Crash dumps are more sensitive: they can contain source text and other process
memory. File permission requests are not a substitute for OS account/ACL security.

Records are bounded and carry a `truncated` flag when clipped. Writes are flushed
after each record, not guaranteed durable against power loss; a failed write
can leave a partial final JSON line. Startup errors before Qt/command-line
initialization completes are not captured in these files. `--help` and
`--version` exit before creating a diagnostic session.

File-start/write/rotation problems are reported visibly while the application
continues with console/debugger output. Fix the path, permissions, or free space
and restart to resume file logging. A completed marker's `loggingComplete: false`
means file logging failed during that session.

## Troubleshooting

| Symptom | Check |
|---|---|
| An option is disabled | Its method/provider may not support it; smart lookup disables text-only controls |
| Smart lookup rejects a typed name | Select that exact symbol in source/browser, or choose a text method |
| No matches | Scope, selected files, File Types, case/whole-word, comments/strings, inactive-code filtering |
| Too many same-named matches | Use smart matching instead of text occurrences |
| Wrong/missing semantic target | Compilation database, SDK paths, server availability/index readiness |
| Results show stale links | Rerun after source changes; do not guess old offsets |
| Previous results still visible during Replace | Load remaining pages or cancel; replacement waits for full success |
| Closed results disappeared | Expected: closing the tab resets searches; quitting with it open preserves completed groups |
| A filter disappears | Expected when leaving the panel, confirming, escaping, or using history navigation |
| A filter result activates late | Escape or leave the panel to cancel pending confirmation |
| Font changes increase line spacing | Scintilla uses the largest active style for a uniform view line height |
| Preferences fail to apply/save | Active profile, reload error, permissions, or external-write conflict |

### Build and runtime troubleshooting

| Symptom | Check |
|---|---|
| CMake cannot find a library | Install development headers/libraries and use the correct toolchain or `CMAKE_PREFIX_PATH` |
| Tree-sitter ABI check fails | Install a runtime supporting ABI 14 and 15; the CLI alone is insufficient |
| Source download fails | Check dependency URLs and connectivity; do not bypass hash verification |
| Windows launch or Qt tests fail | Run the default build or `--target deploy`; keep runtime files and remove unintended Qt plugin overrides |
| clangd unavailable | Check `PATH` and its configured location; reconfigure after changing LLVM and rebuild to refresh Windows runtime copies |
| Missing symbols or no refresh | Check workspace root, exclusions, warnings, UTF-8 encoding, and the 4 MiB file limit; refresh after cancelling |
| Window/session state will not save | Check permissions, corruption, or external-write conflicts; restart after another instance changes state |
| Corrupt index | Close the app, back up the database and journals together, then reopen/rebuild |
| Unexpected exit | Preserve diagnostics; follow the [crash workflow](DEVELOPER_GUIDE.md#diagnostics-and-crash-investigation) |

### Useful shortcuts

On macOS, use Command instead of Ctrl and Option instead of Alt.
Option+Left/Right is reserved for navigation history.

| Action | Shortcut |
|---|---|
| Activate Project Files | Ctrl+O |
| Open file dialog / open folder | Ctrl+Alt+O / Ctrl+Shift+O |
| Filter the file outline | Alt+L |
| Activate Project Symbols | Ctrl+Shift+S |
| Save / Save As | Ctrl+S / Ctrl+Alt+S |
| Close document | Ctrl+W |
| Next / previous document | Ctrl+Tab / Ctrl+Shift+Tab |
| Jump to definition | Ctrl+= |
| Lookup References | Ctrl+/ |
| Search Project | Ctrl+Shift+F |
| Follow reference / return to Search Results | Ctrl+L |
| First reference | Ctrl+Shift+L |
| Next / previous reference | Shift+F9 / Shift+F8 |
| Navigation back / forward | Alt+Left / Alt+Right |
| Reset source zoom | Ctrl+0 |

Find, editing, and zoom actions otherwise use Qt's standard platform shortcuts.
Editor navigation history and context-preview history are separate.
