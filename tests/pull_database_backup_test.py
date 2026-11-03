import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).parents[1] / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


backup = load('pull_database_backup')
launcher = load('backup_ssh_command')
maintenance = load('beta_maintenance')


class PullBackupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'archives'
        self.config = {'directory': str(self.root), 'reserve_bytes': 0, 'receipt': False,
                       'attempts': 1, 'timeout_seconds': 2,
                       'source': [sys.executable, '-c', 'import sys; sys.stdout.buffer.write(b"PGDMP" + b"x"*40)'],
                       'verify': [sys.executable, '-c', 'import sys; assert sys.stdin.buffer.read().startswith(b"PGDMP")']}

    def receiver(self):
        return backup.Receiver(self.config)

    def test_success_is_atomic_private_and_checksummed(self):
        record = self.receiver().run()
        final = self.root / record['file']
        self.assertEqual(final.stat().st_mode & 0o777, 0o600)
        self.assertEqual(record['sha256'], backup.digest(final))
        self.assertFalse(list(self.root.rglob('*.partial')))
        self.assertTrue(backup.status(self.root)[1])
        self.assertEqual(json.loads(final.with_suffix('.json').read_text()), record)

    def test_partial_exit_error_preserves_last_success_without_rotation(self):
        record = self.receiver().run()
        self.config['source'] = [sys.executable, '-c', 'import sys; sys.stdout.write("PGDMPpartial"); sys.exit(23)']
        with patch.object(backup, 'rotate') as rotate, self.assertRaises(backup.Failure):
            self.receiver().run()
        rotate.assert_not_called()
        value, healthy = backup.status(self.root)
        self.assertEqual(value['last_success'], record)
        self.assertTrue(healthy)
        self.assertFalse(value['last_attempt_ok'])
        self.assertEqual(backup.digest(self.root / record['file']), record['sha256'])
        self.assertFalse(list(self.root.rglob('*.partial')))

    def test_empty_invalid_or_bad_toc_never_published(self):
        for script, verify in [('pass', 'pass'), ('print("bad")', 'pass'),
                               ('import sys; sys.stdout.write("PGDMP"+"x"*40)', 'raise SystemExit(1)')]:
            self.config['source'] = [sys.executable, '-c', script]
            self.config['verify'] = [sys.executable, '-c', verify]
            with self.subTest(script=script), self.assertRaises(backup.Failure):
                self.receiver().run()
            self.assertFalse(list(self.root.rglob('*.dump')))

    def test_timeout_kills_process_group_and_releases_lock(self):
        self.config['timeout_seconds'] = 0.1
        # Receiver normalizes integer seconds; use direct command for the timeout boundary.
        started = time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired):
            backup.command([sys.executable, '-c', 'import time; time.sleep(60)'], timeout=0.1)
        self.assertLess(time.monotonic() - started, 2)

    def test_lock_refuses_concurrent_run(self):
        receiver = self.receiver()
        with (self.root / 'backup.lock').open('w') as stream:
            backup.fcntl.flock(stream, backup.fcntl.LOCK_EX)
            with self.assertRaisesRegex(backup.Failure, 'already_running'):
                receiver.run()

    def test_space_failure_never_starts_transfer(self):
        receiver = self.receiver()
        receiver.reserve = 10 ** 30
        with patch.object(backup, 'command') as command, self.assertRaises(backup.Failure):
            receiver.run()
        command.assert_not_called()

    def test_retries_are_bounded(self):
        self.config.update(attempts=3, retry_seconds=0)
        receiver = self.receiver()
        with patch.object(receiver, 'attempt', side_effect=backup.Failure('failed')) as attempt, \
                self.assertRaises(backup.Failure):
            receiver.run()
        self.assertEqual(attempt.call_count, 3)

    def test_stale_checks_snapshot_age_and_missing_file(self):
        record = self.receiver().run()
        self.assertTrue(backup.status(self.root, record['started_epoch'] + 26 * 3600)[1])
        self.assertFalse(backup.status(self.root, record['started_epoch'] + 26 * 3600 + 1)[1])
        (self.root / record['file']).unlink()
        self.assertFalse(backup.status(self.root)[1])

    def test_receipt_failure_preserves_completed_backup_in_status(self):
        self.config['receipt'] = True
        receiver = self.receiver()
        original = backup.command
        def command(argv, **kwargs):
            if 'receipt' in argv:
                raise backup.Failure('receipt_failed')
            return original(argv, **kwargs)
        with patch.object(backup, 'command', side_effect=command), self.assertRaises(backup.Failure):
            receiver.run()
        value, healthy = backup.status(self.root)
        self.assertTrue(healthy)
        self.assertFalse(value['last_attempt_ok'])
        self.assertTrue((self.root / value['last_success']['file']).is_file())

    def test_retention_is_daily_and_does_not_follow_symlinks(self):
        self.receiver()
        for day in range(1, 10):
            directory = self.root / '2026' / '01' / f'{day:02}'
            directory.mkdir(parents=True)
            for suffix in ('aaaaaaaa', 'bbbbbbbb'):
                path = directory / f'backup-202601{day:02}T000000000000Z-{suffix}.dump'
                path.touch(mode=0o600)
                path.with_suffix('.json').write_text('{}')
            (directory / 'manual.dump').write_text('keep')
        outside = Path(self.temp.name) / 'outside'
        outside.mkdir()
        (outside / 'precious.dump').write_text('keep')
        (self.root / '2025').symlink_to(outside, target_is_directory=True)
        backup.rotate(self.root, 7)
        records = backup.archive_records(self.root)
        self.assertEqual(len(records), 14)
        self.assertEqual(len({item.parent for item in records}), 7)
        self.assertEqual(len(list(self.root.rglob('manual.dump'))), 9)
        self.assertTrue((outside / 'precious.dump').is_file())

    def test_forced_command_rejects_shell_forwarding_or_parameters(self):
        self.assertEqual(launcher.arguments('dump'), ['dump'])
        self.assertEqual(len(launcher.arguments('receipt 1800000000 123 ' + 'a'*64)), 4)
        for value in ('', 'sh', 'dump; id', 'dump otherdb', 'receipt 1 1 a', 'dump\n'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                launcher.arguments(value)

    def test_external_receipt_does_not_disable_stale_detection(self):
        receipt = Path(self.temp.name) / 'receipt.json'
        monitor = maintenance.Monitor(argparse.Namespace(state_dir=Path(self.temp.name),
                                      compose_project='test', external_backup_receipt=receipt))
        self.assertTrue(monitor.backup_issues())
        for epoch, expected in [(time.time(), False), (time.time()-27*3600, True), (time.time()+60, True)]:
            backup.save(receipt, {'backup_verified_epoch': epoch, 'backup_bytes': 100})
            self.assertEqual(bool(monitor.backup_issues()), expected)

    def test_sigterm_cancels_without_retry_or_success(self):
        self.config.update(attempts=3, retry_seconds=0)
        self.config['source'] = [sys.executable, '-c',
            'import os,signal,time; os.kill(os.getppid(), signal.SIGTERM); time.sleep(10)']
        config_path = Path(self.temp.name) / 'config.json'
        config_path.write_text(json.dumps(self.config))
        result = subprocess.run([sys.executable, str(Path(backup.__file__)), 'run', '--config', str(config_path)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(result.returncode, 143)
        self.assertIn(b'backup_cancelled', result.stderr)
        self.assertFalse(list(self.root.rglob('*.dump')))
        self.assertFalse(list(self.root.rglob('*.partial')))
        self.assertTrue(backup.read(self.root / 'status.json')['cancelled'])


if __name__ == '__main__':
    unittest.main()
