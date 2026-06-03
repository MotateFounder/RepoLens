<!--
  RepoLens README
  Retro terminal-inspired GitHub format
-->

<div align="center">

```text
  ____                 _                    
 |  _ \ ___ _ __   ___| |    ___ _ __  ___ 
 | |_) / _ \ '_ \ / _ \ |   / _ \ '_ \/ __|
 |  _ <  __/ |_) |  __/ |__|  __/ | | \__ \
 |_| \_\___| .__/ \___|_____\___|_| |_|___/
           |_|                               
```

# RepoLens

**A local, cross-platform repository context retrieval engine for AI-assisted programming workflows.**

`C++20` · `CMake` · `SQLite` · `CLI-first` · `Read-only indexing` · `Standalone builds`

</div>

---

## $ whatis repolens

RepoLens is a small terminal-based engine that indexes source repositories into an **external SQLite database** and retrieves precise code context for AI coding assistants.

It is designed for workflows where an AI assistant should not receive the entire repository, but should receive the **right files, symbols, snippets, line ranges, and related context**.

```text
user/code selection
        │
        ▼
AI coding app asks RepoLens for context
        │
        ▼
RepoLens searches its local SQLite index
        │
        ▼
RepoLens returns a compact context package
        │
        ▼
local/remote AI gets better context
```

RepoLens v1.0 is a **C++20/CMake command-line tool** with standalone release builds. It writes only to the configured external index directory and leaves the target repository untouched.

---

## $ status

```text
[OK] Local-first repository indexer
[OK] External SQLite database
[OK] Read-only target repository access
[OK] Terminal progress reports
[OK] Deterministic search
[OK] Context package generation
[OK] Local HTTP API
[OK] Optional OpenAI-compatible enrichment
[OK] Standalone release builds
```

---

## $ commands

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

---

## $ quickstart

### 1. Create an external index

```bash
repolens init /path/to/repo --index-dir /path/to/repolens-index
```

`init` creates:

```text
/path/to/repolens-index/repolens.db
```

and stores repository metadata in SQLite.

RepoLens does **not** write inside `/path/to/repo`.

### 2. Scan and index the repository

```bash
repolens update --index-dir /path/to/repolens-index
```

In normal text mode, `update` prints a terminal progress report:

```text
RepoLens Update
---------------
Repo root:   /path/to/repo
Index dir:   /path/to/repolens-index
Database:    /path/to/repolens-index/repolens.db

Processing files:
[##########------------------------------] 25%  854 / 3419

Current file:
src/ViewModels/SystemCalibrationPanelViewModel.cs

Running totals:
Added:      12
Modified:   4
Deleted:    1
Unchanged: 837
Parsed:    16
Failed:     0
```

Use script-friendly output when needed:

```bash
repolens update --index-dir /path/to/repolens-index --quiet
repolens update --index-dir /path/to/repolens-index --format json
```

### 3. Search the index

```bash
repolens search --index-dir /path/to/repolens-index --query Calibration
```

Use filters:

```bash
repolens search --index-dir /path/to/repolens-index --query AutoTune --kind method --limit 20
repolens search --index-dir /path/to/repolens-index --query Tuning --partial --format json
```

### 4. Build an AI context package

```bash
repolens context \
  --index-dir /path/to/repolens-index \
  --symbols "SystemCalibrationPanelViewModel,AutoTune" \
  --budget-chars 12000 \
  --include-tree \
  --format json
```

The context package may include:

```text
- matching symbols
- source snippets
- file paths
- line ranges
- signatures
- warnings
- budget information
- reduced file tree
- optional related type definitions
```

Use substring matching:

```bash
repolens context --index-dir /path/to/index --symbols "Tuning" --partial --format json
```

Use compact output:

```bash
repolens context --index-dir /path/to/index --symbols "AutoTune" --basic --format json
```

---

## $ how-it-works

```text
+-------------------+       +--------------------+       +--------------------+
| target repository | ----> | RepoLens scanner   | ----> | external SQLite DB |
+-------------------+       +--------------------+       +--------------------+
          ^                            |                            |
          |                            v                            v
   read-only access          language interpreters          search/context API
```

RepoLens keeps the repository clean:

```text
Target repository:  read only
Index directory:    repolens.db, config, cache, diagnostics
```

`update` recursively scans repository files, ignores common build/dependency folders, and records added, modified, deleted, and unchanged file counts in the external SQLite index.

`diagnostics` reports SQLite database size and row counts for repositories, files, symbols, symbol parameters, symbol relations, snapshots, and changes. It is useful for explaining index growth after updates.

`updateroot` reads `include.txt` and optional `exclude.txt` from the same directory as the `repolens` executable, stores `repolens.db` in that executable directory, and updates only the included paths that are not excluded. Each non-empty line in those files is a quoted or unquoted absolute file or directory path.

---

## $ supported-languages

RepoLens includes interpreters for many common code and project formats.

