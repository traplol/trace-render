#!/usr/bin/env python3
"""Insert a tracing macro into every C++ function body using libclang.

Usage:
    python3 scripts/insert_trace.py [options]

Options:
    --build-dir DIR       Path to build dir with compile_commands.json (default: build)
    --macro MACRO         Macro to insert (default: TRACE_FUNCTION())
    --include HEADER      Header to ensure is included (default: tracing.h)
    --commit              Actually modify files (default is dry-run)
    --remove              Strip existing TRACE_ macros first, then insert standardized ones
    --skip-pattern PAT    Regex pattern for function names to skip (can repeat)
    --ignore-file FILE    File with function names to skip, one per line (default: .trace_ignore)
    --only GLOB           Only process files matching this glob (e.g. "src/ui/*.cpp")
    --min-lines N         Skip functions with fewer than N lines (default: 1)

The macro is inserted as the first statement after the opening brace of each
function definition. Functions that already contain any TRACE_ macro call
are skipped.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path
from fnmatch import fnmatch

import clang.cindex as ci


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build-dir", default="build", help="Build directory with compile_commands.json")
    p.add_argument("--macro", default='TRACE_FUNCTION_CAT("{cat}")',
                   help='Macro call to insert. {cat} is replaced with the directory-based category '
                        '(default: TRACE_FUNCTION_CAT("{cat}")). Use TRACE_FUNCTION() for no category.')
    p.add_argument("--include", default="tracing.h", help="Header to ensure is included")
    p.add_argument("--commit", action="store_true", help="Actually modify files (default is dry-run)")
    p.add_argument("--remove", action="store_true", help="Strip existing TRACE_ macros first, then insert standardized ones")
    p.add_argument("--skip-pattern", action="append", default=[], help="Regex for function names to skip")
    p.add_argument("--ignore-file", default=".trace_ignore",
                   help="File with function names to skip, one per line (default: .trace_ignore)")
    p.add_argument("--only", default=None, help="Only process files matching this glob")
    p.add_argument("--min-lines", type=int, default=1, help="Skip functions shorter than N lines")
    return p.parse_args()


def load_ignore_list(ignore_file):
    """Load function names from an ignore file, one per line. Blank lines and #comments skipped."""
    if not os.path.exists(ignore_file):
        return set()
    names = set()
    with open(ignore_file) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                names.add(line)
    return names


def category_for_file(filepath, project_root):
    """Derive a tracing category from the file's directory relative to src/.

    src/ui/foo.cpp       -> "ui"
    src/model/bar.cpp    -> "model"
    src/parser/baz.cpp   -> "parser"
    src/platform/x.cpp   -> "platform"
    src/app.cpp          -> "app"
    tests/test_foo.cpp   -> "tests"
    """
    rel = os.path.relpath(filepath, project_root)
    parts = Path(rel).parts  # e.g. ("src", "ui", "foo.cpp")
    if len(parts) >= 3 and parts[0] == "src":
        return parts[1]  # subdirectory name: ui, model, parser, platform
    elif len(parts) >= 2 and parts[0] == "src":
        return "app"  # top-level src/ files
    elif parts[0] == "tests":
        return "tests"
    else:
        return "other"


def load_compile_commands(build_dir):
    path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(path):
        print(f"Error: {path} not found. Run cmake -B {build_dir} first.", file=sys.stderr)
        sys.exit(1)
    with open(path) as f:
        return json.load(f)


def get_project_files(commands, project_root, only_glob):
    """Filter compile_commands to project source files only."""
    files = []
    for entry in commands:
        filepath = entry["file"]
        # Skip files outside project root (deps, build artifacts)
        if not filepath.startswith(project_root):
            continue
        # Only .cpp files (skip headers parsed via compile_commands)
        if not filepath.endswith(".cpp"):
            continue
        # Skip test files and third-party
        rel = os.path.relpath(filepath, project_root)
        if rel.startswith("build/") or rel.startswith("third_party/") or rel.startswith("external/"):
            continue
        if rel.startswith("tests/"):
            continue
        if only_glob and not fnmatch(rel, only_glob):
            continue
        files.append((filepath, entry))
    # Deduplicate by filepath (compile_commands can have duplicates)
    seen = set()
    deduped = []
    for filepath, entry in files:
        if filepath not in seen:
            seen.add(filepath)
            deduped.append((filepath, entry))
    return deduped


