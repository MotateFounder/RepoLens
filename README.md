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

## $ what is repolens

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
[OK] Deterministic source facts
[OK] Situating context metadata
[OK] Local HTTP API
[OK] Lite indexing mode for fast refreshes
[OK] Explicit SQLite compaction
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
repolens update --index-dir <index_path> [--format text|json] [--quiet] [--verbose] [--no-progress] [--lite] [--staged] [--show-diff] [--optimize-large-repo] [--no-similarity-prioritization] [--scip-index <path>]
repolens updateroot [--include-file <path>] [--exclude-file <path>] [--lite] [--staged]
repolens update-files --index-dir <index_path> --repo-root <repo_path> --files <a,b,c> [--lite] [--replace] [--format text|json]
repolens import-scip --index-dir <index_path> <scip-json-path> [--format text|json|--json]
repolens status --index-dir <index_path> --show-scip
repolens status --index-dir <index_path> --fingerprints
repolens status --index-dir <index_path> --staged
repolens status --index-dir <index_path> --similarity
repolens diagnostics --index-dir <index_path>
repolens compact --index-dir <index_path>

repolens search --index-dir <index_path> --query <text> [--kind <kind>] [--limit <n>] [--partial] [--format text|json]
repolens resolve-symbol --index-dir <index_path> <name> [--file <path>] [--format text|json|--json]
repolens snippet --index-dir <index_path> --file <path> --start <n> --end <n> [--format text|json|--json]
repolens symbol-range --index-dir <index_path> <name> [--file <path>] [--format text|json|--json]
repolens compact-view --index-dir <index_path> [--file <path>|--symbol <name>] [--max-depth <n>] [--budget-chars <n>] [--include-private] [--format text|json|--json]
repolens describe --index-dir <index_path> [--file <path>|--symbol <name>|--all] [--deterministic] [--refresh] [--format text|json|--json]
repolens refs --index-dir <index_path> <symbol> [--format text|json|--json]
repolens relationships --index-dir <index_path> <symbol> [--type <kind>] [--format text|json|--json]
repolens unresolved-refs --index-dir <index_path> [--format text|json|--json]
repolens trace --index-dir <index_path> <symbol> [--direction callees|callers|both] [--depth <n>] [--min-confidence <n>] [--max-results <n>] [--budget-chars <n>] [--format text|json|--json]
repolens architecture --index-dir <index_path> [--communities|--hubs] [--level file|symbol] [--top <n>] [--seed <id-or-name>] [--format text|json|--json]
repolens impact --index-dir <index_path> [<symbol>|--symbol <symbol>|--file <path>] [--depth <n>] [--max-results <n>] [--budget-chars <n>] [--include-paths] [--format text|json|--json]
repolens quality --index-dir <index_path> [--dead-code] [--complexity] [--unresolved] [--max-function-lines <n>] [--max-file-lines <n>] [--complexity-threshold <n>] [--top <n>] [--format text|json|--json]
repolens signals import --index-dir <index_path> --type <type> --file <path> [--max-bytes <n>] [--format text|json|--json]
repolens signals list --index-dir <index_path> [--format text|json|--json]
repolens signals search --index-dir <index_path> <query> [--limit <n>] [--format text|json|--json]
repolens signals delete --index-dir <index_path> --path <virtual-or-source-path> [--format text|json|--json]
repolens context --index-dir <index_path> [<symbol>|--symbols "A,B,C"] --format json [--signals <query>] [--situated] [--partial] [--basic] [--budget-chars <n>] [--include-tree] [--include-types] [--grow --grow-files <a,b>]
repolens direct-context --file <path> --signature <text> [--repo-root <path>] [--budget-chars <n>] --format json

repolens serve --index-dir <index_path> --port 7123
repolens enrich --index-dir <index_path> --changed-only
```

---

## $ integrated operating model

RepoLens is now organized as a deterministic context engine rather than a loose collection of commands. The normal loop is:

```text
init/update -> deterministic facts -> focused retrieval -> graph/quality/signals -> context for an agent
```

Recommended small-LLM workflow:

```bash
repolens update --index-dir /path/to/index --show-diff
repolens resolve-symbol --index-dir /path/to/index AutoTuneStart --json
repolens compact-view --index-dir /path/to/index --symbol AutoTuneStart --max-depth 3
repolens refs --index-dir /path/to/index AutoTuneStart --json
repolens trace --index-dir /path/to/index AutoTuneStart --direction both --depth 2 --json
repolens impact --index-dir /path/to/index AutoTuneStart --include-paths --json
repolens quality --index-dir /path/to/index --json
repolens signals search --index-dir /path/to/index "compiler error" --json
repolens context --index-dir /path/to/index AutoTuneStart --signals AutoTuneStart --situated --format json
```

Command families:

```text
Facts:        resolve-symbol, snippet, symbol-range, compact-view, describe
Search:       search, context, direct-context, signals search
Relations:    refs, relationships, unresolved-refs, trace, impact
Architecture: architecture --communities, architecture --hubs
Quality:      quality --dead-code, --complexity, --unresolved
Updates:      update --show-diff, update --staged, update --lite, update --optimize-large-repo
Integrations: import-scip, signals import, serve --mcp, serve --port
Maintenance:  status, diagnostics, compact
```

JSON conventions: commands that support scripting accept `--json` or `--format json`, return stable object keys, and sort results by deterministic tie-breakers such as file path, line number, confidence, depth, and rank. Human-readable output is for terminal inspection; JSON output is the preferred surface for agents and MCP clients.

MCP usage: start `repolens serve --index-dir /path/to/index --mcp`. MCP mode writes protocol JSON only to stdout and logs diagnostics to stderr. Exposed tools cover symbol resolution, snippets, compact views, search, refs, trace, impact, architecture hubs/communities, schema info, status, and quality.

Indexing strategy: use ordinary `update` for full refreshes, `update --show-diff` to inspect Merkle-style file/folder fingerprints, `update --lite --staged` for frequent safe refreshes, and `update --optimize-large-repo` when large repositories benefit from similarity-based processing order. Staged updates build a sibling database and promote only after validation, leaving the previous active index usable after failure.

Optional and degraded modes: SCIP import is optional and uses the documented JSON adapter, so default builds do not require SCIP libraries. AI enrichment is optional; deterministic descriptions and all source facts work without a model. If an index is missing metadata, commands fail with a clear “Run init first” style message. Existing databases are brought forward by schema creation and additive columns/tables where possible.

Known limits and troubleshooting:

```text
No results after edits: run update or update-files for the changed paths.
Unexpected unresolved refs: inspect unresolved-refs and refs --json for strategy/confidence evidence.
Trace or impact looks small: traversal only follows stored resolved relationships and respects depth/confidence limits.
Quality dead code: treat entries as candidates, especially public APIs, reflection, generated code, and plugin entry points.
Signals missing from context: import with signals import, then use signals search or context --signals <query>.
MCP protocol errors: ensure wrappers do not print banners/debug text to stdout before RepoLens starts.
Large logs: use signals import --max-bytes and avoid importing secrets into local indexes.
Large repos: combine --show-diff, --staged, and --optimize-large-repo to keep refreshes observable and safe.
```

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

Use lite mode for frequent low-latency refreshes:

```bash
repolens update --index-dir /path/to/repolens-index --lite
```

Lite mode keeps files and core symbols for search/context, but skips heavier metadata such as symbol parameters, symbol relations, snapshots, and change history. This keeps the database smaller and makes repeated updates faster.

Use staged mode when readers should keep using the last complete database while the next one is being built:

```bash
repolens update --index-dir /path/to/repolens-index --staged
repolens update --index-dir /path/to/repolens-index --staged --lite
```

`--staged` builds in a sibling temporary index directory and promotes the completed `repolens.db` only after the stage succeeds. With `--lite`, only the lite stage is produced. Without `--lite`, RepoLens promotes a lite stage first and then a full stage.

Staged updates are meant for frequent refreshes where retrieval should keep using the last complete database while the next one is prepared. RepoLens copies the active database to `<index>.stage/repolens.db`, applies the update there, validates repository metadata and row counts, then promotes through a short-lived `repolens.db.previous` backup. If validation, update, or promotion fails, RepoLens removes the stage directory and restores the active database from the backup when needed.

```bash
repolens status --index-dir /path/to/repolens-index --staged
```

`status --staged` reports whether the sibling stage directory, staged database, or promotion backup exists. After a successful or failed staged update these should normally be absent. If a process is interrupted before cleanup, the active `repolens.db` remains the source of truth and stale stage files can be inspected or removed.


Index only selected files when an editor, build tool, or retrieval workflow already knows the changed files:

```bash
repolens update-files \
  --index-dir /path/to/repolens-index \
  --repo-root /path/to/repo \
  --files src/Foo.cpp,include/Foo.hpp \
  --lite