| Ecosystem | Extracted information |
|---|---|
| C# / .NET | namespaces, types, methods, constructors, properties, fields, `.csproj`, `.xaml` |
| C / C++ / JUCE | namespaces, classes, structs, enums, functions, fields, macros, CMake, Makefiles, VC++ projects, `.jucer` metadata |
| SQL | tables, views, indexes, procedures, functions, triggers, schemas, migrations |
| DevOps | Dockerfile, Docker Compose/YAML, Terraform resources, data, modules, variables, outputs, providers |
| Swift / Objective-C | imports, types, methods/functions, properties |
| R / RMarkdown | packages, functions, variables, chunks |
| Java / JVM | packages, imports, classes, interfaces, enums, records, objects, methods/functions, constructors, fields/properties, Maven/Gradle metadata |
| Go | packages, imports, structs, interfaces, functions, methods, variables, modules, dependencies |
| Rust | modules, uses, structs, enums, traits, impl blocks, functions, methods, constants, macros, packages, dependencies |
| PHP | namespaces, imports, classes, interfaces, traits, enums, functions, methods, constructors, properties, constants, Composer metadata |
| Ruby | modules, classes, methods, constants, attributes, Rails routes, gem dependencies |
| Shell / PowerShell | functions, aliases, exports, sourced files, commands, variables, param blocks, module manifest properties |
| Python | imports, classes, functions, methods, async symbols, variables, fields, requirements, `pyproject.toml`, `Pipfile`, `poetry.lock`, tool configs |
| MATLAB / Octave / Scilab | classes, functions, methods, properties, script variables, sections, Scilab `deff` functions |
| Web / JS / TS | functions, classes, methods, React components/hooks, Angular decorated classes, Vue SFC symbols, HTML ids/classes, CSS selectors/keyframes/custom properties, JSON objects/properties, `package.json` scripts/dependencies |

Objective-C `.m` files are detected without breaking MATLAB `.m` parsing.

---

## $ local-api

Start the local API server:

```bash
repolens serve --index-dir /path/to/repolens-index --port 7123
```

RepoLens binds to:

```text
127.0.0.1
```

Available endpoints:

```text
GET  /health
GET  /status
POST /update
POST /search
POST /context
```

The API exposes the same update, search, context, and status behavior as the CLI.

---

## $ ai-enrichment

`enrich` can optionally fill symbol descriptions and tags from an OpenAI-compatible endpoint configured in:

```text
<index_path>/config.json
```

Example config:

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

Run enrichment only when needed:

```bash
repolens enrich --index-dir /path/to/repolens-index --changed-only
```

Core indexing, search, and context retrieval do **not** require AI.

---

## $ build

The default build vendors SQLite through the SQLite amalgamation in:

```text
third_party/sqlite
```

The produced RepoLens binary does not require a separate SQLite installation for core features.

### Build-time requirements

```text
[REQ] CMake 3.16 or newer
[REQ] C++20 compiler
[REQ] C compiler for bundled SQLite amalgamation
```

### Runtime requirements for core features

```text
[REQ] produced repolens binary
[OK]  no separate SQLite install
[OK]  no Python runtime
[OK]  no Node.js runtime
[OK]  no .NET runtime
[OK]  no Java runtime
[OK]  no external database server
```

### Runtime requirements for optional features

```text
serve   -> operating-system TCP sockets
enrich  -> configured HTTP endpoint when AI is enabled
```

RepoLens itself does not require a local language runtime for AI enrichment.

---

## $ cmake-options

```text
REPOLENS_BUILD_STANDALONE=ON
REPOLENS_ENABLE_AI=ON
REPOLENS_ENABLE_API=ON
REPOLENS_USE_SYSTEM_SQLITE=OFF
```

Set `REPOLENS_USE_SYSTEM_SQLITE=ON` only when you intentionally want RepoLens to load a system SQLite runtime instead of compiling the bundled amalgamation into the executable.

---

## $ install-scripts

The installer scripts check for build-time dependencies, prompt before installing anything, build a standalone release, copy only the final binary into `release`, and remove the generated `build` folder afterward.

```bash
bash install_Windows11.sh
bash install_Windows10.sh
bash install_Linux.sh
bash install_macOS.sh
```

On Windows, run the script from Git Bash or another Bash-compatible shell. The Windows scripts use `winget` when the user agrees to install missing dependencies.

The top-level `CMakeLists.txt` intentionally stays in the repository root because it is the standard CMake project entry point.

---

## $ manual-build

### Windows

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

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DREPOLENS_BUILD_STANDALONE=ON -DREPOLENS_USE_SYSTEM_SQLITE=OFF
cmake --build build
```

Run:

```bash
./build/repolens --help
./build/repolens --version
```

---

## $ release-verification

After building a release binary, verify it without relying on development tools:

### Windows

```powershell
.\build\Release\repolens.exe --version
.\build\Release\repolens.exe init <repo_path> --index-dir <external_index_path>
.\build\Release\repolens.exe status --index-dir <external_index_path>
.\build\Release\repolens.exe update --index-dir <external_index_path>
.\build\Release\repolens.exe search --index-dir <external_index_path> --query <symbol>
.\build\Release\repolens.exe context --index-dir <external_index_path> --symbols "<symbol>" --format json
```

### Linux/macOS

Use `./build/repolens` or the path produced by your generator.

```bash
./build/repolens --version
./build/repolens init <repo_path> --index-dir <external_index_path>
./build/repolens status --index-dir <external_index_path>
./build/repolens update --index-dir <external_index_path>
./build/repolens search --index-dir <external_index_path> --query <symbol>
./build/repolens context --index-dir <external_index_path> --symbols "<symbol>" --format json
```

### Dependency check

```text
Windows: dumpbin /dependents repolens.exe
Linux:   ldd ./repolens
macOS:   otool -L ./repolens
```

With the default standalone options, SQLite should not appear as an external runtime dependency.

---

## $ design-principles

```text
[01] Do not modify the target repository.
[02] Store persistent data in an external index directory.
[03] Keep SQLite as the source of truth.
[04] Keep AI optional.
[05] Prefer deterministic context retrieval before semantic magic.
[06] Keep the CLI useful for humans and scripts.
[07] Make the binary easy to move, run, and verify.
```

---

<div align="center">

```text
root@repolens:~# index less, understand more
```

</div>