def extract_compile_args(entry):
    """Parse compiler args from a compile_commands entry, keeping includes and defines."""
    cmd = entry.get("command", "") or " ".join(entry.get("arguments", []))
    tokens = cmd.split()
    args = []
    skip_next = False
    for i, tok in enumerate(tokens):
        if skip_next:
            skip_next = False
            continue
        if tok.startswith("-I") or tok.startswith("-D") or tok.startswith("-std="):
            args.append(tok)
        elif tok in ("-I", "-D", "-isystem"):
            if i + 1 < len(tokens):
                args.append(tok)
                args.append(tokens[i + 1])
                skip_next = True
        elif tok.startswith("-isystem"):
            args.append(tok)
    # Add common clang flags for C++ parsing
    if not any(t.startswith("-std=") for t in args):
        args.append("-std=c++17")
    return args


def find_function_body_brace(source_bytes, cursor):
    """Find the byte offset of the opening '{' of a function body.

    libclang's cursor extent for a function definition includes the entire
    function (signature + body). We need to find the '{' that starts the
    compound statement body.
    """
    # Look for the compound statement child
    for child in cursor.get_children():
        if child.kind == ci.CursorKind.COMPOUND_STMT:
            # The compound statement starts at the '{'
            offset = child.extent.start.offset
            # Verify it's actually a brace
            if offset < len(source_bytes) and source_bytes[offset:offset + 1] == b'{':
                return offset
    return None


def function_already_traced(source_bytes, body_start, body_end):
    """Check if the function body already contains a TRACE_ macro."""
    body = source_bytes[body_start:body_end].decode("utf-8", errors="replace")
    return bool(re.search(r'\bTRACE_\w+\s*\(', body))


def count_body_lines(source_bytes, body_start, body_end):
    """Count lines in the function body (excluding braces)."""
    body = source_bytes[body_start:body_end]
    return body.count(b'\n')


def count_body_statements(compound_cursor):
    """Count direct child statements/expressions in a compound statement."""
    return sum(1 for _ in compound_cursor.get_children())


def find_insertion_point(source_bytes, brace_offset):
    """Find the byte offset where the macro should be inserted.

    Returns the offset right after the opening '{' and any trailing whitespace
    on the same line, so we insert on the next line.
    """
    pos = brace_offset + 1
    # Skip to end of the line containing '{'
    while pos < len(source_bytes) and source_bytes[pos:pos + 1] in (b' ', b'\t'):
        pos += 1
    if pos < len(source_bytes) and source_bytes[pos:pos + 1] == b'\n':
        pos += 1
    return pos


def detect_indent(source_bytes, brace_offset):
    """Detect the indentation of the line containing the opening brace,
    then add one level (4 spaces)."""
    # Walk back to find start of line
    line_start = brace_offset
    while line_start > 0 and source_bytes[line_start - 1:line_start] != b'\n':
        line_start -= 1
    # Extract leading whitespace
    indent = b""
    pos = line_start
    while pos < brace_offset and source_bytes[pos:pos + 1] in (b' ', b'\t'):
        indent += source_bytes[pos:pos + 1]
        pos += 1
    # Add one indent level
    return indent + b"    "


def ensure_include(source_text, header):
    """If the file doesn't include the header, add it after the last existing #include."""
    include_directive = f'#include "{header}"'
    if include_directive in source_text:
        return source_text, False

    lines = source_text.split('\n')
    last_include_idx = -1
    for i, line in enumerate(lines):
        if line.strip().startswith('#include'):
            last_include_idx = i

    if last_include_idx >= 0:
        lines.insert(last_include_idx + 1, include_directive)
    else:
        # No includes at all — put at top
        lines.insert(0, include_directive)

    return '\n'.join(lines), True