```

By default, `update-files` is a partial update: it refreshes the listed files and does not delete symbols from unrelated files. Add `--replace` only when the provided file list is the complete intended indexed set.
Differential Merkle-style updates make repeated indexing deterministic and cheap. RepoLens scans file metadata, compares stored file content hashes, and re-parses only added, modified, reactivated, or explicitly forced files. It also stores folder fingerprints derived from sorted child file fingerprints, plus a repository fingerprint derived from all active files and folders.

```bash
repolens update --index-dir /path/to/repolens-index --show-diff
repolens update --index-dir /path/to/repolens-index --format json --show-diff
repolens status --index-dir /path/to/repolens-index --fingerprints
```

Stored fingerprints include:

```text
files     relative path, content hash, size, line/char counts, modified time, language
folders   folder path, Merkle-style fingerprint, active file count, total byte size
repo      repository fingerprint derived from sorted file and folder fingerprint records
deletes   inactive file rows plus deactivated symbols and change records in full mode
```

Update statistics include `Files scanned`, `Files re-indexed`, `Added`, `Modified`, `Deleted`, `Unchanged`, `Folders changed`, total elapsed time, and the old/new repository fingerprints. JSON output exposes the same data under `files`, `snapshot`, and `diff` so scripts can assert no-op updates without parsing text.

Hash choice: RepoLens currently uses its existing deterministic FNV-1a 64-bit content fingerprint for files and the derived folder/repository fingerprints. This is a strong non-cryptographic fingerprint chosen to keep builds dependency-free; it is not intended as a security hash.

Known limitations: rename and move detection is best-effort at the change-record level. A moved file normally appears as one delete plus one add unless another workflow supplies a narrower file list. Generated files are treated like any other source file unless ignored or excluded.
Similarity prioritization for large repositories is optional. It computes a compact token SimHash for each file, clusters files with nearby signatures, and processes similar groups together. This can improve cache locality and make large initial indexing runs easier to inspect, but it does not skip eligible files.

```bash
repolens update --index-dir /path/to/repolens-index --optimize-large-repo
repolens update --index-dir /path/to/repolens-index --optimize-large-repo --show-diff
repolens update --index-dir /path/to/repolens-index --optimize-large-repo --no-similarity-prioritization
repolens status --index-dir /path/to/repolens-index --similarity
```

What it does:

```text
computes a deterministic token SimHash per file
stores the signature in file metadata
clusters files with similar signatures using Hamming distance
prioritizes update processing by similarity group when enabled
reports group counts, largest group size, and grouped files in diff/status output
```

What it does not do: it does not permanently skip files, suppress parsing, or treat a similar file as already indexed. Every eligible file is still scanned and indexed unless you explicitly use a bounded command such as `update-files`.

The optimization is disabled by default. Use `--optimize-large-repo` for large initial or broad refreshes, and `--no-similarity-prioritization` to force path-order processing even when the optimization flag is present. The signature is approximate and language-agnostic, so groups are hints for ordering and reporting rather than semantic equivalence classes.

### 3. Search the index

```bash
repolens search --index-dir /path/to/repolens-index --query Calibration
```

Use filters:

```bash
repolens search --index-dir /path/to/repolens-index --query AutoTune --kind method --limit 20
repolens search --index-dir /path/to/repolens-index --query Tuning --partial --format json
```

### 4. Resolve deterministic source facts

RepoLens can answer concrete code-fact questions from SQLite without asking an LLM to infer them from raw text.

```bash
repolens resolve-symbol --index-dir /path/to/repolens-index SystemCalibrationPanelViewModel
repolens resolve-symbol --index-dir /path/to/repolens-index AutoTuneStart --file Views/SystemCalibrationPanelViewModel.cs --json
repolens snippet --index-dir /path/to/repolens-index --file src/main.cpp --start 20 --end 45 --json
repolens symbol-range --index-dir /path/to/repolens-index AutoTuneStart --json
```

Human-readable symbol output includes the symbol kind, qualified name, file, line range, stable ID, signature, and parent scope when available. Ambiguous names return every matching candidate in deterministic order so callers can disambiguate by file and line.

Example JSON shape for `resolve-symbol` and `symbol-range`:

```json
{
  "ambiguous": false,
  "symbols": [
    {
      "stable_id": "src/sample.cpp|cpp|function|Demo::Worker::Run|void Run() {|4",
      "row_id": 3,
      "file": "src/sample.cpp",
      "absolute_path": "/path/to/repo/src/sample.cpp",
      "language": "cpp",
      "kind": "function",
      "name": "Run",
      "qualified_name": "Demo::Worker::Run",
      "signature": "void Run() {",
      "line_start": 4,
      "line_end": 6,
      "parent_scope": "Demo::Worker"
    }
  ]
}
```

Example JSON shape for `snippet`:

```json
{
  "file": "src/sample.cpp",
  "absolute_path": "/path/to/repo/src/sample.cpp",
  "language": "cpp",
  "line_start": 4,
  "line_end": 6,
  "code": "    void Run() {\n        Helper();\n    }\n"
}
```

Known limitations: these commands report facts already present in the index and read snippets from the current source file on disk. Run `update` or `update-files` after edits when callers need newly changed line ranges.

### 5. Read compact structural views

Compact views show file and symbol structure without dumping implementation bodies. They are useful when a small local LLM needs to understand organization, declarations, signatures, and line ranges before requesting exact snippets.

```bash
repolens compact-view --index-dir /path/to/repolens-index --file src/indexer.cpp
repolens compact-view --index-dir /path/to/repolens-index --symbol SystemCalibrationPanelViewModel --json
repolens context --index-dir /path/to/repolens-index --file src/indexer.cpp --compact
repolens context --index-dir /path/to/repolens-index --symbol AutoTuneStart --compact --json
```

Useful controls:

```bash
repolens compact-view --index-dir /path/to/index --file src/indexer.cpp --max-depth 3
repolens compact-view --index-dir /path/to/index --symbol Worker --budget-chars 12000
repolens compact-view --index-dir /path/to/index --file src/indexer.cpp --include-private
```

Human-readable output is intentionally compact:

```text
file: src/indexer.cpp

