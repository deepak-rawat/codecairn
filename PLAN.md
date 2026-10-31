# Cross-platform source browser/editor plan

Historical snapshot: 2026-10-17.

This records decisions and milestone descriptions from earlier phases, not the
current operating state. Use the [developer guide](docs/DEVELOPER_GUIDE.md) for the
implemented architecture and the [README](README.md) for current usage.

## Current delivery: navigation, lookup and language corrections

Startup/example follow-up:

- The application no longer embeds, opens or depends on the typography fixture.
  Fixture resources and helper code belong to tests only.
- No-argument startup restores the last available workspace, with no document
  auto-open. Missing/unavailable workspace state produces an empty editor and
  an explicit status message. Command-line paths take precedence.
- Every language example has companion files with imports/includes, separate
  declarations/implementations, modules or related configuration data.
- Startup tests isolate user settings; multi-file tests check companion files
  and definitions. Available C/C++, Bash, JavaScript, Go and CMake runtimes
  execute the examples; Rust/TypeScript compiler validation remains unverified.

Implemented and regression-tested:

- Real single-click/keyboard navigation in file symbols, project files and
  project symbols; reusable preview tabs; Enter/double-click/edit pinning;
  dirty-buffer preservation and separate Alt/Option+arrow editor history.
- Selected-token normalization, source Ctrl/Command links and plain context
  links, independent asynchronous request channels, and internal header jumps
  without workspace-switch prompts.
- Dedicated panel options, persistent JSON preferences, transient zoom/reset,
  native shortcut labels and focused menu-command enablement.
- Lookup References: semantic LSP references plus separately labeled text
  occurrences, scopes, inclusion options, linked grouped results and cancellation.
- Shared language registry and original fixtures for C/C++, Python, Bash,
  JavaScript/TypeScript/TSX, Go, Rust, JSON, YAML and CMake. Optional non-C servers
  are lazy and report missing dependencies; only clangd is integration-tested.
- Selective libclang recovery of macro-generated functions and thread-local
  declarations, corrected type scopes, local-symbol filtering, schema migration,
  immediate stale-query invalidation and per-revision byte/hash snapshots.

A read-only check of the reported workspace analyzed 163 C/C++ files and
recovered all five specifically missing definitions. Four files still require
syntax recovery and remain visibly partial. No source or database was modified
by that check. Reopen the workspace or rebuild its index to populate the new
schema/extraction results.

Remaining acceptance work: native Linux/macOS execution, additional language
server integration tests, large-project performance certification, incremental
tree reuse, richer semantic/base-type display and fine-grained edited-symbol
markers. The delivered application is not claimed to cover every planned feature.

## Previous delivery: maintainability and configuration

Keep Qt/Scintilla. Package-size optimization is explicitly deferred. The current
work is to make the accepted UI easier to configure and the implementation/build
easier to understand, without moving configuration or I/O into hot paths.

Delivered in this pass:

1. **Typed, versioned appearance configuration.** Reference defaults live in
   [default-settings.json](config/default-settings.json). A per-user JSON file
   is selected through Qt's standard configuration location, with an optional
   `--config` path as agreed with the user. Workspace metadata now lives under
   the per-user state directory's `workspaces/<workspace-hash>/`.
2. **Safe persistence and live application.** Partial overrides, strict
   validation, atomic writes, competing-writer/external-edit detection, and
   last-good appearance on invalid reload. Settings controls support fonts,
   named colors, saving/reloading/exporting and restoring defaults.
3. **Explicit boundaries.** MainWindowSettings handles application of preferences;
   QSettings retains only window state. Shared page/history/worker limits have
   names rather than repeated literals. Painting and parsing do not read JSON.
4. **Simpler CMake ownership.** The root declares application targets;
   Dependencies handles source/system providers, tests have shared setup, and
   deployment remains platform-specific. Downloads can be disabled when explicit
   local Scintilla/C/C++ grammar source trees are supplied.
5. **Regression coverage.** Configuration errors/persistence, preservation of
   text and undo state, renderer consistency, existing workspace/clangd behavior,
   and default/local-source builds are validation gates.

