# Building packages and releasing

CMake defines the installed distribution and CPack creates standalone packages.
Scripts in [packaging](../packaging) prepare sources, recipes, and release assets;
they do not build the application, install dependencies, or publish releases.
GitHub Actions owns the build/test/package pipeline and draft publication.
Standalone releases do not wait for managed-package or performance validation.

Run commands from the project directory using Bash/zsh, including MSYS2 UCRT64
on Windows. Install the [platform dependencies](../README.md#build-and-run),
including clangd, first. Release inputs must be clean and committed. The version
in [CMakeLists.txt](../CMakeLists.txt) must match `codecairn-vMAJOR.MINOR.PATCH`.

## Standalone packages

Use a separate build directory for each compiler, platform, and dependency mode.
Builds use all available CPUs with `--parallel`.

### macOS 26+ ARM64

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCODECAIRN_RELEASE=ON -DCODECAIRN_BUNDLE_RUNTIME=ON -DBUILD_TESTING=ON \
  "-DCMAKE_PREFIX_PATH=$(brew --prefix qt);$(brew --prefix tree-sitter);$(brew --prefix sqlite);$(brew --prefix re2);$(brew --prefix llvm)"
```

Release builds target macOS 26 and ARM64; dependencies must also support that
minimum OS. Installing newer dependencies does not lower their deployment target.

### Windows 11 x64

Use the MSYS2 UCRT64 shell:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DCODECAIRN_RELEASE=ON -DCODECAIRN_BUNDLE_RUNTIME=ON -DBUILD_TESTING=ON
```

### Ubuntu 24.04+ x64 Linux

The release workflow builds on Ubuntu 24.04 with Qt 6.8.3. Install that Qt SDK
(including Core5Compat) and the Ubuntu development packages:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config patchelf \
  libsqlite3-dev libre2-dev clang clangd libclang-dev llvm-dev \
  libgl1-mesa-dev libxkbcommon-x11-0 libxcb-cursor0 fonts-dejavu-core
cmake "-DOUTPUT=$PWD/build/linux-deps" -P cmake/PrepareLinuxDependencies.cmake
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  "-DCMAKE_PREFIX_PATH=$QT_ROOT_DIR;$PWD/build/linux-deps" \
  -DCODECAIRN_RELEASE=ON -DCODECAIRN_BUNDLE_RUNTIME=ON \
  -DBUILD_TESTING=ON -DCODECAIRN_BUILD_BENCHMARKS=OFF
```

Set `QT_ROOT_DIR` to the Qt SDK prefix. Dependency preparation builds hash-pinned
Tree-sitter 0.25.10 privately because Ubuntu's default runtime is too old for the
grammar ABI. It also prepares runtime license notices; it does not install
system packages. General Linux bundling requires Qt 6.7+ and patchelf.

### Build, test, package

After the matching platform configure:

```sh
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure
cpack --config build/release/CPackConfig.cmake -B build/releases/standalone
```

CPack invokes installation into its private staging area, including runtime
deployment, dependency notices, build provenance, and macOS ad-hoc signing.
It produces a `.sha256` sidecar for each package.

For version `0.1.0`, the standalone outputs are:

| Platform | Output |
|---|---|
| macOS | `codecairn-0.1.0-macos26-arm64-unsigned.dmg` |
| Windows | `codecairn-0.1.0-windows11-x64.zip` |
| Linux | `codecairn-0.1.0-linux-ubuntu24.04-x64.tar.gz` |

Open the DMG and drag `codecairn.app` into Applications. Do not overwrite an
existing installation without checking it first. The app bundles its runtime
dependencies and clangd. It is ad-hoc signed, **not Developer ID signed or
notarized**. Only approve trusted downloads through the per-app
**Privacy & Security > Open Anyway** flow.

To expose the macOS launcher, first check that the destination link does not
already exist:

```sh
mkdir -p "$HOME/.local/bin"
ln -s /Applications/codecairn.app/Contents/MacOS/cairn "$HOME/.local/bin/cairn"
```

Keep `$HOME/.local/bin` on PATH. The standalone app does not install a launcher
outside its bundle.

Extract the entire Windows ZIP and run `./bin/codecairn.exe`, or add its `bin`
directory to PATH for `cairn`. Keep all DLLs, plugins, configuration, and resource
headers together. Save and close an existing running app before replacing it.

Extract the whole Linux TAR and run `./bin/cairn`, or add its `bin` directory to
PATH. It bundles Qt, parser/compiler libraries, clangd and Clang resource headers.
Keep `bin`, `lib`, `plugins`, configuration and notices together. The supported
baseline is Ubuntu 24.04 x64 or newer, not every Linux distribution: glibc,
graphics drivers, X11/XWayland display services and installed fonts still come
from the host.

## Source archive and managed recipes

```sh
cmake -P packaging/PrepareSource.cmake
cmake -P packaging/PrepareRecipes.cmake
```

Source preparation stages Git-scoped project files plus hash-pinned Scintilla
and grammar sources, their licenses, and a revision manifest. It uses CPack TGZ
to produce `build/releases/codecairn-0.1.0-source.tar.gz` and its checksum.
System development dependencies are still required; package recipes disable
source downloads during the build.

`PrepareRecipes.cmake` uses that source archive to generate:

- `build/releases/homebrew/Formula/codecairn.rb`
- `build/releases/msys2/PKGBUILD`
- `build/releases/arch/PKGBUILD`

Native recipe directories also receive a local source-archive copy.
Arch `.SRCINFO` is generated by `makepkg --printsrcinfo`, not maintained separately.

Public recipe URLs use
`https://github.com/deepak-rawat/codecairn/releases/download/codecairn-v<VERSION>`.
Override `RELEASE_URL` only for a deliberate alternate download location.
For uncommitted local source snapshots, use `-DALLOW_DIRTY=ON` with
`PrepareSource.cmake`; never publish those snapshots or use them for release
standalone packages. Existing different same-name source archives are rejected.

### Homebrew local tap

Homebrew builds CodeCairn from source with system-managed dependencies. There
are no bottles or casks.

```sh
cmake -DCOMMIT_TAP=ON -P packaging/PrepareLocalTap.cmake
brew tap local/releases "$HOME/local-releases/tap"
brew update
brew install --build-from-source local/releases/codecairn
brew test local/releases/codecairn
cairn --version
```

Preparation stores the archive under `~/local-releases/artifacts/codecairn` and
the formula under `~/local-releases/tap/Formula`. Override `LOCAL_ROOT` to use a
different location. It protects modified formulas and differing same-name
archives, preserves other packages, and does not invoke Homebrew.
`COMMIT_TAP=ON` commits only the formula in that separate tap repository.

Homebrew compiles in its temporary build area, installs the app into its Cellar,
and exposes `cairn` through its bin directory; it does not build in this checkout.
The formula ad-hoc signs the managed app in `post_install`, after Homebrew's
binary fixups, and `brew test` verifies that installed signature.
After preparing an updated formula, run `brew update` and
`brew upgrade local/releases/codecairn`. To remove it, run
`brew uninstall local/releases/codecairn`. Never publish a formula with `file://`
download URLs.

### MSYS2 and Arch

Run the matching command after source and recipe preparation:

| Platform | Working directory | Build command |
|---|---|---|
| MSYS2 UCRT64 | `build/releases/msys2` | `makepkg-mingw --syncdeps --noconfirm --cleanbuild` |
| Arch Linux x64 | `build/releases/arch` | `makepkg --syncdeps --cleanbuild` |

Do not run makepkg as root. Install the resulting package with `pacman -U`
(using `sudo` on Arch). These packages use system dependencies and must not
contain private runtime libraries, Qt configuration, or copied Clang headers.

## GitHub Actions

The [build workflow](../.github/workflows/codecairn-build.yml) runs only through
manual dispatch or a call from the release workflow, not on branch pushes or
pull requests. It prepares sources, builds/tests Windows, macOS and Ubuntu Linux,
and produces standalone CPack packages only when called with a validated release
tag. Regular functional tests remain enabled; benchmarks are disabled in this
workflow.

The [release workflow](../.github/workflows/codecairn-release.yml) runs for
`codecairn-v*` tags or manual selection of an existing tag:

1. Validate the tag against the version and resolve its immutable commit.
2. Call the build workflow for that revision.
3. Check the extracted Windows ZIP's platform plugin and application/launcher
   versions with MSYS2 removed from PATH.
4. Assemble the Windows ZIP, macOS DMG, Linux TAR, source archive, recipe archive
   and `SHA256SUMS`.
5. Recheck that the tag has not moved and create a **draft** GitHub release.

Release-asset assembly rejects missing, empty, duplicate, unexpected, or
checksum-mismatched packages, incorrect source URLs/revisions, and dirty sources.
Recipe archives include the Homebrew formula and both PKGBUILDs; native managed
packages and build-generated `.SRCINFO` are not draft assets. Public download
URLs work after publication. macOS packaging still ad-hoc signs and verifies the
app, but the release workflow does not mount/test the DMG. Linux TAR installation
and native GUI launch must also be checked manually before publication.

The [managed workflow](../.github/workflows/codecairn-managed.yml) separately
builds/tests and installs Homebrew, MSYS2 and Arch packages. Select a branch, tag
or commit through its `source-ref` input; it resolves one immutable revision for
all jobs. Run it against the exact release tag before distributing managed
recipes/packages. Its native packages, checksums and `.SRCINFO` are workflow
artifacts, not automatic release assets.

The [performance workflow](../.github/workflows/codecairn-performance.yml) runs
benchmark fixtures separately on all three platforms and preserves measurements.
Both workflows are manual and neither blocks standalone draft creation.

Before publishing manually, verify native installation/startup and license/source
obligations. Offscreen tests are not native interaction certification.
Tap/AUR publication remains separate.

## Build outputs and diagnostics

Build trees are not automatically pruned. Retain matching binaries, symbols,
source revision, and diagnostic logs when investigating failures. CI preserves
failure logs and matching application symbols as workflow artifacts; download
them before their GitHub retention period expires.

Source, recipe, and local-tap scripts remove only their own temporary staging
directory after success. Failed staging remains for diagnosis. Final packages
live under `build/releases`; the stable local tap lives outside the build tree.