namespace repolens [lines 1-260]
  class RepositoryIndexer [lines 12-240] { ... }
    RepositoryIndexer(Database& db) [lines 18-21] { ... }
    bool update(const UpdateOptions& options) [lines 25-88] { ... }
    std::vector<Symbol> collectSymbols(const FileInfo& file) [lines 91-160] { ... }
```

JSON output keeps the same tree shape for scripts and agents:

```json
{
  "compact": true,
  "views": [
    {
      "file": "src/indexer.cpp",
      "language": "cpp",
      "symbols": [
        {
          "kind": "class",
          "name": "RepositoryIndexer",
          "qualified_name": "repolens::RepositoryIndexer",
          "signature": "class RepositoryIndexer {",
          "line_start": 12,
          "line_end": 240,
          "body_elided": true,
          "children": []
        }
      ],
      "warnings": []
    }
  ],
  "truncated": false
}
```

Compact views are built from indexed symbols. Languages with rich parent metadata produce the best hierarchy; when an interpreter lacks parent links, RepoLens falls back to deterministic line-range nesting. Run `update` or `update-files` after edits to refresh compact line ranges.


### 6. Situate context snippets

Situating context metadata prepends short descriptions before retrieved code so small local LLMs can understand where a snippet sits without reading an entire file first. RepoLens works without any LLM configured: the default provider generates deterministic descriptions from indexed code facts only.

```bash
repolens describe --index-dir /path/to/repolens-index --file src/indexer.cpp
repolens describe --index-dir /path/to/repolens-index --symbol AutoTuneStart --json
repolens describe --index-dir /path/to/repolens-index --all --deterministic
repolens context --index-dir /path/to/repolens-index AutoTuneStart --situated --format json
```

Human-readable descriptions avoid inferred intent:

```text
symbol: repolens::RepositoryIndexer::update
source: deterministic
Method `repolens::RepositoryIndexer::update` in scope `repolens::RepositoryIndexer`, defined in `src/indexer.cpp`, lines 25-88.

file: src/indexer.cpp
source: deterministic
File `src/indexer.cpp` contains 1 namespace, 2 classes, 18 methods and 4 enum declarations.
```

JSON output is stable for scripts:

```json
{
  "descriptions": [
    {
      "target_type": "symbol",
      "target_id": 42,
      "target_key": "sha256:...",
      "source": "deterministic",
      "updated_at": "2026-07-08 10:30:00",
      "description": "Method `AutoTuneStart` in scope `SystemCalibrationPanelViewModel`, defined in `Views/SystemCalibrationPanelViewModel.cs`, lines 120-190."
    }
  ]
}
```

With `--situated`, context output keeps the exact code snippet but prefixes it with the stored or fallback description:

```text
Context: Method `AutoTuneStart` in scope `SystemCalibrationPanelViewModel`, defined in `Views/SystemCalibrationPanelViewModel.cs`, lines 120-190.

