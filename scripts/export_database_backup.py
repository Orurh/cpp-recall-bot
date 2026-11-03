#!/usr/bin/env python3
"""Root-owned forced-command endpoint: no caller-selected container, SQL or paths."""

import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

CONFIG = Path('/etc/cpp-recall-backup/export.json')
RECEIPT = Path('/var/lib/cpp-recall-external/receipt.json')


def dispatch(config, args):
    if args == ['dump']:
        for key in ('user', 'database'):
            if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,62}', config[key]):
                raise ValueError('invalid_config')
        if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,127}', config['container']):
            raise ValueError('invalid_config')
        # stdout is exclusively pg_dump bytes; errors never disclose stderr/env.
        with (RECEIPT.parent / 'dump.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            process = subprocess.run(['/usr/bin/docker', 'exec', config['container'],
                                      'timeout', '--signal=TERM', '--kill-after=10s', '540s', 'pg_dump',
                                      '--no-password', '-U', config['user'], '-d', config['database'],
                                      '-Fc', '--lock-wait-timeout=15s'],
                                     stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=580)
            return process.returncode
    if len(args) == 4 and args[0] == 'receipt':
        epoch, size, checksum = args[1:]
        if not re.fullmatch(r'\d{10}', epoch) or not re.fullmatch(r'\d{1,15}', size) \
                or not re.fullmatch(r'[a-f0-9]{64}', checksum):
            raise ValueError('invalid_receipt')
        if not 0 <= time.time() - int(epoch) <= 3600 or int(size) < 32:
            raise ValueError('invalid_receipt')
        from pull_database_backup import save
        with (RECEIPT.parent / 'receipt.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            save(RECEIPT, {'backup_verified_epoch': int(epoch), 'backup_bytes': int(size),
                           'sha256': checksum, 'verification': 'offhost_toc_checksum'})
        return 0
    raise ValueError('command_not_allowed')


def main():
    os.umask(0o077)
    # Forced shell script uses `sudo ... exporter.py dump` with validated argv.
    return dispatch(json.loads(CONFIG.read_text()), sys.argv[1:])


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:
        print('backup_export_failed', file=sys.stderr)
        sys.exit(1)