def process_file(filepath, entry, args, index, project_root, source_override=None,
                 ignore_names=None):
    """Process a single source file, returning list of insertions.

    If source_override is provided (as a string), it is used instead of reading
    from disk. libclang also receives it via unsaved_files so the AST matches.
    """
    if ignore_names is None:
        ignore_names = set()
    rel = os.path.relpath(filepath, project_root)
    compile_args = extract_compile_args(entry)

    unsaved = []
    if source_override is not None:
        unsaved = [(filepath, source_override)]

    tu = index.parse(filepath, args=compile_args,
                     unsaved_files=unsaved,
                     options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)

    if not tu:
        print(f"  Warning: failed to parse {filepath}", file=sys.stderr)
        return []

    if source_override is not None:
        source_bytes = source_override.encode("utf-8")
    else:
        with open(filepath, "rb") as f:
            source_bytes = f.read()

    insertions = []  # list of (offset, macro_text_bytes)
    skip_patterns = [re.compile(p) for p in args.skip_pattern]

    def visit(cursor):
        # Only look at function/method definitions in this file
        if cursor.location.file and cursor.location.file.name != filepath:
            return

        is_func_def = (
            cursor.kind in (ci.CursorKind.FUNCTION_DECL, ci.CursorKind.CXX_METHOD,
                            ci.CursorKind.CONSTRUCTOR, ci.CursorKind.DESTRUCTOR,
                            ci.CursorKind.FUNCTION_TEMPLATE)
            and cursor.is_definition()
        )

        if is_func_def:
            short_name = cursor.spelling or cursor.displayname
            # Build qualified name (e.g. Namespace::Class::method)
            parts = []
            parent = cursor.semantic_parent
            while parent and parent.kind != ci.CursorKind.TRANSLATION_UNIT:
                if parent.spelling:
                    parts.append(parent.spelling)
                parent = parent.semantic_parent
            parts.reverse()
            parts.append(short_name)
            qualified_name = "::".join(parts)
            name = qualified_name

            # Skip ignored functions (match against qualified or bare name)
            if name in ignore_names or short_name in ignore_names:
                print(f"    skip {rel}:{name} (in ignore list)")
                return

            # Skip patterns
            for pat in skip_patterns:
                if pat.search(name):
                    print(f"    skip {rel}:{name} (matches pattern '{pat.pattern}')")
                    return

            brace_offset = find_function_body_brace(source_bytes, cursor)
            if brace_offset is None:
                print(f"    skip {rel}:{name} (no body brace found)")
                return

            # Get body extent
            for child in cursor.get_children():
                if child.kind == ci.CursorKind.COMPOUND_STMT:
                    body_end = child.extent.end.offset
                    break
            else:
                print(f"    skip {rel}:{name} (no compound statement)")
                return

            # Skip already-traced
            if function_already_traced(source_bytes, brace_offset, body_end):
                print(f"    skip {rel}:{name} (already traced)")
                return

            # Skip short functions
            if count_body_lines(source_bytes, brace_offset, body_end) < args.min_lines:
                print(f"    skip {rel}:{name} (fewer than {args.min_lines} lines)")
                return

            # Skip single-statement functions (e.g. getters, simple returns)
            if count_body_statements(child) <= 1:
                print(f"    skip {rel}:{name} (single statement)")
                return

            # Compute insertion
            insert_at = find_insertion_point(source_bytes, brace_offset)
            indent = detect_indent(source_bytes, brace_offset)
            cat = category_for_file(filepath, project_root)
            macro_text = args.macro.replace("{cat}", cat)
            macro_line = indent + macro_text.encode("utf-8") + b";\n"
            insertions.append((insert_at, macro_line, name))

        for child in cursor.get_children():
            visit(child)

    visit(tu.cursor)
    return insertions