Keep intentional protocol constants, bounded safety limits, icon geometry and
dependency pins in code. Do not make every number independently configurable or
add a generic configuration/plugin framework to remove harmless constants.

Next production-maturity work:

- Native Linux/macOS CI and deployment validation.
- Large-project profiling before changing index/search algorithms.
- Further extraction of document/preview lifecycle responsibilities where it
  reduces real coupling, with stale-result and cancellation tests.
- Schema/config migration policy for future versions.
- Semantic styling/type-preview refinements.
- Package size and dependency trimming only after these priorities.

This is a maintainability/configuration hardening pass, not a claim that the
whole application is fully production-certified. The developer guide documents
the implemented architecture and remaining limits.

## Previous phase: UI parity and workspace functionality

Current implementation update: the user accepted the revised UI. Automatic
workspace indexing, SQLite in per-user workspace storage, refresh/
rebuild/cancel controls, live overrides, filesystem refresh, and clangd definition
previews are now implemented. See [README.md](README.md) for current behavior and
remaining limitations. The older milestone descriptions below are historical.

The [UI guide](docs/DEVELOPER_GUIDE.md#ui-and-interaction-model) documents the current
interaction model. The sections below retain prototype-era architectural
background.

The user wants almost the same controls, density, scaling, and arrangement as
the supplied UI reference, except without docking/MDI windows.
The prototype is a starting point; its UI has not passed visual acceptance.

Implementation update: the compact reference-style UI, tree controls, four
browser tabs, source palette/fonts, and UI regression tests are now implemented.
The user accepts Scintilla's uniform line height; that is no longer a blocker.
See the [developer guide](docs/DEVELOPER_GUIDE.md) for delivered scope and remaining
validation work.

New order:

1. Measure the screenshot/reference settings and match shell, tree/table controls,
   source appearance, and scale.
2. Obtain side-by-side visual acceptance, explicitly resolving Scintilla's
   line-height limitations.
3. Open a folder/file without a create-project workflow and build a complete
   background symbol catalog, with refresh/rebuild/cancel commands.
4. Resolve the function under the caret to its actual definition, not the
   enclosing function.
5. Validate integrated behavior on UCRT64, Linux, and macOS.

Folders and Symbol Categories controls are now in the UI-parity scope. The
permanent typography toolbar and spacious modern chrome are not.

Status: first-milestone Windows UCRT64 prototype implemented; visual acceptance
and Linux/macOS validation remain open. Later milestones are not complete.

## Implementation checkpoint

The [CMake project](CMakeLists.txt) now builds a Qt 6 Widgets application using
upstream Scintilla 5.6.7. See [README.md](README.md) for MSYS2 dependency setup,
build/run commands, limits, and tests.

Implemented:

- Fixed splitters, editable document tabs, read-only/pinnable context preview,
  typography controls, line-height display, and saved UI preferences.
- UTF-8/BOM/line-ending preservation, atomic saves, dirty-document prompts,
  and external-change checks before overwriting open files.
- Background Tree-sitter C/C++ styling and live file outlines to exercise
  typography on actual code rather than mock data.
- Background project file discovery and open-document symbol browsing.
- QtTest/CTest checks for parsing, file I/O, presentation-only styling,
  preview isolation, latest-result handling, navigation, and basic layout.
- Native UCRT64 shell/CMake workflow without launcher scripts or hard-coded
  toolchain paths. Windows builds deploy app-local DLLs/plugins for Explorer
  launch; installs deploy a relocatable Windows directory, a macOS app bundle,
  or a Linux executable/library/plugin tree through native CMake/Qt tools.

Scope distinctions:

- This is M1 plus limited M2/M3 groundwork, not the completed project index.
- Symbol browsing is explicitly open-document-only; SQLite is not implemented.
- Parsing is debounced full-buffer parsing in workers, not incremental tree reuse.
- Initial symbol extraction walks grammar nodes; broader query coverage remains
  planned.
- Context selection is syntax/source-based, not clangd semantic resolution.
- Tests and native launch are verified on Windows UCRT64 only.
- Windows deployment is tested without MSYS2 on PATH. Linux/macOS deployment
  paths are implemented but still require native build and runtime validation.
- The user's selected font settings and approval of the typography
  approximation are still needed to close the M1 gate.

## 1. Goal and current decisions

Build an independent Linux/macOS source editor with configurable source
presentation and coordinated navigation, in a simpler modern shell.

| Area | Current decision |
|---|---|
| Layout | Fixed regions with draggable splitters; no docking or floating panels. |
| Documents | Tabs, not movable/minimizable MDI child windows. |
| UI toolkit | Recommend C++ and Qt 6 Widgets. |
| Editor | Scintilla Qt integration, conditional on a typography prototype. |
| Syntax and symbol extraction | Tree-sitter, initially with C/C++ grammars. |
| Project catalog | Application-owned index persisted in SQLite. |
| Semantic C/C++ resolution | Add clangd when definition/type accuracy is required. |
| Ctags | Not required for the initial design. Reconsider only for specific grammar/coverage gaps. |
| Source fidelity | Aim for the same visual hierarchy and workflow; exact layout equivalence remains unproven. |
| Implementation style | Start with a small vertical slice, not a plugin platform or custom editor engine. |

C/C++ is the proposed first language scope, based on the screenshot. This is not
a decision to exclude other languages permanently.

Superseded parts of the initial plan:

- Remove docking/floating-panel work and MDI window management.
- Replace mandatory Ctags plus Tree-sitter with Tree-sitter plus a project index.
- Keep semantic resolution distinct from syntax extraction.
- Make Scintilla's uniform line-height limitation an explicit acceptance gate.

## 2. Fixed, resizable layout

```text
+---------------------------------------------------------------------+
| Compact application actions and document tabs                       |
+--------------+--------------------------------+---------------------+
| Active-file  |                                | Context preview     |
| symbols      |                                |                     |
|              |          Source editor         +---------------------+
| Filter       |                                | Project Files       |
| Outline      |                                | Project Symbols     |
|              |                                | Search + results    |
+--------------+--------------------------------+---------------------+
| Status: file, position, encoding, project/index state                |
+---------------------------------------------------------------------+
```

Implementation:

- A horizontal `QSplitter` contains the outline, editor area, and right column.
- A vertical `QSplitter` separates context and project browsing in the right
  column.
- Document tabs select the active document and its outline.
- Project Files and Project Symbols share the lower-right tabbed area.
- Use model/view controls, with custom delegates only where needed.
- Save splitter sizes, selected browser tab, and document view state.
- Give panes sensible minimum dimensions and disable accidental collapse.
- Do not add detach, dock, float, or individual minimize/maximize controls.

Suggested initial proportions, estimated from the screenshot rather than
measured requirements: 18% outline, 46% editor, 36% right column; roughly equal
height for context and project browsing. All are user-resizable.

Folders and Symbol Categories are visible in the reference screenshot but are
outside the first five-component vertical slice. They can be added later to the
same browser region without redesigning the layout.

Modernization means consistent spacing, restrained borders, readable tabs,
accessible contrast, and a smaller action surface. It should not discard the
information density or keyboard workflow.

## 3. Why Qt remains the recommendation

Qt Widgets supplies mature desktop layout, tables/trees, focus handling,
clipboard, input methods, high-DPI support, accessibility facilities, and
platform integration. The fixed layout does not require QML or a web runtime.

The original executable is about 3 MB, but that excludes Windows-provided UI
libraries and the rest of the installed application. Its executable size is not
directly comparable to a self-contained cross-platform application bundle.

A Qt bundle will be larger. Since size is not a blocker, prefer predictable
desktop behavior and engineering reliability over minimizing the main binary.
Still measure cold start, resident memory, and installed size separately.

Packaging strategy:

- Start with the required Qt Core/Gui/Widgets modules and platform plugins.
- Avoid Qt WebEngine and unnecessary modules.
- Choose and test the actual Scintilla Qt integration/version early.
- Do not assume QScintilla and Scintilla's own Qt integration have identical
  licensing or release behavior; review the chosen component.
- Review Qt, Scintilla, Tree-sitter grammar, font, and clangd distribution terms.
- Measure release packages on Linux and macOS instead of guessing bundle size.

A lighter toolkit is a fallback only if measured distribution/startup constraints
change; it is not currently a reason to replace Qt.

## 4. Is Tree-sitter enough?

### For the initial browsing feature set: yes, with application indexing

Tree-sitter can supply syntax trees and queries for:

- Current-file declarations and outline structure.
- Syntax-based styling.
- Function, type, member, enum, macro, and typedef extraction where the grammar
  and queries support them.
- Re-extraction after edits, including incomplete code.

Parse project files in the background, extract lightweight symbol records, and
put those records in the project catalog. Tree-sitter does not itself provide a
complete workspace database, file watcher, query UI, or cross-file resolver.

There is no need to maintain Ctags and Tree-sitter pipelines for the same initial
C/C++ scope unless a demonstrated coverage problem warrants it.

### For full C++ semantic behavior: no

A syntax tree alone does not reliably resolve:

- Which overloaded function a call selects.
- Cross-file type aliases, templates, or inheritance.
- Compiler flags, include paths, and build-configuration-specific macros.
- The underlying type of an arbitrary selected expression.
- Accurate references or full semantic token classification.

Add clangd for those capabilities. Without it, still offer the outline, project
lists, syntax styling, and declaration previews. Name-based lookup should expose
multiple candidates and must not pretend to be an authoritative semantic answer.

### One extraction/indexing path

```text
File discovery / changes ----> background Tree-sitter parse
                                         |
                                  extracted symbols
                                         |
                                        SQLite
                                         |
                                  Project Symbol List

Unsaved document edits -----> incremental Tree-sitter parse
                                         |
                              live outline + style captures
                                         |
                             live overlay on project results

Caret / selection ---------> optional clangd request
                                         |
                           resolved target / semantic styles
```

Use one set of language extraction queries for disk and live documents. Reuse
existing grammar/tag/highlight queries where appropriate, with tested
application-specific additions. Supporting a grammar is not automatically
equivalent to a complete, high-quality symbol extractor.

Keep syntax trees for active/recent documents under a memory budget; project
indexing can discard a tree after extracting its records.

Suggested symbol record:

- Internal identity, normalized file identity, language, and kind.
- Display name, qualified/container name, and optional signature.
- Definition/declaration range and name-selection range.
- Parent relationship, extraction source, and content revision.

Do not treat parse-node identities as persistent IDs across edits.

Index correctness requirements:

- Transactional replacement of records for a changed file.
- Dirty-buffer overlays replace, rather than duplicate, disk results.
- File additions, changes, renames, and deletes invalidate appropriate records.
- Grammar/query version changes trigger reindexing.
- A cancelled or older parse cannot overwrite a newer document revision.
- Unsupported files and parse limitations are visible, not silent successes.
- Index progress/errors are surfaced without blocking normal browsing.

## 5. Source appearance with Scintilla

### Expected to be feasible

- Dark backgrounds and similar token color roles.
- Per-style fonts, fractional sizes, boldness, and italics.
- Larger/bolder declaration names than surrounding source.
- Proportional fonts and configurable tab stops.
- Selection, current-line emphasis, margins, and whitespace display.
- The same style palette in the editor and context preview.

Tree-sitter captures can drive syntax styles. clangd semantic tokens can refine
classification where available. Resolve overlapping captures through an explicit
priority policy rather than letting multiple styling systems overwrite each
other unpredictably.

Styles must affect presentation only: no insertion of extra whitespace, modified
source text, document dirty flag, or undo entries just to imitate appearance.

### Known constraint

Stock Scintilla uses a uniform text-line height based on the largest font in its
current styles. Arbitrary per-style above/below-line spacing is not directly
supported.

Consequently, matching mixed font sizes may make ordinary lines more widely
spaced than the reference. Proportional-font whitespace alignment also needs
measurement; tab settings alone are not a proof of equivalence.

Exact font settings cannot be inferred reliably from the screenshot. Capture the
user's selected font/style values before claiming a match. Use fonts that are
available or legally distributable on the target platforms.

### First prototype: typography decision gate

Build the fixed shell with one Scintilla editor and one context-preview instance.
Use a small original C/C++ fixture resembling the constructs in the screenshot:

- Includes and conditional compilation.
- Anonymous structures and typedef aliases.
- Function-pointer typedefs.
- Function declarations and definitions.
- Members, enums, macros, and multiline comments.
- Tabs, spaces, long lines, Unicode, and incomplete edits.

Compare:

1. Colors and font hierarchy.
2. Baseline, line spacing, and visible line count at a fixed viewport size.
3. Indentation with mixed styles and proportional fonts.
4. Selection bounds and caret placement.
5. No clipping of large text or descenders.
6. Preview/editor styling consistency.
7. Linux/macOS font fallback, scaling, and input methods.

Exit with a recorded user decision:

- Accept stock Scintilla's approximation, or
- Investigate a narrowly justified upstream-compatible enhancement, or
- Reconsider the text-view engine if exact variable-line-height fidelity is
  mandatory.

Do not silently reduce the requested appearance or promise exact parity. Do not
start a custom editor or a deep Scintilla fork before this comparison.

## 6. Coordinated context and navigation

Use a small shared context-target representation rather than tightly coupling
each panel to all the others.

Inputs:

- Editor caret or selection.
- Active-file outline selection.
- Project file selection.
- Project symbol selection.

A target identifies an origin, file, optional symbol/range, and document version.
The context controller obtains the best available target and updates a secondary
source view without moving focus.

Rules:

- Selection previews; explicit activation opens/navigates.
- Context preview does not create an editable document tab or normal navigation
  history entry by itself.
- Pinning prevents automatic replacement.
- Preview changes must not feed back into a self-triggering selection loop.
- Unsaved document contents take precedence over disk contents.
- Asynchronous replies carry a selection/document generation and stale replies
  are discarded.
- Errors and unresolved targets have explicit states.
- Preserve previous useful content only with a clear indication if it is stale.
- Handle UTF-8 byte positions, negotiated LSP position encodings, line endings,
  and Unicode boundaries explicitly.

A read-only preview may share an editor document when supported, or read a
versioned snapshot. Validate the chosen mechanism, particularly that read-only
settings do not unintentionally affect the editable view.

## 7. Project browsing and keyboard behavior

Project File List:

- Flattened model with filename and disambiguating directory.
- Keyboard filtering, selection, preview, and explicit open action.
- Keep stable selection when background refreshes do not remove its target.

Project Symbol List:

- Search the complete application catalog, with live document overlays.
- Name fragments match independently of their order.
- Leading `?` enables regex; trailing `(` restricts results to functions.
- Show kind/container/path to distinguish duplicate names.
- Use bounded/cancellable regex evaluation with a visible error for invalid
  patterns; do not freeze the UI.
- Define normalization and ranking rules and test them separately from display.

File Symbol Window:

- Hierarchical declarations from the live buffer.
- Name/line/kind sorting and filters.
- Expand/collapse nodes, source-range navigation, and current-symbol indication.
- Preprocessor structure as syntactic structure, not a claim that a branch is
  active under a particular build configuration.

Keyboard shortcuts should preserve the user's fast panel activation workflow
while respecting macOS conventions. Record exact shortcuts during the behavior
capture rather than assuming every Windows combination transfers unchanged.

## 8. Interaction and typography validation

Use original fixtures to validate visual fidelity and subtle selection behavior.
Broad investigation of other applications is not a prerequisite.

Before or alongside the typography prototype:

1. Record actual font families, sizes, style scaling, fixed-whitespace settings,
   and zoom in the user's normal configuration.
2. Observe a synthetic fixture under different styles and measure line geometry.
3. Record single/double click, Enter, Escape, context lock, and focus behavior.
4. Observe outline and context changes after an unsaved edit.
5. Investigate unexplained differences through the application's drawing,
   context, and interaction tests.

Do not attach a debugger to an active work session merely for completeness.
Use a disposable test project if process inspection is necessary. No additional
runtime inspection has been performed for this plan.

## 9. Milestones and acceptance criteria

### M1. Reference behavior and typography prototype

- Capture relevant style settings and original test fixtures.
- Build Qt/Scintilla on Linux and macOS.
- Implement fixed resizable regions and source/preview views.
- Compare typography and input correctness.

**Exit:** documented acceptance or rejection of Scintilla's appearance,
particularly uniform line height. No full application investment before this
decision.

### M2. Tree-sitter browsing vertical slice

- Add C/C++ grammar integration and tested symbol/highlight extraction.
- Build the active-file outline.
- Discover project files and persist extracted symbols in SQLite.
- Implement Project Files, Project Symbols, and selection-driven context preview.

**Exit:** selecting and activating each kind of target displays and navigates to
the correct file/range. Duplicate-name candidates remain distinguishable.

### M3. Reliable editing and live state

- Connect editor changes to incremental parsing and versioned result delivery.
- Add live project-index overlays, file watching, and consistent save/reload.
- Validate undo/redo, document tabs, navigation history, and context pinning.
- Preserve splitter sizes and document view state.

**Exit:** unsaved declarations appear correctly in outline/search/preview,
removed symbols disappear, stale jobs cannot restore them, and styling never
changes file contents or undo history.

### M4. C/C++ semantic refinement

- Add clangd process lifecycle and LSP document synchronization.
- Support definition/declaration/type resolution and semantic tokens.
- Handle compilation database discovery, missing configuration, and server
  failure explicitly.
- Keep syntax-only browsing available without claiming semantic accuracy.

**Exit:** test overloaded functions, aliases, includes, and selected variable
types using projects with known compilation commands.

### M5. UI polish and platform hardening

- Refine dense modern visuals, keyboard navigation, accessibility, and themes.
- Complete project search modes, outline filters, and preview behavior.
- Validate high DPI, IME, font fallback, path casing, and filesystem changes.
- Package only required dependencies and measure footprint/startup.

**Exit:** usable release builds on Linux and macOS, with recorded known
differences from the reference.

### M6. Large-project validation

Agree on a reference machine and dataset before enforcing performance targets.
Suggested stress fixture: 10,000 files and up to 1,000,000 indexed symbols.

Proposed targets, not measurements of the delivered application:

- Warm project filtering: p95 under 100 ms.
- Warm local context preview: p95 under 150 ms.
- Measure cold semantic resolution separately; it has different dependencies.
- No full-project parse, SQLite scan, or blocking semantic request on the UI
  thread.
- Bounded syntax-tree and preview caches.
- Record indexing duration, peak memory, typing latency, and cold startup.

Test cancellation, rapid typing, rapid selection changes, dirty buffers, deletes,
renames, inaccessible files, invalid regexes, and process failure in addition to
happy-path timing.

## 10. Validation strategy

- Unit tests: extraction queries, ranges/position conversions, fragment search,
  function-only search, ranking, catalog replacement, and stale-result rejection.
- Integration tests: file discovery, index updates, dirty-buffer overlays,
  context navigation, clangd synchronization, and server failure.
- UI tests: focus stability, splitter persistence, keyboard activation, tab
  changes, preview pinning, and editable/read-only separation.
- Visual comparisons: same fixture and viewport with recorded fonts/scaling;
  compare hierarchy and geometry while allowing platform rasterization variance.
- Packaging tests: launch and use the application on clean Linux/macOS systems.

Select thresholds from the prototype and reference project rather than inventing
claims about equivalence from a single screenshot.

## 11. Out of scope for the first implementation

- Docking/floating panels and MDI child-window management.
- Recreating proprietary parsers or database formats.
- Importing third-party executable code, proprietary UI assets, or branding.
- Broad plugin infrastructure.
- A new text-buffer/undo engine.
- Exact pixel identity across operating systems.
- Folder/category tabs, relation/call-tree browsing, debugger integration, and
  other adjacent features until the core five components work.

## 12. Immediate next action

Run and assess **M1: the fixed-layout typography prototype**. Resolve the
Scintilla appearance question first, then finish M2's persistent project
catalog and browsing semantics.

The prototype, basic parser integration, and focused tests are implemented.
No exact visual equivalence, full-project symbol index, semantic resolution,
large-project latency result, or Linux/macOS execution result is claimed.