<code snippet>
```

Deterministic descriptions are stored in `context_descriptions` with `target_type`, `target_id`, `target_key`, `description`, `source`, and `updated_at`. Optional model-generated descriptions can be added later through the provider layer, but normal RepoLens builds do not require a model, API key, MCP server, or plugin. Descriptions are metadata, not replacements for source facts; use `--refresh` after updates when you want stored text regenerated from the current index. Known limitations: fallback text reports structure, paths, and line ranges only, and it intentionally does not claim business intent or behavior that the index cannot prove.

### 6. Query references and relationships

RepoLens stores a deterministic reference inventory during full indexing. It records resolved same-file calls/references, parent-child containment, parser-discovered inheritance or implementation text, imports/includes/usings, and unresolved call-like references instead of silently dropping them.

```bash
repolens refs --index-dir /path/to/repolens-index AutoTuneStart
repolens refs --index-dir /path/to/repolens-index AutoTuneStart --json
repolens relationships --index-dir /path/to/repolens-index SystemCalibrationPanelViewModel
repolens relationships --index-dir /path/to/repolens-index SystemCalibrationPanelViewModel --type calls
repolens unresolved-refs --index-dir /path/to/repolens-index --json
```

Relationship types currently include:

```text
contains
contained_by
calls
references
inherits
implements
imports
includes
uses_type
```

Reference JSON includes exact source location where available:

```json
{
  "references": [
    {
      "source_symbol": "Demo::Worker::Run",
      "target_symbol": "Demo::Worker::Helper",
      "file": "src/sample.cpp",
      "language": "cpp",
      "line": 6,
      "column": 9,
      "reference_text": "Helper",
      "relationship_type": "calls",
      "confidence": 0.93,
      "resolution_strategy": "same_scope_match",
      "resolution_evidence": "candidate shares source scope 'Demo::Worker'",
      "unresolved": false
    }
  ]
}
```

Relationship JSON exposes graph-style edges:

```json
{
  "relationships": [
    {
      "source_symbol": "Demo::Worker::Run",
      "target_symbol": "Demo::Worker::Helper",
      "relationship_type": "calls",
      "file": "src/sample.cpp",
      "line": 6,
      "column": 9,
      "confidence": 0.93,
      "resolution_strategy": "same_scope_match",
      "resolution_evidence": "candidate shares source scope 'Demo::Worker'",
      "unresolved": false
    }
  ]
}
```

RepoLens resolves calls and references through a deterministic cascade. Each stored reference and relationship records `resolution_strategy`, `confidence`, `resolution_evidence`, and `unresolved` so callers can decide how much trust to place in a link.

Current strategy order:

```text
exact_fully_qualified  token text exactly matches a qualified symbol
receiver_type          hybrid type pass matched receiver variable to a parent type
same_scope_match       candidate shares the caller scope
same_file_match        unique candidate in the same file or translation unit
same_namespace_match   candidate shares the caller namespace/module root
import_context_match   candidate namespace appears in using/import/include context
unique_global_name     only one active symbol has the simple name
suffix_qualified_match token matches a qualified-name suffix
unresolved_ambiguous   multiple candidates matched the best available strategy
unresolved_call_text   no deterministic candidate was found
```

Confidence is numeric and intentionally conservative: parser containment uses `1.0`; exact qualified matches use `1.0`; hybrid receiver type matches use about `0.97` for parameters, `0.96` for local variables, and `0.94` for class fields; same-scope matches use about `0.93`; same-file matches use about `0.9`; namespace/import/global/suffix matches step down from there. Ambiguous calls are stored unresolved instead of being forced to a low-confidence target.

Example resolved call:

```json
{
  "source_symbol": "Cascade::Runner::Run",
  "target_symbol": "Alpha::Service::Ping",
  "relationship_type": "calls",
  "confidence": 0.97,
  "resolution_strategy": "receiver_type",
  "resolution_evidence": "receiver 'service' matched parameter type 'Alpha::Service'",
  "unresolved": false
}
```

Example unresolved ambiguous call:

```json
{
  "source_symbol": "Cascade::Runner::Run",
  "target_symbol": "Clash",
  "relationship_type": "calls",
  "confidence": 0.45,
  "resolution_strategy": "unresolved_ambiguous",
  "resolution_evidence": "multiple candidates matched unique_global_name",
  "unresolved": true
}
```

Imports/includes/usings are also stored as inventory facts. They can influence the cascade where the indexed symbols and text context are sufficient, but unresolved import/include edges remain visible with strategies such as `preprocessor_include` or `import_statement`.

The hybrid type resolution pass runs before receiver calls enter the resolver cascade. It builds a lightweight type environment for each parsed file and symbol scope from:

```text
function parameters
local variable declarations
class fields
simple using/typedef aliases
pointer/reference/const-normalized type names
basic smart-pointer templates such as std::unique_ptr<T>, std::shared_ptr<T>, std::weak_ptr<T>, and optional<T>
```

Supported C++ receiver patterns include:

```cpp
ProjectManager manager;
manager.loadProject(path);

SystemCalibrationPanelViewModel* vm;
vm->AutoTuneStart();

std::unique_ptr<ProjectManager> manager;
manager->loadProject(path);

