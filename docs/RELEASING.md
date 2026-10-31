# Release command reference

Run from the project directory with the [platform dependencies](../README.md#build-and-run)
installed, including clangd. Use a clean, committed worktree for release sources.
Examples use version `0.1.0`; update filenames when the version changes.
Outputs go to `build/releases`, with `.sha256` sidecars.
Builds use all available CPUs by default. Release scripts detect the CPU count;
use `-DJOBS=N` only when intentionally limiting parallelism. For development
builds, use `cmake --build build --parallel`.

## macOS: two installation options

| Option | Dependencies | `cairn` on PATH |
|---|---|---|
| Standalone app ZIP | Bundled | Manual setup |
| Source-only Homebrew formula | Homebrew-managed | Automatic |

Apple Silicon/macOS 26+ only. Apps are ad-hoc signed, **not Developer ID signed
or notarized**. Only approve trusted downloads through macOS's per-app
**Privacy & Security > Open Anyway** flow.

### Build the standalone ZIP

```sh
cmake -DTARGET=macos-app -P scripts/Release.cmake
```

Produces `build/releases/codecairn-0.1.0-macos26-arm64-unsigned.zip`.
Extract it and move `codecairn.app` into Applications. Do not overwrite an existing
installation without checking it first.

Optional: expose its launcher, provided that link does not already exist:

```sh
mkdir -p "$HOME/.local/bin"
ln -s /Applications/codecairn.app/Contents/MacOS/cairn "$HOME/.local/bin/cairn"
```

Ensure `$HOME/.local/bin` is on PATH. No Homebrew runtime dependencies are needed.

## Source archive and package recipes

```sh
cmake -P scripts/PrepareSource.cmake
cmake -P scripts/PrepareRecipes.cmake
```

Run in this order:

- `PrepareSource.cmake`: creates `build/releases/codecairn-0.1.0-source.tar.gz`
  containing CodeCairn and pinned Scintilla/grammar sources. Requires a clean,
  committed worktree by default.
- `PrepareRecipes.cmake`: uses that archive to generate the Homebrew formula and
  MSYS2/Arch recipes with the version, download URL, and source checksum.

Recipe downloads default to
`https://github.com/deepak-rawat/codecairn/releases/download/codecairn-v<VERSION>`.
Use `-DRELEASE_URL=<base-url>` to override the download location.

Neither command compiles or installs CodeCairn, creates the app ZIP, or publishes anything.
For uncommitted local snapshots only, add `-DALLOW_DIRTY=ON` to source preparation;
do not publish those snapshots.

## Homebrew

### Prepare the generic local tap

This copies the source archive to a stable location and generates the formula.
`COMMIT_TAP=ON` commits only that formula in a separate tap repository.
It does **not** register the tap, install software, or commit the project.

```sh
cmake -DARCHIVE="$PWD/build/releases/codecairn-0.1.0-source.tar.gz" \
  -DCOMMIT_TAP=ON -P scripts/PrepareLocalTap.cmake
```

Default layout; other packages can share the same tap:

```text
~/local-releases/
    tap/Formula/codecairn.rb
    artifacts/codecairn/         # Source archives and checksums
```

Override the location with `-DLOCAL_ROOT=<absolute-directory>`.
Keep these directories; they do not depend on retaining the project build tree.
Local edits and different same-name archives are protected from overwriting.

### Register the local tap

```sh
brew tap local/releases "$HOME/local-releases/tap"  # Register once
```

Registration does not install CodeCairn.
If you already use the older `codecairn/local` tap, check that installation
before switching; preparation does not migrate or unregister it.

### Install from the prepared local tap

Update Homebrew's cloned tap before installing the source-only formula:

```sh
brew update
brew install --build-from-source local/releases/codecairn
cairn --version
cairn .
```

Homebrew installs dependencies, compiles CodeCairn, runs the formula's checks,
and exposes `cairn` on PATH. Dependency packages can still use Homebrew's own
precompiled packages; CodeCairn itself is always built from source.

Homebrew extracts and builds sources in a temporary directory, normally under
macOS's temporary location (`/private/tmp` or `/private/var/folders/...`), then
cleans that work after installation. `HOMEBREW_TEMP` can override the location.
It caches downloaded source archives separately in its cache directory.
The installed app lives at `$(brew --prefix codecairn)/codecairn.app`, inside
Homebrew's Cellar (normally `/opt/homebrew/Cellar/codecairn/0.1.0` on Apple Silicon).
This does not use or alter the project's `build` directory.

Regenerating the local tap replaces a committed older formula with the
source-only formula; existing archive files are preserved. Update Homebrew's
cloned tap afterward. Package generation does not reinstall an existing app.
Never publish formulas containing local `file://` URLs.

### Update or remove an installation

After preparing a new version, update Homebrew's cloned tap and upgrade:

```sh
brew update
brew upgrade local/releases/codecairn
```

```sh
brew uninstall local/releases/codecairn
```

## Windows portable ZIP

Use the MSYS2 UCRT64 shell:

```sh
cmake -DTARGET=windows-zip -P scripts/Release.cmake
```

For the ZIP plus MSYS2 package:

```sh
cmake -DTARGET=windows-all -P scripts/Release.cmake
```

`windows-all` may install build dependencies through pacman. Extract the complete
ZIP; run `./bin/codecairn.exe` or add its `bin` directory to PATH for `cairn`.

## MSYS2 and Arch packages

After source/recipe preparation, build and optionally install on the matching OS:

| Platform | Build directory | Build command | Install command |
|---|---|---|---|
| MSYS2 UCRT64 | `build/releases/msys2` | `makepkg-mingw --syncdeps --noconfirm --cleanbuild` | `pacman -U ./mingw-w64-ucrt-x86_64-codecairn-*.pkg.tar.zst` |
| Arch Linux x64 | `build/releases/arch` | `makepkg --syncdeps --cleanbuild` | `sudo pacman -U ./codecairn-*.pkg.tar.zst` |

Do not run makepkg as root. These packages use system-managed dependencies.

## Clean release work directories

Successful local-tap preparation deletes its temporary staging directory.
Failed/incomplete runs stay available for diagnosis.

```sh
cmake -P scripts/PruneReleaseWork.cmake             # Preview only
cmake -DAPPLY=ON -P scripts/PruneReleaseWork.cmake   # Remove older successful work
```

Pruning keeps the latest successful build **per release target**, including any
matching symbols, and retains failed/incomplete runs. Final archives, the stable
tap, and shared dependency caches are protected. Unmarked directories created
before this policy are preserved because their status cannot be verified.
Use `-DWORK_ROOT=<directory>` only for a different release-work root.

## Publishing

- Update the version in `CMakeLists.txt`; release tags are `codecairn-v0.1.0`.
- The [release workflow](../../.github/workflows/codecairn-release.yml) accepts
  manual dispatch; matching tags create a draft release after required jobs pass.
- Before publishing, verify native installation/startup, dependency notices and
  source obligations. Retain matching build directories and symbols.
- Tap/AUR publication is separate; local scripts do not publish or push anything.
