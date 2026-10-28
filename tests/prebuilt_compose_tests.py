"""Read-only checks: requires Docker Compose CLI, never contacts the daemon."""
import json
import os
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]


def config(*, release=False, missing=None, host=False):
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith('COMPOSE_')
    }
    environment.update({
        'CPP_RECALL_IMAGE': 'ghcr.io/orurh/cpp-recall-bot@sha256:' + 'a' * 64,
        'TELEGRAM_BOT_TOKEN': 'validation-only',
        'POSTGRES_PASSWORD': 'validation-only',
        'POSTGRES_USER': 'cpp_recall',
        'POSTGRES_DB': 'cpp_recall_bot',
        'MINI_APP_URL': 'https://example.invalid',
        'DATABASE_POOL_SIZE': '2',
        'MINI_APP_WORKER_COUNT': '2',
        'MINI_APP_FEEDBACK_ADMIN_ID': '',
        'MINI_APP_AUTH_MAX_AGE_SECONDS': '86400',
        'POSTGRES_IMAGE': 'postgres@sha256:' + 'b' * 64,
        'MINI_APP_PRIVATE_ADDRESS': '10.30.0.10',
    })
    if missing:
        environment.pop(missing, None)
    command = ['docker', 'compose', '--env-file', '/dev/null',
               '-f', 'deploy/prebuilt/compose.yaml']
    if host:
        command += ['-f', 'deploy/prebuilt/host.compose.yaml']
    if release:
        command += ['--profile', 'release']
    return subprocess.run(command + ['config', '--format', 'json'],
                          cwd=ROOT, env=environment, capture_output=True,
                          text=True, timeout=30, check=False)


class PrebuiltComposeTests(unittest.TestCase):
    def model(self, release=False):
        result = config(release=release)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_default_does_not_enable_bot(self):
        model = self.model()
        self.assertEqual(model['name'], 'cpp-recall-germany')
        self.assertEqual(set(model['services']), {'postgres'})

    def test_release_has_image_without_build_or_home_proxy(self):
        model = self.model(release=True)
        self.assertEqual(set(model['services']), {'postgres', 'bot'})
        bot = model['services']['bot']
        self.assertNotIn('build', bot)
        self.assertIn('@sha256:', bot['image'])
        self.assertEqual(bot['platform'], 'linux/amd64')
        self.assertNotIn('ALL_PROXY', bot['environment'])
        self.assertEqual(set(model['networks']), {'default'})

    def test_only_loopback_http_is_published(self):
        services = self.model(release=True)['services']
        self.assertFalse(services['postgres'].get('ports'))
        ports = services['bot']['ports']
        self.assertEqual(len(ports), 1)
        self.assertEqual(ports[0]['host_ip'], '127.0.0.1')
        self.assertEqual(str(ports[0]['published']), '8081')
        self.assertEqual(ports[0]['target'], 8080)

    def test_database_volume_requires_separate_creation(self):
        volume = self.model()['volumes']['postgres_data']
        self.assertTrue(volume['external'])
        self.assertEqual(volume['name'], 'cpp_recall_germany_postgres_data')

    def test_missing_required_values_fail_closed(self):
        for name in ('CPP_RECALL_IMAGE', 'TELEGRAM_BOT_TOKEN',
                     'POSTGRES_PASSWORD', 'MINI_APP_URL'):
            with self.subTest(name=name):
                result = config(release=True, missing=name)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(name, result.stderr)

    def test_external_proxy_uses_private_and_loopback_interfaces_only(self):
        result = config(release=True, host=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        model = json.loads(result.stdout)
        postgres, bot = model['services']['postgres'], model['services']['bot']
        self.assertFalse(postgres.get('ports'))
        self.assertEqual({port['host_ip'] for port in bot['ports']}, {'127.0.0.1', '10.30.0.10'})
        self.assertEqual(postgres['image'], 'postgres@sha256:' + 'b' * 64)
        self.assertEqual(bot['pull_policy'], 'never')
        self.assertEqual(postgres['pull_policy'], 'never')
        self.assertTrue(bot['volumes'][0]['read_only'])

    def test_external_proxy_requires_explicit_private_address_and_database_pin(self):
        for name in ('MINI_APP_PRIVATE_ADDRESS', 'POSTGRES_IMAGE'):
            with self.subTest(name=name):
                result = config(release=True, host=True, missing=name)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(name, result.stderr)


if __name__ == '__main__':
    unittest.main()
