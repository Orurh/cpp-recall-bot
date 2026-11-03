#!/usr/bin/env python3
"""Pull a streamed custom archive. Configuration is trusted, administrator-owned JSON."""

import argparse
import contextlib
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import signal
import stat
import subprocess
import sys
import time
import uuid


class Failure(Exception):
    """Fixed messages only; subprocess stderr can contain private data."""


class Cancelled(Failure):
    """Operator cancellation is not a transient transport error."""


NAME = re.compile(r'backup-\d{8}T\d{12}Z-[a-f0-9]{8}\.dump')


def fsync_directory(path):
    fd = os.open(path, os.O_DIRECTORY | os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def save(path, value):
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.partial')
    try:
        fd = os.open(temporary, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream, sort_keys=True)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        fsync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def read(path):
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        return {}


def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            checksum.update(chunk)
    return checksum.hexdigest()


def private_directory(path):
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    details = path.lstat()
    if not stat.S_ISDIR(details.st_mode) or details.st_uid != os.geteuid() or details.st_mode & 0o077:
        raise Failure('unsafe_backup_directory')


def command(argv, *, stdin=None, stdout=subprocess.DEVNULL, timeout=600, limit=None):
    def limits():
        if limit is not None:
            resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))
    process = subprocess.Popen(argv, stdin=stdin or subprocess.DEVNULL, stdout=stdout,
                               stderr=subprocess.DEVNULL, start_new_session=True,
                               preexec_fn=limits if limit is not None else None)
    try:
        if process.wait(timeout=timeout):
            raise Failure('command_failed')
    finally:
        # Also terminate orphaned pipeline children, not just their shell.
        with contextlib.suppress(ProcessLookupError, PermissionError):
            os.killpg(process.pid, signal.SIGKILL)
        # Privileged fixed verifiers must also enforce their own shorter timeout.
        with contextlib.suppress(subprocess.TimeoutExpired):
            process.wait(timeout=10)


def archive_records(root):
    """Only our regular files in real date directories; never follow symlinks."""
    records = []
    for year in root.iterdir():
        if not re.fullmatch(r'\d{4}', year.name) or year.is_symlink() or not year.is_dir():
            continue
        for month in year.iterdir():
            if not re.fullmatch(r'\d{2}', month.name) or month.is_symlink() or not month.is_dir():
                continue
            for day in month.iterdir():
                if not re.fullmatch(r'\d{2}', day.name) or day.is_symlink() or not day.is_dir():
                    continue
                for path in day.iterdir():
                    if NAME.fullmatch(path.name) and stat.S_ISREG(path.lstat().st_mode):
                        meta = path.with_suffix('.json')
                        if meta.is_file() and not meta.is_symlink():
                            records.append(path)
    return sorted(records)


def rotate(root, days):
    records = archive_records(root)
    retained = sorted({path.parent for path in records})[-days:]
    # Keep every successful run within the retained days. Unknown files remain untouched.
    for path in records:
        if path.parent not in retained:
            path.unlink()
            path.with_suffix('.json').unlink()
            fsync_directory(path.parent)


def status(root, now=None):
    now = time.time() if now is None else now
    value = read(root / 'status.json')
    latest = value.get('last_success', {})
    age = now - latest.get('started_epoch', 0)
    healthy = bool(latest) and 0 <= age <= 26 * 3600
    if latest:
        path = root / latest['file']
        healthy = healthy and not path.is_symlink() and path.is_file() and path.stat().st_size == latest['bytes']
    return {**value, 'age_seconds': int(age), 'stale': not healthy}, healthy


