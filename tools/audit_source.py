"""Inventory first-party code and validate local MSBuild/RCC paths. Read only."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET
from urllib.parse import unquote

EXTENSIONS = {'.c', '.cpp', '.h', '.inc', '.qml', '.py', '.ps1', '.sh', '.awk', '.cmd'}
ROOTS = ('sensors', 'sdk', 'tests', 'qml', 'tools')
C_TOKEN = re.compile(r'R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/')


def sloc(path):
    text = path.read_text(encoding='utf-8-sig')
    if path.suffix in {'.c', '.cpp', '.h', '.inc', '.qml'}:
        text = C_TOKEN.sub(lambda m: '\n' * m[0].count('\n') if m[0].startswith(('//', '/*')) else m[0], text)
        return sum(bool(line.strip()) for line in text.splitlines())
    if path.suffix == '.ps1':
        text = re.sub(r'<#[\s\S]*?#>', lambda m: '\n' * m[0].count('\n'), text)
    return sum(bool(line.strip()) and not line.lstrip().startswith(('#', 'rem ', 'REM ', '::'))
               for line in text.splitlines())


def category(relative):
    if relative.startswith('tests/'):
        return 'tests'
    if relative.startswith('tools/rv_capture/') and ('/runtime/' in relative or '/common/' in relative):
        return 'board_runtime'
    if relative.startswith('tools/') and Path(relative).suffix not in {'.cpp', '.h', '.inc'}:
        return 'scripts'
    return 'application_and_native_support'


def audit(root):
    paths = [p for p in root.iterdir() if p.is_file() and p.suffix in EXTENSIONS]
    paths += [p for directory in ROOTS for p in (root / directory).rglob('*')
              if p.is_file() and p.suffix in EXTENSIONS and '__pycache__' not in p.parts]
    rows = []
    groups = collections.defaultdict(lambda: {'files': 0, 'physical_lines': 0, 'code_lines': 0})
    for path in sorted(paths):
        rel = path.relative_to(root).as_posix()
        data = path.read_bytes()
        row = {'path': rel, 'group': category(rel), 'physical_lines': len(data.splitlines()),
               'code_lines': sloc(path), 'sha256': hashlib.sha256(data).hexdigest()}
        rows.append(row)
        group = groups[row['group']]
        group['files'] += 1
        for key in ('physical_lines', 'code_lines'):
            group[key] += row[key]
    issues = []
    project_entries = {}
    projects = list(root.glob('*.vcxproj')) + list((root / 'tools').rglob('*.vcxproj'))
    for project in projects:
        entries = set()
        for element in ET.parse(project).iter():
            kind = element.tag.rsplit('}', 1)[-1]
            name = element.get('Include', '')
            if kind not in {'ClCompile', 'ClInclude', 'QtMoc', 'QtRcc', 'ResourceCompile', 'None'} or not name:
                continue
            entries.add((kind, name))
            if '$(' not in name and not (project.parent / name.replace('\\', '/')).is_file():
                issues.append({'file': str(project.relative_to(root)), 'missing': name})
        project_entries[project.relative_to(root).as_posix()] = len(entries)
        filters = Path(str(project) + '.filters')
        if filters.exists():
            filtered = {(e.tag.rsplit('}', 1)[-1], e.get('Include')) for e in ET.parse(filters).iter()
                        if e.tag.rsplit('}', 1)[-1] != 'Filter' and e.get('Include')}
            for entry in sorted(entries - filtered):
                issues.append({'file': filters.relative_to(root).as_posix(), 'not_in_filters': entry})
            for entry in sorted(filtered - entries):
                issues.append({'file': filters.relative_to(root).as_posix(), 'not_in_project': entry})
    for resource in root.glob('*.qrc'):
        for element in ET.parse(resource).iter('file'):
            if not (resource.parent / element.text).is_file():
                issues.append({'file': resource.name, 'missing': element.text})
    historical_warnings = []
    documents = list(root.glob('*.md')) + list((root / 'docs').rglob('*.md')) + list((root / 'tools').rglob('*.md'))
    documents += list((root / 'archives').glob('README.md')) + list((root / 'archives/cad/tactile').glob('README.md'))
    for document in documents:
        for name in re.findall(r'\]\(([^)]+)\)', document.read_text(encoding='utf-8-sig')):
            name = unquote(name.strip('<>').split('#', 1)[0])
            if not name or re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:', name):
                continue
            target = (document.parent / name).resolve()
            if not target.exists():
                row = {'file': document.relative_to(root).as_posix(), 'missing': name}
                if 'x64' in target.parts and 'validation' in target.parts:
                    historical_warnings.append(row)
                else:
                    issues.append(row)
    cad_checks = []
    layout = root / 'models/tactile/surface_layouts.json'
    for sensor in json.loads(layout.read_text(encoding='utf-8'))['sensors']:
        for name, expected in sensor['source_sha256'].items():
            source = (layout.parent / sensor['source_directory'] / name).resolve()
            passed = source.is_file() and hashlib.sha256(source.read_bytes()).hexdigest() == expected
            cad_checks.append({'sensor': sensor['id'], 'path': str(source), 'pass': passed})
            if not passed:
                issues.append({'file': layout.relative_to(root).as_posix(), 'missing_or_changed_cad': str(source)})
    return {'root': str(root), 'definition': 'Physical nonblank, non-comment source lines; braces, directives and string literals count. Excludes third_party, dependency_sources, models, assets, generated files, runtime packages and x64. No reachability claim.',
            'groups': dict(groups), 'total_files': len(rows), 'total_code_lines': sum(r['code_lines'] for r in rows),
            'project_entries': project_entries, 'path_issues': issues,
            'historical_path_warnings': historical_warnings, 'cad_checks': cad_checks, 'files': rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.root.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'files'}, ensure_ascii=False, indent=2))
    return 1 if result['path_issues'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
