import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

scripts = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(scripts))
spec = importlib.util.spec_from_file_location('prebuilt_deploy', scripts / 'prebuilt_deploy.py')
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)
SHA = 'a' * 40
DIGEST = deploy.REGISTRY + '@sha256:' + 'b' * 64
IMAGE_ID = 'sha256:' + 'c' * 64


def record():
    return {'Id': IMAGE_ID, 'Architecture': 'amd64', 'RepoDigests': [DIGEST], 'Config': {'Labels': {
        'org.opencontainers.image.revision': SHA,
        'org.opencontainers.image.source': f'https://github.com/{deploy.REPOSITORY}'}}}


class PrebuiltDeploymentTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.env = self.root / '.env'
        self.env.write_text('TELEGRAM_BOT_TOKEN=do-not-print\nCPP_RECALL_IMAGE=old\nDATABASE_POOL_SIZE=2\n')
        self.host = object.__new__(deploy.PrebuiltHost)
        self.host.root = self.root
        self.host.args = SimpleNamespace(compose_project='cpp-recall-germany')
        self.host.candidates = {}

    def test_digest_requires_exact_source_revision_and_architecture(self):
        self.assertEqual(deploy.reviewed_digest(record(), SHA), DIGEST)
        for changed in ({'Architecture': 'arm64'}, {'RepoDigests': []},
                        {'RepoDigests': ['other/repo@sha256:' + 'b' * 64]}, {'Config': {}},
                        {'RepoDigests': [DIGEST, deploy.REGISTRY + '@sha256:' + 'd' * 64]}):
            with self.subTest(changed=changed), self.assertRaises(deploy.Failure):
                deploy.reviewed_digest({**record(), **changed}, SHA)
        with self.assertRaises(deploy.Failure):
            deploy.reviewed_digest(record(), 'd' * 40)

    def test_download_checks_pin_without_invoking_build(self):
        def response(command, **kwargs):
            return b'' if command[1] == 'pull' else json.dumps([record()]).encode()
        with patch.object(deploy, 'run', side_effect=response) as run:
            self.host.build(SHA, self.root)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(commands[0], ['docker', 'pull', '--platform', 'linux/amd64', deploy.REGISTRY + ':' + SHA])
        self.assertEqual(commands[-1], ['docker', 'image', 'inspect', DIGEST])
        self.assertFalse(any('build' in command for command in commands))
        self.assertEqual(self.host.candidates[SHA], DIGEST)

    def test_changed_image_is_rejected_before_switch(self):
        answers = [b'', json.dumps([record()]).encode(), json.dumps([{**record(), 'Id': 'changed'}]).encode()]
        with patch.object(deploy, 'run', side_effect=answers), self.assertRaises(deploy.Failure):
            self.host.build(SHA, self.root)
        self.assertFalse(self.host.candidates)

    def test_low_disk_does_not_download_or_change_configuration(self):
        before = self.env.read_text()
        with patch.object(deploy.shutil, 'disk_usage', return_value=SimpleNamespace(free=1, total=100)), \
                patch.object(deploy, 'run') as run, self.assertRaises(deploy.Failure):
            self.host.build(SHA, self.root)
        run.assert_not_called()
        self.assertEqual(self.env.read_text(), before)

    def test_image_change_is_private_and_preserves_secrets(self):
        deploy.persist_image(self.env, DIGEST)
        self.assertEqual(self.env.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.env.read_text(),
                         f'TELEGRAM_BOT_TOKEN=do-not-print\nCPP_RECALL_IMAGE={DIGEST}\nDATABASE_POOL_SIZE=2\n')
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), ['.env'])

    def test_mutable_refs_duplicate_keys_and_symlinks_fail_closed(self):
        for image in ('latest', deploy.REGISTRY + ':' + SHA, 'other/repo@sha256:' + 'b' * 64):
            with self.subTest(image=image), self.assertRaises(deploy.Failure):
                deploy.persist_image(self.env, image)
        original = self.env.read_text()
        link = self.root / 'linked'
        link.symlink_to(self.env)
        with self.assertRaises(deploy.Failure):
            deploy.persist_image(link, DIGEST)
        self.assertEqual(self.env.read_text(), original)
        self.env.write_text(original + 'CPP_RECALL_IMAGE=duplicate\n')
        with self.assertRaises(deploy.Failure):
            deploy.persist_image(self.env, DIGEST)

    def test_switch_uses_only_prepared_digest_and_only_recreates_bot(self):
        self.host.candidates[SHA] = DIGEST
        with patch.object(deploy, 'run', return_value=b'') as run:
            self.host.switch(f'{deploy.IMAGE}:release-{SHA}')
        command = run.call_args.args[0]
        self.assertEqual(command[-7:], ['up', '-d', '--no-deps', '--no-build', '--pull', 'never', 'bot'])
        self.assertIn(str(self.root / 'host.compose.yaml'), command)
        self.assertIn('CPP_RECALL_IMAGE=' + DIGEST, self.env.read_text())
        with self.assertRaises(deploy.Failure):
            self.host.switch(f'{deploy.IMAGE}:release-' + 'd' * 40)

    def test_rollback_accepts_previous_local_image_id_without_restoring_database(self):
        with patch.object(deploy, 'run', return_value=b'') as run:
            self.host.switch(IMAGE_ID)
        self.assertIn('CPP_RECALL_IMAGE=' + IMAGE_ID, self.env.read_text())
        self.assertFalse(any('pg_restore' in call.args[0] or 'down' in call.args[0]
                             for call in run.call_args_list))

    def test_ci_publication_is_after_tests_and_not_for_pr_or_forks(self):
        workflow = (scripts.parent / '.github/workflows/ci.yml').read_text()
        publish = workflow.split('  publish-image:\n', 1)[1]
        self.assertIn('needs: build-and-test', publish)
        self.assertIn("github.event_name == 'push'", publish)
        self.assertIn("github.ref == 'refs/heads/main'", publish)
        self.assertIn("github.repository == 'Orurh/cpp-recall-bot'", publish)
        self.assertIn('persist-credentials: false', publish)
        self.assertIn('org.opencontainers.image.revision=$GITHUB_SHA', publish)
        self.assertIn('APP_VERSION=$GITHUB_SHA', publish)
        self.assertNotIn('secrets.', publish)
        self.assertNotIn(':latest', publish)


if __name__ == '__main__':
    unittest.main()
