import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('maintenance', Path(__file__).parents[1] / 'scripts/beta_maintenance.py')
maintenance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maintenance)


class MonitoringTest(unittest.TestCase):
    def test_three_failures_and_one_recovery(self):
        state = {}
        self.assertIsNone(maintenance.alert_decision(state, ['down'], 10000))
        self.assertIsNone(maintenance.alert_decision(state, ['down'], 10060))
        self.assertEqual(maintenance.alert_decision(state, ['down'], 10120), 'problem')
        state.update(last_alert=10120, alert_open=True)
        self.assertIsNone(maintenance.alert_decision(state, ['down'], 10180))
        self.assertEqual(maintenance.alert_decision(state, [], 10240), 'recovery')
        state['alert_open'] = False
        self.assertIsNone(maintenance.alert_decision(state, [], 10300))

    def test_short_vpn_failure_does_not_alert(self):
        state = {}
        maintenance.alert_decision(state, ['down'], 10000)
        maintenance.alert_decision(state, ['down'], 10060)
        self.assertIsNone(maintenance.alert_decision(state, [], 10120))
        self.assertEqual(state['consecutive_failures'], 0)

    def test_private_state_permissions_and_public_status(self):
        with tempfile.TemporaryDirectory() as root:
            private = Path(root) / 'private.json'
            public = Path(root) / 'public/status.json'
            maintenance.save_json(private, {'last_alert': 42})
            maintenance.save_json(public, {'issues': []}, 0o644)
            self.assertEqual(private.stat().st_mode & 0o777, 0o600)
            self.assertEqual(public.stat().st_mode & 0o777, 0o644)
            self.assertEqual(maintenance.read_json(private)['last_alert'], 42)

    def test_subprocess_errors_do_not_expose_credentials(self):
        with self.assertRaises(maintenance.Failure) as caught:
            maintenance.command(['sh', '-c', 'echo secret-token >&2; exit 1'], 'Безопасное сообщение')
        self.assertEqual(str(caught.exception), 'Безопасное сообщение')


if __name__ == '__main__':
    unittest.main()