const SpeakerResponse& response;
response.Load(filePath);
```

The pass intentionally does not become a compiler. It does not perform overload resolution, template instantiation, macro expansion, control-flow-sensitive type tracking, or dynamic dispatch analysis. When receiver evidence still leaves multiple candidate methods, RepoLens records the call as unresolved and marks the ambiguity in `resolution_evidence`.

Known limitations: the generic cascade and hybrid type pass use indexed symbols plus lightweight source-token evidence. C and C++ consider translation unit text, namespaces/classes, includes/usings, parameters, local declarations, class fields, simple aliases, pointer/reference qualifiers, and common smart-pointer wrappers. C# uses namespaces, `using` lines, class scope, and simple receiver type hints from parsed parameters or local declarations. Other languages use the generic symbol/name/scope cascade and may produce more unresolved facts. RepoLens still does not perform overload resolution, full template specialization, macro expansion, dynamic dispatch analysis, impact analysis, PageRank, or community detection.


### 7. Optional SCIP indexing

RepoLens can optionally ingest externally generated SCIP-derived data to improve symbol, reference, call, inheritance, and graph accuracy. SCIP is not required for normal indexing; default builds still use the built-in language interpreters and resolver cascade.

```bash
repolens update --index-dir /path/to/repolens-index --scip-index ./index.scip.json
repolens import-scip --index-dir /path/to/repolens-index ./index.scip.json
repolens import-scip --index-dir /path/to/repolens-index ./index.scip.json --json
repolens status --index-dir /path/to/repolens-index --show-scip
```

The first adapter supports a documented intermediate JSON export format instead of binary `.scip` parsing, so RepoLens does not need protobuf or SCIP libraries at build time:

```json
{
  "symbols": [
    {
      "scip_symbol": "local 0",
      "kind": "function",
      "name": "loadProject",
      "qualified_name": "Hybrid::ProjectManager::loadProject",
      "file": "src/hybrid.cpp",
      "language": "cpp",
      "line_start": 3,
      "line_end": 5
    }
  ],
  "references": [
    {
      "source_symbol": "Hybrid::Scenario::Run",
      "target_symbol": "Hybrid::ProjectManager::loadProject",
      "file": "src/hybrid.cpp",
      "line": 33,
      "column": 17,
      "reference_text": "manager.loadProject",
      "relationship_type": "calls",
      "language": "cpp"
    }
  ]
}
```

SCIP-derived relationships are stored in the existing reference and relationship tables with `resolution_strategy: "scip"`, high confidence for mapped references, and explicit unresolved status when a SCIP target cannot be mapped. If SCIP and RepoLens heuristics disagree at the same source location, RepoLens keeps both facts, prefers SCIP in downstream queries by confidence, and reports conflict counts in `status --show-scip`.

Languages with mature SCIP producers benefit most, especially C/C++, Java/Kotlin, Scala, Go, Rust, TypeScript/JavaScript, and Python when an external SCIP indexer is available. Without SCIP data, RepoLens falls back to its normal interpreters, resolver cascade, and hybrid type pass.

Known limitations: RepoLens currently imports SCIP data from the JSON adapter format above. Binary `.scip` parsing is intentionally not built in yet, and no SCIP dependency is required for default builds. Mapping is best-effort by qualified name, simple name, and file path; unmapped targets remain visible as unresolved SCIP references.
### 8. Trace relationship chains

Graph tracing follows the stored relationship inventory so agents can ask for callers, callees, dependencies, and nearby related symbols without searching raw source text. Traversal is deterministic, cycle-safe, depth-limited, and sorted by confidence, file path, and source location.

```bash
repolens trace --index-dir /path/to/repolens-index AutoTuneStart --direction callees --depth 2
repolens trace --index-dir /path/to/repolens-index AutoTuneStart --direction callers --depth 3
repolens trace --index-dir /path/to/repolens-index AutoTuneStart --direction both --depth 2 --min-confidence 0.7
repolens trace --index-dir /path/to/repolens-index AutoTuneStart --budget-chars 12000 --json
```

Direction options:

```text
callees  follow outgoing relationships from the root symbol
callers  follow incoming relationships to the root symbol
both     follow incoming and outgoing relationships
```

Useful controls:

```bash
repolens trace --index-dir /path/to/index AutoTuneStart --depth 1
repolens trace --index-dir /path/to/index AutoTuneStart --max-results 50
repolens trace --index-dir /path/to/index AutoTuneStart --budget-chars 6000 --json
```

Human-readable output includes the root symbol, direction, depth, reached symbols, relationship edges, paths, file paths, line ranges, and confidence values.

JSON output is shaped for scripts and local agents:

```json
{
  "root_symbol": "AutoTuneStart",
  "direction": "callees",
  "max_depth": 2,
  "truncated": false,
  "budget": {"requested_chars": 12000},
  "nodes": [
    {
      "id": 42,
      "depth": 1,
      "kind": "function",
      "name": "ValidateInput",
      "qualified_name": "Calibration::ValidateInput",
      "file": "src/calibration.cpp",
      "line_start": 30,
      "line_end": 54
    }
  ],
  "edges": [
    {
      "id": 77,
      "depth": 1,
      "source_symbol_id": 12,
      "target_symbol_id": 42,
      "source_symbol": "Calibration::AutoTuneStart",
      "target_symbol": "Calibration::ValidateInput",
      "relationship_type": "calls",
      "file": "src/calibration.cpp",
      "line": 18,
      "column": 9,
      "confidence": 0.93,
      "resolution_strategy": "same_scope_match",
      "resolution_evidence": "candidate shares source scope 'Demo::Worker'",
      "unresolved": false
    }
  ],
  "paths": [
    {"depth": 1, "confidence": 0.9, "path": "Calibration::AutoTuneStart --calls--> Calibration::ValidateInput"}
  ]
}
```

Unresolved edges remain visible in reference and relationship queries, but tracing only expands through edges that have a target symbol ID. Use `--min-confidence` when you want only higher-confidence links. Budget limits return valid JSON with `truncated: true` when output must be shortened.

Known limitations: tracing follows the relationships currently stored in SQLite. It does not perform new semantic resolution during traversal and does not implement impact analysis.

### 9. Analyze architecture communities and hubs

Architecture analysis projects stored symbol relationships into a graph so RepoLens can summarize large repositories without asking an LLM to infer structure from raw text. Use it to find strongly connected functional areas and important hub files or symbols.

```bash
repolens architecture --index-dir /path/to/repolens-index --communities
repolens architecture --index-dir /path/to/repolens-index --communities --level file --json
repolens architecture --index-dir /path/to/repolens-index --hubs
repolens architecture --index-dir /path/to/repolens-index --hubs --level symbol --top 25 --json
repolens architecture --index-dir /path/to/repolens-index --hubs --seed AutoTuneStart
```

Levels:

```text
file    group and rank files using relationships between their contained symbols
symbol  group and rank individual symbols where resolved target IDs are available
```

Community detection is a pragmatic first version: RepoLens builds a weighted graph from resolved `calls`, `references`, `imports`, `includes`, `inherits`, `implements`, `uses_type`, and `contains` relationships, then reports deterministic weighted connected components. Edge weights favor stronger architectural signals such as calls and inheritance, and are scaled by stored confidence.

Hub ranking uses a deterministic PageRank-style score over the directed relationship graph. The human-readable view also reports incoming and outgoing degrees so you can see caller-heavy and callee-heavy hubs at a glance. `--seed` switches the PageRank teleport set to matching files or symbols for a lightweight personalized ranking.

Human-readable output is shaped like:

```text
Architecture Analysis
Level: file
Nodes: 42
Edges: 117
Communities: 5
Largest community size: 14

Communities
  [0] size=14 weight=39.45
    src/calibration.cpp
    src/project_manager.cpp

Hubs
  1. src/project_manager.cpp score=0.083 in=12 out=7 community=0
```

JSON output includes summary stats, communities, hubs, and projected edges:

```json
{
  "level": "file",
  "seed": "",
  "summary": {
    "nodes": 42,
    "edges": 117,
    "communities": 5,
    "largest_community_size": 14
  },
  "communities": [
    {"id": 0, "size": 14, "internal_weight": 39.45, "nodes": ["src/calibration.cpp"]}
  ],
  "hubs": [
    {"rank": 1, "id": "src/project_manager.cpp", "label": "src/project_manager.cpp", "community": 0, "pagerank": 0.083, "in_degree": 12, "out_degree": 7, "incoming_weight": 18.2, "outgoing_weight": 10.4}
  ],
  "edges": [
    {"source": "src/ui.cpp", "target": "src/project_manager.cpp", "relationship_type": "calls", "weight": 2.79, "confidence": 0.93, "resolution_strategy": "receiver_type"}
  ]
}
```

Interpretation notes: architecture analysis only uses resolved relationships with target symbol IDs, so unresolved references remain visible through `unresolved-refs` but are excluded from communities and PageRank. The community algorithm is intentionally simpler than full Louvain modularity optimization; treat the groups as stable architectural hints, not proof of module ownership.
### 10. Analyze change impact

Impact analysis follows reverse dependencies in the stored relationship graph so RepoLens can report the likely ripple effect of changing a function, class, file, or symbol. It is deterministic and confidence-aware: direct callers are listed before transitive callers, shorter paths rank before longer paths, higher-confidence paths rank first within the same depth, and file-path tie-breakers keep repeated runs stable.

```bash
repolens impact --index-dir /path/to/repolens-index AutoTuneStart
repolens impact --index-dir /path/to/repolens-index AutoTuneStart --depth 3
repolens impact --index-dir /path/to/repolens-index --file src/indexer.cpp
repolens impact --index-dir /path/to/repolens-index AutoTuneStart --include-paths --json
```

Output includes:

```text
root target and root type
summary counts for affected files and symbols
direct dependents
transitive dependents
affected files to review
representative call paths when --include-paths is set
confidence notes for traversed edges below the low-confidence threshold
truncation metadata when depth, result, or budget limits apply
```

Human-readable output is shaped like:

```text
Impact Analysis
Root: AutoTuneStart
Root type: symbol
Depth: 2
Affected files: 2
Affected symbols: 4
Direct dependents: 1
Transitive dependents: 3

