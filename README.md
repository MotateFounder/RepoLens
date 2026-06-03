# RepoLens

RepoLens is a local, cross-platform repository context retrieval engine for AI-assisted programming workflows.

This repository is RepoLens v1.0: a C++20/CMake command-line tool with standalone release builds.
Current implemented commands:

```text
repolens --help
repolens --version
repolens init <repo_path> --index-dir <index_path>
repolens status --index-dir <index_path>
repolens update --index-dir <index_path> [--format text|json] [--quiet] [--verbose] [--no-progress]
repolens updateroot
repolens diagnostics --index-dir <index_path>
repolens search --index-dir <index_path> --query <text> [--kind <kind>] [--limit <n>] [--partial] [--format text|json]
repolens context --index-dir <index_path> --symbols "A,B,C" --format json [--partial] [--basic] [--budget-chars <n>] [--include-tree] [--include-types]
repolens serve --index-dir <index_path> --port 7123
repolens enrich --index-dir <index_path> --changed-only
```

`init` writes only to the external index directory. It creates `<index_path>/repolens.db` and stores repository metadata in SQLite.
`update` recursively scans repository files, ignores common build/dependency folders, and records added, modified, deleted, and unchanged file counts in the external SQLite index.
In normal text mode, `update` prints a terminal progress report with repository details, scan counts, a processed-files progress bar, current file, running totals, elapsed time, database growth, warnings, and final index statistics. Use `--quiet` or `--no-progress` for script-friendly text output. Use `--format json` for valid JSON only; live progress text is suppressed in JSON mode.
`diagnostics` reports SQLite database size and row counts for repositories, files, symbols, symbol parameters, symbol relations, snapshots, and changes. It is useful for explaining index growth after updates.
`updateroot` reads `include.txt` and optional `exclude.txt` from the same directory as the `repolens` executable, stores `repolens.db` in that executable directory, and updates only the included paths that are not excluded. Each non-empty line in those files is a quoted or unquoted absolute file or directory path.
The update pipeline includes interpreters for `.cs`, `.csproj`, `.xaml`, C/C++ source/header files, common C/C++ build files, SQL, Docker/YAML/Terraform infrastructure files, Swift/Objective-C files, R/RMarkdown files, JSON, Python source/config files, Java/JVM files, Go files, Rust files, PHP files, Ruby files, shell/PowerShell files, MATLAB/Octave/Scilab files, and the JavaScript/web ecosystem. C# extraction covers namespaces, types, methods, constructors, properties, and fields. C/C++ extraction covers namespaces, classes, structs, enums, functions, fields, and macros. SQL extraction covers `.sql` tables, views, indexes, procedures, functions, triggers, schemas, and migration files. DevOps extraction covers `Dockerfile`, Docker Compose/YAML, `.yaml`, `.yml`, `.tf`, and `.tfvars` instructions, services, properties, Terraform resources, data, modules, variables, outputs, and providers. Swift/Objective-C extraction covers `.swift`, `.m`, `.mm`, and `.h` imports, types, methods/functions, properties, and Objective-C `.m` files are detected without breaking MATLAB `.m` parsing. R extraction covers `.R`, `.r`, and `.Rmd` packages, functions, variables, and RMarkdown chunks. Java/JVM extraction covers `.java`, `.kt`, `.kts`, `.gradle`, and `pom.xml` packages, imports, classes, interfaces, enums, records, objects, methods/functions, constructors, fields/properties, Maven dependencies, Gradle dependencies, plugins, and tasks. Go extraction covers `.go`, `go.mod`, and `go.sum` packages, imports, structs, interfaces, functions, methods, variables, modules, and dependencies. Rust extraction covers `.rs`, `Cargo.toml`, and `Cargo.lock` modules, uses, structs, enums, traits, impl blocks, functions, methods, constants, macros, packages, and dependencies. PHP extraction covers `.php` and `composer.json` namespaces, imports, classes, interfaces, traits, enums, functions, methods, constructors, properties, constants, Composer packages, dependencies, and autoload metadata. Ruby extraction covers `.rb`, `Gemfile`, and `.gemspec` modules, classes, methods, constants, attributes, Rails routes, gemspec names, and gem dependencies. Shell/PowerShell extraction covers `.sh`, `.bash`, `.zsh`, `.ps1`, `.psm1`, and `.psd1` functions, aliases, exports, sourced files, commands, variables, param blocks, and module manifest properties. Python extraction covers `.py`, `.pyw`, and `.pyi` imports, classes, functions, methods, async functions/methods, variables, fields, requirements files, `pyproject.toml`, `Pipfile`, `poetry.lock`, and common INI-style Python tool configs. MATLAB/Octave/Scilab extraction covers `.m`, `.mlx`, `.sci`, `.sce`, `.tst`, and `.dem` classes, functions, methods, properties, script variables, sections, and Scilab `deff` functions. Project/XAML/build extraction captures project references, package references, XAML classes, named XAML elements, CMake targets, Makefile targets, solution projects, Visual C++ project files, and JUCE `.jucer` metadata. Web extraction covers JavaScript/TypeScript functions, classes, methods, React components/hooks, Angular decorated classes, Vue SFC template/script/style symbols, HTML ids/classes, CSS selectors/keyframes/custom properties, generic JSON objects/properties, and `package.json` scripts/dependencies.
`search` performs deterministic lookup over active symbols and files, with optional kind filtering and text or JSON output.
`context` builds a JSON package with matching symbols, source snippets, warnings, budget information, an optional reduced file tree, and optional related type definitions. Use `--partial` with `context` to match symbol names, qualified names, or signatures by substring, such as finding `InitTuningStages(...)` with `--symbols "Tuning" --partial`. Use `--basic` to return only file, line range, and code for every match.
`serve` exposes the same update, search, context, and status behavior through a local HTTP API bound to `127.0.0.1`.
`enrich` optionally fills symbol description/tag metadata from an OpenAI-compatible endpoint configured in `<index_path>/config.json`.

