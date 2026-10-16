#!/usr/bin/env python3
"""Apply the standalone Rock to Fakie source patch to an exact supported ReSkate checkout."""
import argparse
import hashlib
import json
import shutil
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent

def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def update(repo: Path, dry_run: bool):
    if not (repo/'CMakeLists.txt').is_file():
        raise ValueError('This does not look like a ReSkate checkout: CMakeLists.txt missing')
    manifest = json.loads((HERE/'source_patch/manifest.json').read_text(encoding='utf-8'))['files']
    pending=[]
    for rel, checksums in manifest.items():
        payload = HERE/'source_patch/payload'/rel
        dest = repo/rel
        if not payload.is_file() or not dest.is_file():
            raise ValueError(f'Missing source file {rel}. Nothing changed.')
        data=payload.read_bytes()
        if sha(data) != checksums['after']:
            raise ValueError(f'Package integrity mismatch: {rel}. Nothing changed.')
        current=sha(dest.read_bytes())
        if current==checksums['after']:
            print('ALREADY',rel)
        elif current==checksums['before']:
            print('UPDATE ',rel)
            pending.append((rel,dest,data))
        else:
            raise ValueError(f'Unsupported source revision for {rel}: {current}. Nothing changed. Only v1.4.41 or v1.4.42 supported.')
    if dry_run:
        print(f'DRY RUN PASSED ({len(pending)} files to change; no writes)')
        return
    if not pending:
        print('ALREADY APPLIED')
        return
    git=repo/'.git'
    if not git.is_dir():
        raise ValueError('Expected a Git checkout with .git directory for safe backups; nothing changed.')
    backups=git/'rock-to-fakie-source-backups'/datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    backups.mkdir(parents=True)
    done=[]
    try:
        for rel,dest,data in pending:
            backup=backups/rel
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(dest,backup)
            temp=dest.with_name(dest.name+'.r2f-staging')
            try:
                temp.write_bytes(data)
                temp.replace(dest)
            finally:
                temp.unlink(missing_ok=True)
            done.append((dest,backup))
        print('UPDATED SOURCE:',len(done),'file(s)')
        print('BACKUPS:',backups)
        print('Review git diff, then rebuild. No Git commit/push performed.')
    except Exception:
        for dest,backup in reversed(done):
            shutil.copy2(backup,dest)
        raise

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('reskate_checkout',type=Path)
    p.add_argument('--dry-run',action='store_true')
    args=p.parse_args()
    try:
        update(args.reskate_checkout.expanduser().resolve(),args.dry_run)
    except (OSError,ValueError) as e:
        print('ERROR:',e,file=sys.stderr)
        raise SystemExit(1)
if __name__=='__main__':
    main()