Affected files
  src/calibration_panel.cpp

Direct dependents
  [d1] UI::CalibrationPanel::Start confidence=0.97 src/calibration_panel.cpp:42-60
```

JSON output is shaped for scripts and local agents:

```json
{
  "root_target": "AutoTuneStart",
  "root_type": "symbol",
  "max_depth": 2,
  "truncated": false,
  "budget": {"requested_chars": 12000},
  "summary": {
    "root_symbols": 1,
    "affected_files": 2,
    "affected_symbols": 4,
    "direct_dependents": 1,
    "transitive_dependents": 3,
    "low_confidence_edges": 1
  },
  "affected_files": ["src/calibration_panel.cpp"],
  "direct_dependents": [
    {
      "id": 88,
      "depth": 1,
      "confidence": 0.97,
      "kind": "function",
      "name": "Start",
      "qualified_name": "UI::CalibrationPanel::Start",
      "file": "src/calibration_panel.cpp",
      "line_start": 42,
      "line_end": 60
    }
  ],
  "paths": [
    {"depth": 1, "confidence": 0.97, "path": "UI::CalibrationPanel::Start --calls--> Calibration::AutoTuneStart"}
  ],
  "confidence_notes": {
    "low_confidence_threshold": 0.9,
    "low_confidence_edges": []
  }
}
```

For file impact, RepoLens treats all indexed symbols in the file as changed roots and reports dependents outside that root set. This is useful for review planning: affected files are files that contain callers or dependents that may need inspection.

Limitations: impact analysis follows relationships already stored in SQLite and does not perform fresh semantic resolution during traversal. Unresolved references without target symbol IDs cannot be traversed, but low-confidence resolved edges are called out separately. Cycles are deduplicated and depth-limited. This command does not implement Merkle updates.
### 11. Measure deterministic code quality

Quality reports turn indexed code facts into concrete grounding signals for agents and review workflows. The report uses stored symbols, relationships, unresolved references, file metadata, and deterministic source scans instead of asking a model to infer quality from raw text.

```bash
repolens quality --index-dir /path/to/repolens-index
repolens quality --index-dir /path/to/repolens-index --dead-code
repolens quality --index-dir /path/to/repolens-index --complexity --complexity-threshold 10
repolens quality --index-dir /path/to/repolens-index --unresolved --json
repolens quality --index-dir /path/to/repolens-index --max-function-lines 200 --max-file-lines 1000 --top 30
```

The report measures unresolved references, weak or heuristic relationship resolution, dead-code candidates, cyclomatic complexity, large files, large symbols or methods, highly referenced symbols, and high fan-in/fan-out symbols. Dead-code results are deliberately labeled candidates: RepoLens excludes obvious entry points and externally visible public APIs where possible, but it cannot prove that reflection, dynamic dispatch, generated code, plugins, or external callers never use a symbol.

Human-readable output is grouped by deterministic sections:

```text
Quality Report
Files scanned: 42
Symbols measured: 318
Unresolved references: 3
Dead code candidates: 7
Complex symbols: 4
Large files: 2

Complex symbols
  Calibration::AutoTuneStart complexity=14 lines=78 src/calibration.cpp:120-198
```

JSON output is shaped for scripting and MCP clients:

```json
{
  "summary": {"files": 42, "symbols": 318, "unresolved_references": 3, "dead_code_candidates": 7},
  "thresholds": {"max_function_lines": 200, "max_file_lines": 1000, "complexity_threshold": 10, "top": 30},
  "unresolved_references": [{"reference_text": "MissingQuality", "source_file": "src/quality.cpp", "line": 24, "resolution_strategy": "unresolved", "confidence": 0.0}],
  "dead_code_candidates": [{"qualified_name": "Quality::UnusedPrivate", "kind": "method", "file_path": "src/quality.cpp", "line_start": 36, "line_end": 37, "reason": "no indexed incoming references"}],
  "complex_symbols": [{"qualified_name": "Quality::Complex", "complexity": 8, "lines": 14}],
  "high_coupling_symbols": [{"qualified_name": "Quality::Caller", "fan_in": 0, "fan_out": 3}]
}
```

Cyclomatic complexity is exact only where RepoLens has enough parsed source range data. The fallback scanner counts deterministic branch tokens such as `if`, `for`, `while`, `case`, `catch`, `&&`, `||`, and ternary operators inside indexed symbol ranges. Use the report as a stable prioritization and grounding aid for small-context LLMs, not as a substitute for compiler-grade static analysis.
### 12. Import peripheral signals as virtual files

Peripheral signals are non-code context that helps coding agents stay grounded: terminal output, compiler logs, test output, chat notes, manual summaries, and other local text. RepoLens stores these as virtual files in the index so they can be listed, searched, and attached to context output without polluting source-code symbols or relationship graphs.

```bash
repolens signals import --index-dir /path/to/repolens-index --type terminal --file ./logs/build.txt
repolens signals import --index-dir /path/to/repolens-index --type test-output --file ./logs/tests.txt --json
repolens signals import --index-dir /path/to/repolens-index --type chat --file ./notes/chat.md
repolens signals list --index-dir /path/to/repolens-index --json
repolens signals search --index-dir /path/to/repolens-index "compiler error" --json
repolens context --index-dir /path/to/repolens-index --signals AutoTuneStart --format json
```

Imported signals are text data only. RepoLens does not execute logs, scripts, terminal transcripts, or chat notes. Each virtual file stores a signal type, original source path, stable virtual path, import timestamp, content hash, size, line count, truncation flag, and content. Re-importing the same source path refreshes the virtual file content and metadata.

JSON search output is shaped like:

```json
{
  "results": [
    {
      "id": 1,
      "type": "terminal",
      "virtual_path": "signals/terminal/abc123/build.txt",
      "source_path": "logs/build.txt",
      "content_hash": "...",
      "line": 2,
      "snippet": "compiler error C1001 near AutoTuneStart"
    }
  ]
}
```

Signals differ from source files: normal `search`, symbol resolution, references, traces, impact analysis, quality reports, and architecture graphs remain source-code based. Use `signals search` or `context --signals <query>` when you want imported logs or notes. The default import limit is 1 MiB per file; use `--max-bytes` for larger logs. Privacy note: imported logs and chat notes are stored locally in `repolens.db`, so avoid importing secrets unless that local index is allowed to contain them.

Recommended agent workflow: import build output after a failed compile, import test output after a failed suite, search signals for the failing symbol or error code, then request source context with `--signals` so the model sees both deterministic source facts and the relevant diagnostic text.
### 13. Build an AI context package

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

Grow the index around a missing symbol without scanning the whole repository:

```bash
repolens context \
  --index-dir /path/to/index \
  --symbols "DoWork" \
  --grow \
  --grow-files src/Worker.cpp,include/Worker.hpp \
  --format json
