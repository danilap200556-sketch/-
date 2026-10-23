#!/usr/bin/env python3
"""Готовит обновление для программы: zip с приложением и version.json (его читает src/updater.cpp).

Запускается в CI после сборки (см. .github/workflows/build-windows.yml):

  python tools/make_update_package.py --dist dist --zip inventory_manager-windows.zip --manifest version.json \
      --notes release_notes.md --build 57 --sha <полный хеш коммита> [--prev prev_version.json]

version.json: { build, commit, date, file, sha256, size, notes, history: [{build, commit, date, title, notes}, ...] }.
Что нового берётся из сообщений коммитов с прошлой опубликованной сборки (prev: version.json прошлого релиза).
Код возврата 3 - в релизе уже лежит сборка новее этой (две сборки закончились не по порядку): публиковать не нужно.
"""
import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys
import zipfile

KEEP_HISTORY = 30
MAX_COMMITS = 15
MAX_NOTES = 3000
TRAILERS = ('co-authored-by:', 'claude-session:', 'signed-off-by:', 'https://claude.ai/code/session_')


def git(*args, cwd='.'):
    return subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True, encoding='utf-8', errors='replace')


def make_zip(dist, out):
    if not os.path.isdir(dist):
        sys.exit(f'нет папки {dist}')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for root, dirs, files in os.walk(dist):
            dirs.sort()
            for name in sorted(files):
                path = os.path.join(root, name)
                z.write(path, os.path.relpath(path, dist).replace(os.sep, '/'))


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def clean_body(body):
    lines = [l.rstrip() for l in body.splitlines()]
    lines = [l for l in lines if not l.strip().lower().startswith(TRAILERS)]
    return '\n'.join(lines).strip()


def commits_since(prev_commit, sha, repo):
    """[(короткий хеш, заголовок, описание)] от новых к старым."""
    rng = [sha, '-n', '1']
    if prev_commit and git('cat-file', '-e', f'{prev_commit}^{{commit}}', cwd=repo).returncode == 0:
        rng = [f'{prev_commit}..{sha}', '-n', str(MAX_COMMITS)]
    r = git('log', '--no-merges', '--format=%h%x1f%s%x1f%b%x1e', *rng, cwd=repo)
    out = []
    for rec in r.stdout.split('\x1e'):
        rec = rec.strip('\n')
        if not rec.strip():
            continue
        parts = rec.split('\x1f')
        if len(parts) >= 2:
            out.append((parts[0].strip(), parts[1].strip(), clean_body(parts[2] if len(parts) > 2 else '')))
    if not out:  # история недоступна (мелкая копия) - хотя бы хеш
        out = [(sha[:7], 'Обновление программы', '')]
    return out


def notes_text(commits):
    chunks = []
    for _, title, body in commits:
        chunk = f'• {title}'
        if body:
            chunk += '\n' + '\n'.join('  ' + l if l else '' for l in body.splitlines())
        chunks.append(chunk)
    text = '\n'.join(chunks)
    return text if len(text) <= MAX_NOTES else text[:MAX_NOTES].rstrip() + '\n…'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dist', required=True)
    ap.add_argument('--zip', required=True)
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--notes', help='куда записать текст «что нового» для описания релиза (markdown)')
    ap.add_argument('--build', type=int, required=True)
    ap.add_argument('--sha', required=True)
    ap.add_argument('--prev', help='version.json прошлого релиза (если есть)')
    ap.add_argument('--repo', default='.')
    a = ap.parse_args()

    prev = {}
    if a.prev and os.path.isfile(a.prev):
        try:
            with open(a.prev, encoding='utf-8') as f:
                prev = json.load(f)
        except (OSError, ValueError):
            prev = {}
    if int(prev.get('build') or 0) >= a.build:
        print(f'В релизе уже сборка {prev.get("build")} - сборку {a.build} не публикуем')
        return 3

    make_zip(a.dist, a.zip)
    commits = commits_since(prev.get('commit') or '', a.sha, a.repo)
    short = git('rev-parse', '--short=7', a.sha, cwd=a.repo).stdout.strip() or a.sha[:7]
    now = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    notes = notes_text(commits)
    entry = {'build': a.build, 'commit': short, 'date': now, 'title': commits[0][1], 'notes': notes}
    history = [entry] + [h for h in prev.get('history', []) if isinstance(h, dict) and int(h.get('build') or 0) < a.build]
    manifest = {
        'build': a.build, 'commit': short, 'date': now, 'file': os.path.basename(a.zip),
        'sha256': sha256_of(a.zip), 'size': os.path.getsize(a.zip), 'notes': notes, 'history': history[:KEEP_HISTORY],
    }
    with open(a.manifest, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    if a.notes:
        with open(a.notes, 'w', encoding='utf-8') as f:
            f.write(f'Сборка {a.build} ({short}). Программа для Windows: распакуйте архив и запустите inventory_manager.exe; '
                    f'установленная программа обновится сама (вкладка «Обновления»).\n\n{notes}\n')
    print(f'Сборка {a.build} ({short}): {manifest["size"]} байт, sha256 {manifest["sha256"][:12]}…, коммитов в описании: {len(commits)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
