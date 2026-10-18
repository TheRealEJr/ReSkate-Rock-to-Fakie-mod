#!/usr/bin/env python3
"""Apply the Rock to Fakie source patch to a ReSkate v2.1.0 checkout.

Every file is checked against source_patch/manifest.json before anything is written:
a changed file must match ReSkate v2.1.0 exactly, and a new file must not exist yet
(or already be the Rock to Fakie version). Line endings (LF or CRLF) do not matter.
Originals are backed up under .git/rock-to-fakie-source-backups/ before they are replaced.
"""
import argparse
import hashlib
import json
import shutil
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent


def sha(data: bytes) -> str:
    # Hash with LF line endings, so a checkout with CRLF endings still matches.
    return hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest()


def uses_crlf(data: bytes) -> bool:
    return b'\r\n' in data


def update(repo: Path, dry_run: bool):
    if not (repo / 'CMakeLists.txt').is_file():
        raise ValueError('This does not look like a ReSkate checkout: CMakeLists.txt missing. Nothing changed.')
    manifest = json.loads((HERE / 'source_patch/manifest.json').read_text(encoding='utf-8'))
    crlf = uses_crlf((repo / 'CMakeLists.txt').read_bytes())
    pending = []
    for rel, checksums in manifest['files'].items():
        payload = HERE / 'source_patch/payload' / rel
        dest = repo / rel
        if not payload.is_file():
            raise ValueError(f'Missing payload file {rel}. Nothing changed.')
        data = payload.read_bytes()
        if sha(data) != checksums['after']:
            raise ValueError(f'Package integrity mismatch: {rel}. Nothing changed.')
        if dest.is_file():
            current = sha(dest.read_bytes())
            if current == checksums['after']:
                print('ALREADY', rel)
                continue
            if checksums['before'] is None or current != checksums['before']:
                raise ValueError(f'{rel} is not the ReSkate v2.1.0 version ({current}). Nothing changed. '
                                 'Start from a clean ReSkate v2.1.0 checkout.')
            print('UPDATE ', rel)
        else:
            if checksums['before'] is not None:
                raise ValueError(f'{rel} is missing from the checkout. Nothing changed.')
            print('ADD    ', rel)
        if crlf:
            data = data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
        pending.append((rel, dest, data))
    if dry_run:
        print(f'DRY RUN PASSED ({len(pending)} files to write; nothing written)')
        return
    if not pending:
        print('ALREADY APPLIED')
        return
    git = repo / '.git'
    backups = (git if git.is_dir() else repo) / 'rock-to-fakie-source-backups' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    done = []
    try:
        for rel, dest, data in pending:
            existed = dest.is_file()
            if existed:
                backup = backups / rel
                backup.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dest, backup)
            dest.parent.mkdir(parents=True, exist_ok=True)
            temp = dest.with_name(dest.name + '.r2f-staging')
            try:
                temp.write_bytes(data)
                temp.replace(dest)
            finally:
                temp.unlink(missing_ok=True)
            done.append((dest, backups / rel if existed else None))
        print('UPDATED SOURCE:', len(done), 'file(s)')
        if backups.exists():
            print('BACKUPS:', backups)
        print('Review the changes, then build. Nothing was committed or pushed.')
    except Exception:
        for dest, backup in reversed(done):
            if backup is not None:
                shutil.copy2(backup, dest)
            else:
                dest.unlink(missing_ok=True)
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('reskate_checkout', type=Path)
    p.add_argument('--dry-run', action='store_true')
    args = p.parse_args()
    try:
        update(args.reskate_checkout.expanduser().resolve(), args.dry_run)
    except (OSError, ValueError) as e:
        print('ERROR:', e, file=sys.stderr)
        raise SystemExit(1)


if __name__ == '__main__':
    main()