```

`--grow` checks the current database first. If a requested symbol is missing, RepoLens parses only `--grow-files`, saves those symbols into the same database, and then returns the normal context JSON.

Return context from one file immediately without opening or writing SQLite:

```bash
repolens direct-context \
  --file /path/to/repo/src/Worker.cpp \
  --repo-root /path/to/repo \
  --signature "DoWork" \
  --format json
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

`update --lite` stores a smaller structured index: files and core symbols only. It is intended for workflows where RepoLens is called often and context retrieval speed matters more than relation graphs, history, or enrichment metadata.

`update --staged` builds replacement databases in a temporary sibling directory and promotes completed databases into place only after validation succeeds. Existing `search` and `context` calls can continue reading the last complete database while the next one is being prepared; failed stages clean up the temporary directory and leave the active database usable.

`update-files` parses only explicitly listed files and saves them into the same schema used by full updates. It is the fastest path when a caller already knows the files relevant to a retrieval request.

`context --grow --grow-files <a,b>` turns a context miss into a bounded partial update. It first searches the current database, parses only the supplied grow files when needed, then returns context from the updated database.

`direct-context` bypasses SQLite entirely. It parses one file in memory and returns context for a matching symbol name, qualified name, or signature.

`diagnostics` reports SQLite database size and row counts for repositories, files, symbols, symbol parameters, symbol relations, snapshots, and changes. It is useful for explaining index growth after updates.

`compact` runs SQLite optimization and compaction. It is useful after switching an existing full index to lite mode, or after many update cycles:

```bash
repolens update --index-dir /path/to/repolens-index --lite
repolens compact --index-dir /path/to/repolens-index
```

`updateroot` stores `repolens.db` in the same directory as the `repolens` executable and updates only the included paths that are not excluded. By default, it reads `include.txt` and optional `exclude.txt` from that executable directory.

`updateroot --lite` applies the same reduced storage mode and is the recommended mode for very frequent context refreshes.

Use custom path-list files when they need to live somewhere else:

```bash
repolens updateroot --include-file /path/to/include.txt --exclude-file /path/to/exclude.txt
repolens updateroot --include-file /path/to/include.txt --exclude-file /path/to/exclude.txt --lite
```

`--include-file` is required only when `include.txt` is not beside the executable. `--exclude-file` is optional; when omitted, RepoLens uses `exclude.txt` beside the executable if it exists. Each non-empty line in those files is a quoted or unquoted absolute file or directory path.

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

`POST /update` accepts an optional lite flag:

```json
{
  "lite": true
}
```

This performs the same reduced-storage update as `repolens update --lite`.

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

## $ speed-modes

RepoLens now has four fast paths for large repositories:

All speed modes use the shared language interpreter registry, so they apply to every supported parser rather than only C# or C++.

| Mode | Command | What it saves | Best use |
|---|---|---|---|
| Lite index | `update --lite` / `updateroot --lite` | Files and core symbols only | Small database, frequent refreshes |
| Direct context | `direct-context --file ... --signature ...` | Nothing | One editor selection, instant answer |
| File-list index | `update-files --files ...` | Only listed files | Known changed files or focused retrieval |
| Query-grown index | `context --grow --grow-files ...` | Listed files only when a symbol is missing | Start tiny, grow around actual queries |
| Staged update | `update --staged` / `updateroot --staged` | Promoted complete databases | Keep old DB readable while the next stage builds |

Recommended low-latency workflow:

```bash
repolens init /path/to/repo --index-dir /path/to/index
repolens update-files --index-dir /path/to/index --repo-root /path/to/repo --files src/Current.cpp --lite
repolens context --index-dir /path/to/index --symbols "CurrentSymbol" --format json
```

For editor-driven retrieval, prefer this order:

```text
[01] direct-context when the caller has file path + signature
[02] context against the existing DB
[03] context --grow with explicit candidate files when the symbol is missing
[04] update-files --lite for known changed files
[05] update --staged for periodic broader refreshes
```

Current boundary: `--grow` intentionally requires `--grow-files`. RepoLens does not yet guess candidate files by include graph or repository-wide text search, because that can silently turn an instant query into a full scan.

---

<div align="center">

```text
root@repolens:~# index less, understand more
```

</div>







---

## $ mcp-server

RepoLens can run as a native Model Context Protocol server over stdio for Claude Code, Cursor, and other MCP-capable agents.

Start it with:

```bash
repolens serve --index-dir /path/to/repolens-index --mcp
```

MCP mode writes JSON-RPC responses only to stdout. Do not print banners, progress logs, or debug text to stdout in this mode; diagnostics must go to stderr. Each request is read as one JSON line from stdin. RepoLens MCP is a stateful stdio server that uses the standard initialize -> notifications/initialized lifecycle; it does not implement a stateless per-request metadata model.

Supported protocol versions are declared in the server in newest-first order: `2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05`. During `initialize`, RepoLens echoes a supported requested `params.protocolVersion`; if the client requests an unsupported version, RepoLens negotiates by returning the newest supported version. This is the legacy/stateful stdio lifecycle behavior used by the listed protocol versions; HTTP `MCP-Protocol-Version` headers and any future stateless per-request metadata model are outside RepoLens MCP stdio mode. RepoLens does not implement `server/discover`, does not remove the initialize lifecycle, does not use per-request `_meta` protocol version metadata, and does not implement stateless Streamable HTTP; those would be separate future features.

Session lifecycle:

```text
1. Client sends initialize as the first normal request.
2. Server responds with negotiated protocolVersion, tools capability, and serverInfo.
3. Client sends notifications/initialized with no id.
4. Server enters the ready state; tools/list and tools/call are now accepted.
```

