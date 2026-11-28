# Changelog

All notable changes to `ted` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-11-28

### Added
- **CLI Startup Line Jump (`+LINE`):** Support `ted +42 file.c` to open a buffer centered directly on a 1-based target line number.
- **Status Bar Cursor Column & Selection Readouts:** Display 1-based visual column (`Col X`) alongside line numbers (`Ln Y/Total`) and live active selection counts (`(N chars sel)` / `(N lines sel)`).
- **Repository Governance & Sample Configuration:** Added `LICENSE` (GPL-3.0-or-later), `CHANGELOG.md`, `CONTRIBUTING.md`, `.gitignore`, and `examples/tedrc`.

### Changed
- Bumped `TED_VERSION` to `1.0.0` across `src/ted.h`, `ted.1`, `README.md`.

## [0.9.1] - 2026-11-27

### Added
- **Headless C Unit Test Suite (`tests/test_ted.c`):** Added `make check` and `make sanitize` (ASan + UBSan) targets testing tab expansion, auto-indent, range cut/paste, block indentation, syntax/shebang detection, bracket matching, config parsing, and atomic saves.
- **CI/CD & Packaging:** Added GitHub Actions workflow (`.github/workflows/ci.yml`) and Debian packaging scaffolding (`debian/`).

### Fixed
- Guarded `memset()` in `editorUpdateSyntax()` (`src/syntax.c`) on empty lines (`rsize == 0`) to resolve a UBSan `nonnull` runtime check while preserving multi-line comment propagation across blank lines.
- Tracked `created_row` on `editOp` so undoing the initial character or newline in an empty buffer restores `numrows == 0`.

## [0.8.0] - 2026-11-27

### Added
- **Modular `src/` Architecture:** Split single-file `ted.c` into `src/ted.h`, `main.c`, `terminal.c`, `syntax.c`, `buffer.c`, `edit.c`, `fileio.c`, and `render.c` with automated `-MMD -MP` header dependency tracking.
- **Multi-Language Syntax Highlighting:** Added syntax rules for C/C++, Python (including `"""` docstrings), Shell/Bash, Makefile, Rust, Go, and Zig, plus automatic `#!` shebang fallback detection.
- **Visual Selection & Range Clipboard:** Added `Shift+Arrow` and left-button mouse drag selection, range copy/cut/paste (`Ctrl-C` / `Ctrl-X` / `Ctrl-V`), and multi-line block indent/outdent (`Tab` / `Shift-Tab`).
- **Crash-Safe Atomic Saves & Save Hygiene:** Upgraded `editorSave()` to write via sibling `mkstemp()` + `fsync()` + `rename()` while preserving permissions and symlinks, added external `st_mtime` overwrite protection, and added `trim_trailing_whitespace` to `~/.tedrc`.

### Fixed
- Added `termWrite()` helper in `src/terminal.c` to handle short writes and eliminate `_FORTIFY_SOURCE` `-Wunused-result` warnings under `-Wall -Wextra -Werror -pedantic`.
