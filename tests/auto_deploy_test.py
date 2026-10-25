import contextlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

scripts = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(scripts))
spec = importlib.util.spec_from_file_location('auto_deploy', scripts / 'auto_deploy.py')
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)
from check_release_files import validate

REAL_RUN = deploy.run
SHA = 'a' * 40
OTHER = 'b' * 40


def run_record(**changes):
    return {'head_sha': SHA, 'head_branch': 'main', 'event': 'push',
            'path': '.github/workflows/ci.yml', 'status': 'completed', 'conclusion': 'success',
            'repository': {'full_name': deploy.REPOSITORY}, 'run_number': 1, 'run_attempt': 1, **changes}


class FakeHost:
    def __init__(self, root):
        self.root = root
        self.events = []
        self.heads = [SHA, SHA]
        self.is_green = True
        self.fail = None
        self.current = 'old'
        (root / 'scripts').mkdir()
        (root / 'scripts/beta_maintenance.py').write_text('new monitor')

    def action(self, label):
        self.events.append(label)
        if self.fail == label:
            raise deploy.Failure('Simulated deployment failure')

    def head(self):
        return self.heads.pop(0) if len(self.heads) > 1 else self.heads[0]

    def green(self, sha):
        self.action('ci')
        return self.is_green

    def prepare(self, sha):
        self.action('prepare')
        return self.root

    def build(self, sha, release):
        self.action('build')

    def previous(self):
        self.action('previous')
        return {'image': 'sha256:' + 'c' * 64, 'version': 'old'}

    def backup(self):
        self.action('backup')

    def switch(self, image):
        self.action('switch-old' if image.startswith('sha256:') else 'switch-new')
        self.current = 'old' if image.startswith('sha256:') else SHA

    def verify(self, version):
        self.action('verify-old' if version == 'old' else 'verify-new')
        assert self.current == version

    def install_monitor(self, source):
        self.action('install-old' if source.name == 'previous-maintenance.py' else 'install-new')

    def notify(self, message):
        self.events.append('notify')


class DeployTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.monitor = self.root / 'maintenance.py'
        self.monitor.write_text('old monitor')
        self.host = FakeHost(self.root)
        self.runner = deploy.Deployer(self.host, self.root)
        self.run_patch = patch.object(deploy, 'run', return_value=b'')
        self.run = self.enterContext(self.run_patch)
        self.enterContext(patch.object(deploy, 'MAINTENANCE', self.monitor))
        self.enterContext(patch.object(deploy, 'monitor_lock', contextlib.nullcontext))

    def test_only_latest_green_push_for_exact_main_sha_is_allowed(self):
        self.assertTrue(deploy.green_run({'workflow_runs': [run_record()]}, SHA))
        self.assertFalse(deploy.green_run({'workflow_runs': []}, SHA))
        for change in ({'head_sha': OTHER}, {'head_branch': 'feature'}, {'event': 'pull_request'},
                       {'path': '.github/workflows/fake.yml'}, {'status': 'in_progress'},
                       {'conclusion': 'failure'}, {'repository': {'full_name': 'fork/repo'}}):
            self.assertFalse(deploy.green_run({'workflow_runs': [run_record(**change)]}, SHA))
        self.assertFalse(deploy.green_run({'workflow_runs': [run_record(),
            run_record(run_attempt=2, conclusion='failure')]}, SHA))

    def test_green_deploy_is_ordered_and_idempotent(self):
        self.assertEqual(self.runner.tick(), 'deployed')
        self.assertEqual(self.host.events, ['ci', 'prepare', 'build', 'previous', 'backup',
                                          'switch-new', 'verify-new', 'install-new', 'notify'])
        self.assertEqual(self.runner.state['deployed_sha'], SHA)
        self.assertEqual(self.runner.tick(), 'already_deployed')
        self.assertEqual(self.host.events.count('switch-new'), 1)
        self.assertEqual((self.root / 'previous-maintenance.py').read_text(), 'old monitor')
        self.assertEqual((self.root / 'state.json').stat().st_mode & 0o777, 0o600)

    def test_pending_ci_is_cached_without_build_or_backup(self):
        self.host.is_green = False
        self.assertEqual(self.runner.tick(), 'waiting_for_ci')
        self.assertEqual(self.runner.tick(), 'waiting_for_ci')
        self.assertEqual(self.host.events, ['ci'])

    def test_main_changed_during_build_does_not_switch(self):
        self.host.heads = [SHA, OTHER]
        self.assertEqual(self.runner.tick(), 'main_changed_before_switch')
        self.assertNotIn('backup', self.host.events)
        self.assertNotIn('switch-new', self.host.events)

    def test_failed_backup_never_touches_running_image(self):
        self.host.fail = 'backup'
        with self.assertRaises(deploy.Failure):
            self.runner.tick()
        self.assertNotIn('switch-new', self.host.events)
        self.assertEqual(self.host.current, 'old')
        self.assertEqual(self.runner.tick(), 'waiting_before_retry')

    def test_failed_health_restores_image_and_monitor_without_restoring_database(self):
        self.host.fail = 'verify-new'
        with self.assertRaisesRegex(deploy.Failure, 'прежний образ'):
            self.runner.tick()
        self.assertEqual(self.host.events[-4:], ['switch-old', 'install-old', 'verify-old', 'notify'])
        self.assertEqual(self.host.current, 'old')
        self.assertEqual(self.runner.state['phase'], 'idle')
        self.assertEqual(self.runner.state['failed_sha'], SHA)

    def test_interrupted_switch_is_rolled_back_before_another_candidate(self):
        (self.root / 'previous-maintenance.py').write_text('old monitor')
        self.runner.persist(phase='switching', previous=self.host.previous(), failed_sha=SHA, attempts=3)
        self.host.current = SHA
        self.host.events.clear()
        recovered = deploy.Deployer(self.host, self.root)
        self.assertEqual(recovered.tick(), 'failed_waiting_for_new_commit_or_retry')
        self.assertEqual(self.host.events, ['switch-old', 'install-old', 'verify-old', 'notify'])

    def test_retry_limit_and_explicit_operator_retry(self):
        self.runner.persist(failed_sha=SHA, attempts=3)
        self.assertEqual(self.runner.tick(), 'failed_waiting_for_new_commit_or_retry')
        self.assertEqual(self.runner.tick(retry=True), 'deployed')

    def test_rollback_failure_leaves_durable_journal_and_never_reports_success(self):
        (self.root / 'previous-maintenance.py').write_text('old monitor')
        self.runner.persist(phase='switching', previous=self.host.previous(), failed_sha=SHA)
        self.host.fail = 'verify-old'
        with self.assertRaises(deploy.Failure):
            self.runner.tick()
        self.assertEqual(json.loads((self.root / 'state.json').read_text())['phase'], 'rollback_failed')
        self.assertNotIn('deployed_sha', self.runner.state)

    def test_commands_do_not_leak_raw_output(self):
        # Use the real wrapper, not setUp's stub.
        with patch.object(subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, b'', b'token-secret')):
            with self.assertRaises(deploy.Failure) as error:
                REAL_RUN(['false'])
        self.assertNotIn('token-secret', str(error.exception))

    def test_compose_switch_targets_only_bot_and_keeps_existing_profiles(self):
        host = object.__new__(deploy.Host)
        host.root = self.root
        host.switch('sha256:' + 'c' * 64)
        command = self.run.call_args.args[0]
        self.assertEqual(command[-5:], ['up', '-d', '--no-deps', '--no-build', 'bot'])
        self.assertIn(str(self.root / 'deploy/monitoring.compose.yaml'), command)
        self.assertNotIn('down', command)

    def test_build_uses_isolated_compose_context_and_immutable_tag(self):
        host = object.__new__(deploy.Host)
        host.args = SimpleNamespace(state_dir=self.root)
        with patch.object(deploy.shutil, 'disk_usage', return_value=SimpleNamespace(free=32 * 1024 ** 3, total=64 * 1024 ** 3)):
            host.build(SHA, self.root / 'release')
        command = self.run.call_args.args[0]
        self.assertEqual(command[-2:], ['build', 'bot'])
        self.assertNotIn('up', command)
        self.assertEqual(self.run.call_args.kwargs['env']['DOCKER_BUILDKIT'], '1')
        config = json.loads((self.root / 'build-compose.json').read_text())['services']['bot']
        self.assertEqual(config['image'], f'{deploy.IMAGE}:release-{SHA}')
        self.assertEqual(config['build']['context'], str(self.root / 'release'))
        self.assertEqual(config['build']['args']['APP_VERSION'], SHA)

    def test_release_file_policy(self):
        validate(['.env.example', 'src/app.cc', 'tests/auto_deploy_test.py', 'docs/miniapp/index.html'])
        for path in ['.env', '.env.local', 'all-code.txt', 'src/all_code.txt', 'content/all-info.txt',
                     'content/all_json.txt', 'build/app', 'build-debug/app', 'backup.dump', 'key.pem',
                     'scripts/__pycache__/module.pyc']:
            with self.assertRaises(ValueError, msg=path):
                validate([path])


if __name__ == '__main__':
    unittest.main()