`ping` is accepted before and after initialization. `tools/list` and `tools/call` before the initialized notification return a structured protocol error explaining that the MCP session is not ready. `notifications/initialized`, `notifications/cancelled`, `notifications/progress`, `notifications/roots/list_changed`, and unknown notifications have no `id` and produce no stdout response.

Supported request methods: `initialize`, `ping`, `shutdown`, `tools/list`, and `tools/call`. RepoLens ignores inbound JSON-RPC response objects because it does not send server-initiated requests in stdio mode.

Available tools: `resolve_symbol`, `get_snippet`, `compact_view`, `search`, `refs`, `trace`, `impact`, `architecture_hubs`, `architecture_communities`, `schema_info`, `status`, and `quality`.

Initialize and mark the session ready:

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"example-client","version":"1.0"}}}
```

```json
{"jsonrpc":"2.0","method":"notifications/initialized"}
```

List tools after initialization. RepoLens returns its full static tool list in deterministic order. It accepts omitted params, `{}`, or an empty cursor; non-empty pagination cursors return invalid params because the list is not paginated and no `nextCursor` is emitted.

List tools after initialization:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
```

Call a tool:

```json
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"resolve_symbol","arguments":{"name":"AutoTuneStart","file":"Views/SystemCalibrationPanelViewModel.cs"}}}
```

Tool results include concise human-readable `content` plus machine-readable `structuredContent`. RepoLens keeps stdout line-oriented: each response is one compact JSON-RPC object on one line with no embedded raw newlines. For backwards compatibility with clients that do not read `structuredContent`, RepoLens keeps the text block human-readable rather than duplicating potentially large JSON payloads into text; clients should use `structuredContent` for machine-readable data.

Example successful response shape:

```json
{"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"Resolved 1 matching symbol. Results are available in structuredContent.symbols."}],"structuredContent":{"symbols":[{"name":"AutoTuneStart","kind":"method","file":"Views/SystemCalibrationPanelViewModel.cs","line_start":120,"line_end":190}],"truncated":false},"isError":false}}
```

Snippet request:

```json
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_snippet","arguments":{"file":"src/main.cpp","start":20,"end":45}}}
```

`tools/list` includes `inputSchema`, `outputSchema`, and read-only annotations for every RepoLens tool. Most tools advertise `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true`, and `openWorldHint: false`. Output schemas describe the stable top-level structured fields; complex tools may keep flexible nested fields.

Potentially large results include truncation metadata where RepoLens can report it, such as `truncated`, `returnedCount`, `totalCount`, `returnedNodes`, or `returnedEdges`. A `false` truncation value means RepoLens did not intentionally limit that MCP result.

MCP request validation separates protocol failures from tool execution failures:

```text
JSON-RPC protocol error: invalid JSON, missing/wrong jsonrpc, invalid id, non-string method, unknown MCP method, unknown tool name, tools/call before initialization, or invalid tools/call shape.
MCP tool error result: valid tools/call request where the named tool cannot run because arguments or repository facts are missing or invalid.
```

Request IDs must be strings or integers and are echoed exactly. Null, boolean, object, array, and fractional IDs are rejected as invalid requests. Notifications omit `id` entirely and never receive responses.

Protocol error example:

```json
{"jsonrpc":"2.0","id":5,"error":{"code":-32600,"message":"Invalid JSON-RPC request: jsonrpc must be \"2.0\"."}}
```

Tool error example:

```json
{"jsonrpc":"2.0","id":6,"result":{"content":[{"type":"text","text":"Tool error: search requires a non-empty query argument."}],"structuredContent":{"error":{"code":"missing_argument","message":"search requires a non-empty query argument.","argument":"query"}},"isError":true}}
```

Tool errors are returned in-band so agents can self-correct the next `tools/call` without treating the MCP session as broken.

Argument requirements:

```text
resolve_symbol: name or symbol string; optional file string
get_snippet: file string, start/end line integers; end must be >= start
compact_view: file or symbol string; optional max_depth, budget_chars, include_private
search: non-empty query string; optional kind string and limit integer
refs: symbol or name string
trace: symbol or name string; optional direction callees|callers|both, depth, min_confidence, max_results, budget_chars
impact: symbol/name string or file string; optional depth, max_results, budget_chars, include_paths
architecture_hubs: optional level file|symbol, top, seed
architecture_communities: optional level file|symbol, top, seed
schema_info: no arguments; unexpected arguments return isError: true
status: no arguments; unexpected arguments return isError: true
quality: optional max_function_lines, max_file_lines, complexity_threshold, top
```

The no-argument tools advertise explicit empty object schemas with `additionalProperties: false`. Unknown tools and invalid requests return structured JSON-RPC errors. Notifications never receive responses, including unknown notifications. If an MCP client reports protocol failures, check that no wrapper script, shell profile, or debugger is printing anything to stdout before RepoLens starts. The existing HTTP server remains available with `repolens serve --index-dir <index> --port 7123`; MCP mode does not require HTTP.

Manual MCP interoperability test:

```text
stdin  -> {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}
stdout <- {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18",...}}
stdin  -> {"jsonrpc":"2.0","method":"notifications/initialized"}
stdout <- no response
stdin  -> {"jsonrpc":"2.0","id":2,"method":"ping"}
stdout <- {"jsonrpc":"2.0","id":2,"result":{}}
stdin  -> {"jsonrpc":"2.0","id":3,"method":"tools/list"}
stdout <- {"jsonrpc":"2.0","id":3,"result":{"tools":[{"name":"status","inputSchema":...,"outputSchema":...,"annotations":...}]}}
stdin  -> {"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"status","arguments":{}}}
stdout <- {"jsonrpc":"2.0","id":4,"result":{"content":[...],"structuredContent":{"repo_root":...},"isError":false}}
stdin  -> {"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"resolve_symbol","arguments":{"name":"AutoTuneStart"}}}
stdout <- {"jsonrpc":"2.0","id":5,"result":{"content":[...],"structuredContent":{"symbols":[...]},"isError":false}}
stdin  -> {"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"search","arguments":{}}}
stdout <- {"jsonrpc":"2.0","id":6,"result":{"content":[...],"structuredContent":{"error":...},"isError":true}}
stdin  -> {"jsonrpc":"2.0","method":"notifications/custom"}
stdout <- no response
stdin  -> {"jsonrpc":"2.0","id":99,"result":{}}
stdout <- no response
```

To connect an MCP-capable coding assistant, configure a stdio server command equivalent to `repolens serve --index-dir /path/to/repolens-index --mcp`. Set the working directory to the repository or index location if the client supports it, and make sure wrapper scripts do not print shell banners or diagnostics to stdout.