class Receiver:
    def __init__(self, config):
        self.config = config
        self.root = Path(config['directory'])
        private_directory(self.root)
        self.timeout = int(config.get('timeout_seconds', 600))
        self.reserve = int(config.get('reserve_bytes', 1024 ** 3))

    def attempt(self):
        started = time.time()
        date = dt.datetime.fromtimestamp(started, dt.timezone.utc)
        target = self.root / date.strftime('%Y/%m/%d')
        for directory in (target.parent.parent, target.parent, target):
            private_directory(directory)
        usage = os.statvfs(self.root)
        available = usage.f_bavail * usage.f_frsize
        previous = read(self.root / 'status.json').get('last_success', {})
        if available < self.reserve + max(previous.get('bytes', 0) * 2, 1024 ** 2):
            raise Failure('insufficient_space')
        name = 'backup-' + date.strftime('%Y%m%dT%H%M%S%fZ') + '-' + uuid.uuid4().hex[:8] + '.dump'
        final = target / name
        partial = final.with_suffix('.dump.partial')
        try:
            fd = os.open(partial, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            with os.fdopen(fd, 'wb') as stream:
                command(self.config['source'] + ['dump'], stdout=stream,
                        timeout=self.timeout, limit=available - self.reserve)
                stream.flush()
                os.fsync(stream.fileno())
            size = partial.stat().st_size
            with partial.open('rb') as stream:
                header = stream.read(5)
            if size < 32 or header != b'PGDMP':
                raise Failure('empty_or_invalid_archive')
            checksum = digest(partial)
            with partial.open('rb') as stream:
                command(self.config['verify'], stdin=stream, timeout=self.timeout)
            # Re-read to ensure that the file on disk still matches the measured stream.
            if partial.stat().st_size != size or digest(partial) != checksum:
                raise Failure('checksum_mismatch')
            record = {'file': str(final.relative_to(self.root)), 'bytes': size, 'sha256': checksum,
                      'started_epoch': started, 'completed_epoch': time.time(), 'toc_verified': True}
            save(final.with_suffix('.json'), record)
            os.replace(partial, final)
            fsync_directory(target)
            return record
        finally:
            partial.unlink(missing_ok=True)

    def run(self):
        lock_fd = os.open(self.root / 'backup.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        with os.fdopen(lock_fd, 'w') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise Failure('backup_already_running') from error
            old = read(self.root / 'status.json')
            for attempt in range(int(self.config.get('attempts', 3))):
                try:
                    record = self.attempt()
                    old = {'last_success': record, 'last_attempt_ok': True}
                    save(self.root / 'status.json', old)
                    rotate(self.root, int(self.config.get('retain_days', 7)))
                    break
                except Cancelled:
                    save(self.root / 'status.json', {**old, 'last_attempt_ok': False, 'cancelled': True})
                    raise
                except (Failure, OSError, subprocess.TimeoutExpired):
                    save(self.root / 'status.json', {**old, 'last_attempt_ok': False,
                                                  'last_attempt_epoch': time.time()})
                    print('backup_attempt_failed', file=sys.stderr, flush=True)
                    if attempt + 1 < int(self.config.get('attempts', 3)):
                        time.sleep(int(self.config.get('retry_seconds', 30)))
            else:
                raise Failure('backup_failed_previous_copies_preserved')
            # Retry acknowledgment independently: a failed receipt must not create more dumps.
            if self.config.get('receipt', True):
                for attempt in range(int(self.config.get('attempts', 3))):
                    try:
                        command(self.config['source'] + ['receipt', str(int(record['started_epoch'])),
                                str(record['bytes']), record['sha256']], timeout=30)
                        break
                    except Cancelled:
                        save(self.root / 'status.json', {**old, 'last_attempt_ok': False, 'cancelled': True})
                        raise
                    except (Failure, OSError, subprocess.TimeoutExpired):
                        if attempt + 1 < int(self.config.get('attempts', 3)):
                            time.sleep(int(self.config.get('retry_seconds', 30)))
                else:
                    save(self.root / 'status.json', {**old, 'last_attempt_ok': False,
                                                   'receipt_delivered': False})
                    raise Failure('archive_verified_receipt_failed')
            print(json.dumps({'backup': 'verified', 'bytes': record['bytes'], 'sha256': record['sha256']}))
            return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('run', 'status'))
    parser.add_argument('--config', type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    def terminate(signum, frame):
        raise Cancelled('backup_interrupted')
    signal.signal(signal.SIGTERM, terminate)
    config = read(args.config)
    if args.action == 'status':
        result, healthy = status(Path(config['directory']))
        print(json.dumps(result, sort_keys=True))
        return 0 if healthy and result.get('last_attempt_ok') else 1
    Receiver(config).run()
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Cancelled:
        print('backup_cancelled', file=sys.stderr)
        sys.exit(143)
    except Exception:
        print('backup_failed_check_private_status_and_service', file=sys.stderr)
        sys.exit(1)