def apply_insertions(filepath, insertions, include_header, dry_run, source_override=None):
    """Apply all insertions to a file (in reverse offset order to preserve positions)."""
    if source_override is not None:
        source_text = source_override
    else:
        with open(filepath, "r") as f:
            source_text = f.read()

    # Ensure include is present
    source_text, include_added = ensure_include(source_text, include_header)

    source_bytes = source_text.encode("utf-8")

    # Sort by offset descending so earlier inserts don't shift later offsets
    # If include was added, we need to adjust offsets
    if include_added:
        # Recalculate: the include adds bytes. But since insertions were computed
        # on the original file, we need to adjust. The include is added near the
        # top, so all offsets shift by the include line length.
        include_line = f'#include "{include_header}"\n'
        shift = len(include_line.encode("utf-8"))
        insertions = [(off + shift, data, name) for off, data, name in insertions]

    insertions.sort(key=lambda x: x[0], reverse=True)

    for offset, macro_bytes, name in insertions:
        source_bytes = source_bytes[:offset] + macro_bytes + source_bytes[offset:]

    if not dry_run:
        with open(filepath, "wb") as f:
            f.write(source_bytes)

    return include_added


def remove_traces(filepath):
    """Remove all TRACE_ macro lines from a file.

    Returns (cleaned_text, removed) where cleaned_text is the file content
    with traces stripped and removed is a list of (lineno, text) tuples.
    Does NOT write to disk — caller decides whether to persist.
    """
    with open(filepath, "r") as f:
        lines = f.readlines()

    trace_pattern = re.compile(r'^\s*TRACE_\w+\s*\(.*\)\s*;\s*$')
    new_lines = []
    removed = []
    for i, line in enumerate(lines):
        if trace_pattern.match(line):
            removed.append((i + 1, line.rstrip()))
        else:
            new_lines.append(line)

    return "".join(new_lines), removed


def main():
    args = parse_args()
    project_root = os.path.abspath(".")
    build_dir = os.path.join(project_root, args.build_dir)
    ignore_names = load_ignore_list(os.path.join(project_root, args.ignore_file))

    commands = load_compile_commands(build_dir)
    files = get_project_files(commands, project_root, args.only)

    if not files:
        print("No project source files found in compile_commands.json")
        sys.exit(1)

    print(f"Processing {len(files)} source files...")
    print(f"Macro: {args.macro}")
    print(f"Include: {args.include}")
    if ignore_names:
        print(f"Ignore list: {len(ignore_names)} function(s) from {args.ignore_file}")
    if args.remove:
        print("Pre-pass: removing existing TRACE_ macros before inserting")
    if not args.commit:
        print("(dry run — no files will be modified, pass --commit to apply)")
    print()

    # Pre-pass: strip existing TRACE_ lines, keep cleaned content in memory
    cleaned_sources = {}  # filepath -> cleaned text (only for --remove)
    if args.remove:
        total_removals = 0
        for filepath, entry in sorted(files, key=lambda x: x[0]):
            rel = os.path.relpath(filepath, project_root)
            cleaned_text, removed = remove_traces(filepath)

            if removed:
                total_removals += len(removed)
                cleaned_sources[filepath] = cleaned_text
                print(f"  {rel}: {len(removed)} removal(s)")
                for lineno, text in removed:
                    print(f"    - line {lineno}: {text}")
                if args.commit:
                    with open(filepath, "w") as f:
                        f.write(cleaned_text)

        print(f"\nRemoved {total_removals} existing trace(s)")
        print()

    index = ci.Index.create()
    total_insertions = 0
    total_files_modified = 0

    for filepath, entry in sorted(files, key=lambda x: x[0]):
        rel = os.path.relpath(filepath, project_root)
        override = cleaned_sources.get(filepath)
        insertions = process_file(filepath, entry, args, index, project_root,
                                  source_override=override,
                                  ignore_names=ignore_names)

        if insertions:
            total_files_modified += 1
            total_insertions += len(insertions)
            print(f"  {rel}: {len(insertions)} insertion(s)")
            for _, macro_bytes, name in sorted(insertions, key=lambda x: x[0]):
                macro_text = macro_bytes.decode("utf-8").strip()
                print(f"    + {name}: {macro_text}")
            apply_insertions(filepath, insertions, args.include, not args.commit,
                             source_override=override)
        else:
            print(f"  {rel}: (no changes)")

    print()
    print(f"Done: {total_insertions} insertions across {total_files_modified} files")
    if not args.commit:
        print("(dry run — no files were modified)")


if __name__ == "__main__":
    main()
