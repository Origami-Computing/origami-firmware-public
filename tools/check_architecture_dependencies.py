#!/usr/bin/env python3
"""Check project include directions; no SDK, compiler, device or network required.

Examines single-line literal project includes in all preprocessor branches.
SDK/library headers, macro/continued includes and runtime callback dependencies are outside
this check. It complements compilation and ownership review; it cannot prove them.
"""
from __future__ import annotations
import argparse
from collections import deque
from dataclasses import dataclass
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SUFFIXES = {'.c', '.cpp', '.h', '.hpp', '.inc', '.tpp'}
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*(["<])([^">]+)[">]', re.MULTILINE)
LITERAL = r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
TOKENS = re.compile(LITERAL + r'|/\*.*?\*/|//[^\n]*', re.DOTALL)
STRINGS = re.compile(LITERAL, re.DOTALL)
INCLUDE_PREFIX = re.compile(r'^[ \t]*#[ \t]*include[ \t]*$')
PROTECTED = {'core', 'services', 'sfx'}
PRESENTATION = {'apps', 'ui', 'scene'}


@dataclass(frozen=True)
class Edge:
    source: Path
    target: Path
    line: int


def area(path: Path) -> str:
    return path.parts[1] if len(path.parts) > 2 and path.parts[0] in {'src', 'include'} else ''


def forbidden(origin: Path, target: Path) -> bool:
    if area(target) in PRESENTATION or target in {
        Path('include/app.h'), Path('include/nav_handler.h'), Path('src/nav_handler.cpp')
    }:
        return True
    return area(origin) == 'core' and (
        area(target) == 'services' or target == Path('include/services.h')
    )


def mask_non_directives(text: str) -> str:
    def blank(value: str) -> str:
        return ''.join('\n' if char == '\n' else ' ' for char in value)
    text = TOKENS.sub(lambda m: blank(m[0]) if m[0].startswith(('//', '/*')) else m[0], text)
    def literal(match: re.Match) -> str:
        prefix = text[text.rfind('\n', 0, match.start())+1:match.start()]
        return match[0] if match[0].startswith('"') and INCLUDE_PREFIX.fullmatch(prefix) else blank(match[0])
    return STRINGS.sub(literal, text)


def include_graph(root: Path) -> dict[Path, list[Edge]]:
    root = root.resolve()
    files = sorted(p for base in ('src', 'include') for p in (root/base).rglob('*')
                   if p.is_file() and p.suffix in SOURCE_SUFFIXES)
    graph: dict[Path, list[Edge]] = {}
    for source in files:
        relative = source.relative_to(root)
        # Preserve line numbers so diagnostics point at the original source.
        text = mask_non_directives(source.read_text(encoding='utf-8'))
        graph[relative] = []
        for match in INCLUDE.finditer(text):
            delimiter, name = match.groups()
            candidates = ([source.parent/name] if delimiter == '"' else []) + [
                root/'include'/name, root/'src'/name, root/name]
            for candidate in candidates:
                candidate = candidate.resolve()
                if not candidate.is_file() or not candidate.is_relative_to(root):
                    continue
                target = candidate.relative_to(root)
                if target.parts[0] not in {'src', 'include'}:
                    break  # Third-party library: not a project-layer policy.
                graph[relative].append(Edge(relative, target, text.count('\n', 0, match.start())+1))
                break
    return graph


def violations(graph: dict[Path, list[Edge]]) -> list[tuple[Path, list[Edge]]]:
    failures = []
    for origin in sorted(p for p in graph if area(p) in PROTECTED):
        pending = deque([(origin, [])])
        visited = {origin}
        while pending:
            node, chain = pending.popleft()
            for edge in graph.get(node, []):
                if edge.target in visited:
                    continue
                visited.add(edge.target)
                path = chain+[edge]
                if forbidden(origin, edge.target):
                    failures.append((origin, path))
                else:
                    pending.append((edge.target, path))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    graph = include_graph(args.root)
    protected = [p for p in graph if area(p) in PROTECTED]
    if not protected:
        parser.error('No core/services/sfx source files found; check --root')
    failures = violations(graph)
    for origin, chain in failures:
        print(f'ERROR {origin}: forbidden include dependency')
        for edge in chain:
            print(f'  {edge.source}:{edge.line} -> {edge.target}')
    if failures:
        return 1
    print(f'PASS {len(protected)} core/services/SFX files: no presentation imports; core has no service imports (including transitive project headers)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