Example AI config:

```json
{
  "ai": {
    "enabled": false,
    "provider": "openai-compatible",
    "endpoint": "http://127.0.0.1:1234/v1/chat/completions",
    "model": "local-model",
    "api_key_env": "REPOLENS_API_KEY"
  }
}
```

## Local API

```text
GET  /health
GET  /status
POST /update
POST /search
POST /context
```

## Build

The default build vendors SQLite through the SQLite amalgamation in `third_party/sqlite`, so the produced RepoLens binary does not require a separate SQLite installation for core features.

Build-time requirements:

- CMake 3.16 or newer.
- A C++20 compiler.
- A C compiler for the bundled SQLite amalgamation.

Runtime requirements for core features:

- The produced `repolens` binary.
- No separate SQLite, Python, Node.js, .NET, Java, or external database server.

Runtime requirements for optional features:

- `serve` and `enrich` use normal operating-system TCP sockets.
- `enrich` needs only a configured HTTP endpoint when AI is enabled; no local language runtime is required by RepoLens itself.

Useful CMake options:

```text
REPOLENS_BUILD_STANDALONE=ON
REPOLENS_ENABLE_AI=ON
REPOLENS_ENABLE_API=ON
REPOLENS_USE_SYSTEM_SQLITE=OFF
```

Set `REPOLENS_USE_SYSTEM_SQLITE=ON` only when you intentionally want RepoLens to load a system SQLite runtime instead of compiling the bundled amalgamation into the executable.

### One-command installers

The installer scripts check for the build-time dependencies, prompt before installing anything, build a standalone release, copy only the final binary into `release`, and remove the generated `build` folder afterward.

```bash
bash install_Windows11.sh
bash install_Windows10.sh
bash install_Linux.sh
bash install_macOS.sh
```

On Windows, run the script from Git Bash or another Bash-compatible shell. The Windows scripts use `winget` when the user agrees to install missing dependencies.

The top-level `CMakeLists.txt` intentionally stays in the repository root because it is the standard CMake project entry point.

### Windows

Manual build:

```powershell
cmake -S . -B build -DREPOLENS_BUILD_STANDALONE=ON -DREPOLENS_USE_SYSTEM_SQLITE=OFF
cmake --build build --config Release
```

Run:

```powershell
.\build\Release\repolens.exe --help
.\build\Release\repolens.exe --version
```

### Linux and macOS

Manual build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DREPOLENS_BUILD_STANDALONE=ON -DREPOLENS_USE_SYSTEM_SQLITE=OFF
cmake --build build
```

Run:

```bash
./build/repolens --help
./build/repolens --version
```

## Release Verification

After building a release binary, verify it without relying on development tools:

```powershell
.\build\Release\repolens.exe --version
.\build\Release\repolens.exe init <repo_path> --index-dir <external_index_path>
.\build\Release\repolens.exe status --index-dir <external_index_path>
.\build\Release\repolens.exe update --index-dir <external_index_path>
.\build\Release\repolens.exe search --index-dir <external_index_path> --query <symbol>
.\build\Release\repolens.exe context --index-dir <external_index_path> --symbols "<symbol>" --format json
```

On Linux/macOS, use `./build/repolens` or the path produced by your generator.

For a dependency check:

- Windows: use `dumpbin /dependents repolens.exe` from a Visual Studio Developer Command Prompt.
- Linux: use `ldd ./repolens`.
- macOS: use `otool -L ./repolens`.

With the default standalone options, SQLite should not appear as an external runtime dependency.
