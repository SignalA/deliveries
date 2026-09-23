"""Black-box integration tests; each case owns its temporary socket directory."""
import glob
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = Path(os.environ.get('GAIME_TEST_SERVER', ROOT / 'build-dgram/gaime_input_service'))
COMPAT = Path(os.environ.get('GAIME_TEST_CLIENT', ROOT / 'build-dgram/colleague_client'))


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='gaime-test-')
        self.dir = Path(self.tmp.name)
        self.sockpath = str(self.dir / 'input.sock')
        self.log = open(self.dir / 'server.log', 'w+')
        self.args = [str(SERVER), '--socket', self.sockpath, '--result-dir', str(self.dir),
                     '--mode', 'mock', '--delay-ms', '20', '--first-delay-ms', '60']
        self.p = subprocess.Popen(self.args, stdout=self.log, stderr=subprocess.STDOUT)
        self.clients = []
        deadline = time.monotonic() + 3
        while not os.path.exists(self.sockpath) and self.p.poll() is None and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertIsNone(self.p.poll())
        self.assertTrue(os.path.exists(self.sockpath))

    def tearDown(self):
        for s, path in self.clients:
            s.close()
        if self.p.poll() is None:
            self.p.terminate()
            self.p.wait(timeout=3)
        self.log.close()
        self.tmp.cleanup()

    def client(self, role=None, version=1):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        path = str(self.dir / ('client%d.sock' % len(self.clients)))
        s.bind(path)
        s.connect(self.sockpath)
        s.settimeout(2)
        self.clients.append((s, path))
        if role:
            s.send(('HELLO %s %d\n' % (role, version)).encode())
        return s

    def receive(self, s, prefix):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            message = s.recv(4096).decode().strip()
            if message.startswith(prefix):
                return message
        self.fail('Missing ' + prefix)

    def result(self):
        return 'CALIB_RESULT 1 24 ' + ' '.join(['100', '200'] * 24)

    def test_unmodified_game_client(self):
        for role in ['game']:
            completed = subprocess.run([str(COMPAT), self.sockpath,
                                        str(self.dir / ('compat-' + role + '.sock')), role, '8'],
                                       capture_output=True, text=True, timeout=10)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertIn('result=PASS', completed.stdout)
            self.assertIsNone(self.p.poll())
        self.assertEqual(len(glob.glob(str(self.dir / 'capture-gun1-*'))), 0)

    def test_protocol_validation_and_recovery(self):
        s = self.client()
        for data, expected in [(b'PING 1', 'ERROR HELLO_REQUIRED'),
                               (b'HELLO calibration 3', 'ERROR UNSUPPORTED_HELLO'),
                               (b'HELLO game 2', 'ERROR UNSUPPORTED_HELLO'),
                               (b'HELLO junk 1', 'ERROR UNSUPPORTED_HELLO'),
                               (b'HELLO calibration 1\x00extra', 'ERROR BAD_MESSAGE'),
                               (b'x' * 5000, 'ERROR BAD_MESSAGE'),
                               (b'', 'ERROR BAD_MESSAGE')]:
            s.send(data)
            self.assertEqual(self.receive(s, 'ERROR'), expected)
        s.send(b'HELLO calibration 1')
        self.assertEqual(self.receive(s, 'HELLO_OK'), 'HELLO_OK 1')
        s.send(b'PING 1')
        self.assertEqual(self.receive(s, 'PONG'), 'PONG 1')
        s.send(b'PONG 1')
        deadline = time.monotonic() + .2
        s.settimeout(.03)
        while time.monotonic() < deadline:
            try:
                self.assertFalse(s.recv(4096).decode().strip().startswith('ERROR'))
            except socket.timeout:
                pass
        s.settimeout(2)

    def test_result_validation_and_idempotence(self):
        s = self.client('calibration')
        self.receive(s, 'HELLO_OK')
        for message in ['CALIB_RESULT 1 1 10 20', self.result().replace(' 100 ', ' 99999999999999999999 ', 1),
                        self.result().replace('CALIB_RESULT 1', 'CALIB_RESULT 2'), self.result() + ' 42']:
            s.send(message.encode())
            self.assertEqual(self.receive(s, 'ERROR'), 'ERROR BAD_CALIB_RESULT')
        self.assertEqual(glob.glob(str(self.dir / 'capture-*')), [])
        for _ in range(2):
            s.send(self.result().encode())
            self.assertIn('CAPTURE_ONLY', self.receive(s, 'CALIB_STORED'))
        files = glob.glob(str(self.dir / 'capture-*'))
        self.assertEqual(len(files), 1)
        self.assertIn('status=CAPTURE_ONLY', Path(files[0]).read_text())
        s.send(self.result().replace(' 100 ', ' 101 ', 1).encode())
        self.assertEqual(self.receive(s, 'ERROR'), 'ERROR BAD_CALIB_RESULT')

    def test_calibration_v2_player_lock_and_result(self):
        s = self.client('calibration', 2)
        self.assertEqual(self.receive(s, 'HELLO_OK'), 'HELLO_OK 2')
        first = self.receive(s, 'GUN_DOWN').split()
        self.assertEqual(len(first), 4)
        player, x, y = map(int, first[1:])
        self.assertIn(player, (1, 2))
        self.assertTrue(0 <= x <= 32767 and 0 <= y <= 32767)
        wrong = 2 if player == 1 else 1
        result = 'CALIB_RESULT %d 24 %s' % (player, ' '.join(['32767', '0'] * 24))
        s.send(result.replace('CALIB_RESULT %d' % player, 'CALIB_RESULT %d' % wrong).encode())
        self.assertEqual(self.receive(s, 'ERROR'), 'ERROR BAD_CALIB_RESULT')
        s.send(result.encode())
        self.assertIn('CAPTURE_ONLY', self.receive(s, 'CALIB_STORED'))
        files = glob.glob(str(self.dir / 'capture-*'))
        self.assertEqual(len(files), 1)
        saved = Path(files[0]).read_text()
        self.assertIn('protocol=2', saved)
        self.assertIn('player_field=%d' % player, saved)

    def test_calibration_result_with_client_targets(self):
        s = self.client('calibration', 2)
        self.receive(s, 'HELLO_OK')
        player = int(self.receive(s, 'GUN_DOWN').split()[1])
        values = []
        for point in range(8):
            values.extend([1000 + point, 2000 + point] * 3)
            values.extend([3000 + point, 4000 + point])
        message = 'CALIB_RESULT %d 24 %s' % (player, ' '.join(map(str, values)))
        s.send(message.encode())
        self.assertIn('CAPTURE_ONLY', self.receive(s, 'CALIB_STORED'))
        files = glob.glob(str(self.dir / 'capture-*'))
        self.assertEqual(len(files), 1)
        self.assertIn('targets=CLIENT', Path(files[0]).read_text())

    def test_calibration_result_with_explicit_gun_and_profile(self):
        s = self.client('calibration', 1)
        self.receive(s, 'HELLO_OK')
        self.assertEqual(len(self.receive(s, 'GUN_DOWN').split()), 4)
        values = []
        for point in range(8):
            values.extend([1000 + point, 2000 + point] * 3)
            values.extend([3000 + point, 4000 + point])
        message = 'CALIB_RESULT 0 3 32 %s' % ' '.join(map(str, values))
        s.send(message.encode())
        self.assertIn('CAPTURE_ONLY', self.receive(s, 'CALIB_STORED'))
        saved = Path(glob.glob(str(self.dir / 'capture-gun1-*'))[0]).read_text()
        self.assertIn('gun=1', saved)
        self.assertIn('player_field=3', saved)
        self.assertIn('CALIB_RESULT 0 3 32', saved)

    def test_result_queued_before_client_unlink_is_processed(self):
        s = self.client('calibration', 1)
        self.receive(s, 'HELLO_OK')
        self.receive(s, 'GUN_DOWN')
        s.send(self.result().encode())
        path = self.clients[-1][1]
        s.close()
        os.unlink(path)
        deadline = time.monotonic() + 3
        files = []
        while time.monotonic() < deadline:
            files = glob.glob(str(self.dir / 'capture-gun1-*'))
            if files:
                break
            time.sleep(.01)
        self.assertEqual(len(files), 1)
        self.assertIn('CALIB_RESULT 1 24', Path(files[0]).read_text())

    def test_calibration_priority_and_busy(self):
        game = self.client('game')
        self.receive(game, 'HELLO_OK')
        calib = self.client('calibration')
        self.receive(calib, 'HELLO_OK')
        other = self.client('calibration')
        self.assertEqual(self.receive(other, 'ERROR'), 'ERROR CALIBRATION_BUSY')
        # Discard input queued before the calibration handshake.
        game.setblocking(False)
        try:
            while game.recv(4096): pass
        except BlockingIOError: pass
        game.settimeout(.15)
        self.assertTrue(self.receive(calib, 'GUN_DOWN').startswith('GUN_DOWN'))
        try:
            while True: self.assertFalse(game.recv(4096).startswith(b'GUN_DOWN'))
        except socket.timeout: pass
        calib.send(self.result().encode())
        self.receive(calib, 'CALIB_STORED')
        game.settimeout(2)
        self.assertTrue(self.receive(game, 'GUN_DOWN').startswith('GUN_DOWN'))

    def test_game_selects_each_guns_calibration_profile(self):
        game = self.client('game')
        self.receive(game, 'HELLO_OK')
        for gun, profile in [(0, 0), (1, 3), (0, 2)]:
            game.send(('SET_PROFILE %d %d' % (gun, profile)).encode())
            self.assertEqual(self.receive(game, 'PROFILE_SET'),
                             'PROFILE_SET %d %d' % (gun, profile))
        for command in ['SET_PROFILE', 'SET_PROFILE 0', 'SET_PROFILE 2 0',
                        'SET_PROFILE -1 0', 'SET_PROFILE 0 4',
                        'SET_PROFILE 0 1 extra']:
            game.send(command.encode())
            self.assertEqual(self.receive(game, 'ERROR'), 'ERROR BAD_SET_PROFILE')
        calib = self.client('calibration')
        self.receive(calib, 'HELLO_OK')
        calib.send(b'SET_PROFILE 0 1')
        self.assertEqual(self.receive(calib, 'ERROR'), 'ERROR GAME_ROLE_REQUIRED')
        game.send(b'SET_PROFILE 0 1')
        self.assertEqual(self.receive(game, 'ERROR'), 'ERROR CALIBRATION_BUSY')

    def test_dual_gun_player_mapping(self):
        self.p.terminate()
        self.assertEqual(self.p.wait(timeout=3), 0)
        self.args.extend(['--player1', '3', '--player2', '4'])
        self.p = subprocess.Popen(self.args, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 3
        while not os.path.exists(self.sockpath) and self.p.poll() is None and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertIsNone(self.p.poll())
        game = self.client('game')
        self.receive(game, 'HELLO_OK')
        players = set()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and players != {3, 4}:
            message = self.receive(game, 'GUN_DOWN')
            fields = message.split()
            self.assertEqual(len(fields), 4)
            players.add(int(fields[1]))
        self.assertEqual(players, {3, 4})

    def test_duplicate_service_keeps_original_socket(self):
        second = subprocess.run(self.args, capture_output=True, timeout=3)
        self.assertNotEqual(second.returncode, 0)
        self.assertIsNone(self.p.poll())
        s = self.client('game')
        self.assertEqual(self.receive(s, 'HELLO_OK'), 'HELLO_OK 1')

    def test_continuous_dual_gun_delivery(self):
        game = self.client('game')
        self.receive(game, 'HELLO_OK')
        counts = {1: 0, 2: 0}
        for _ in range(40):
            fields = self.receive(game, 'GUN_DOWN').split()
            self.assertEqual(len(fields), 4)
            player, x, y = map(int, fields[1:])
            self.assertIn(player, counts)
            self.assertTrue(0 <= x <= 32767 and 0 <= y <= 32767)
            counts[player] += 1
        self.assertEqual(counts, {1: 20, 2: 20})
        self.assertIsNone(self.p.poll())

    def test_unique_binding_argument_guards(self):
        for options in [ ['--uniq1', 'same', '--uniq2', 'same'],
                         ['--uniq1', 'A', '--device1', '/dev/input/event99'],
                         ['--phys1', 'usb-a', '--uniq1', 'A'],
                         ['--phys1', 'same', '--phys2', 'same'],
                         ['--uniq2', ''], ['--uniq1', 'x' * 128] ]:
            result = subprocess.run(self.args + options, capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 2)

    def test_closed_client_with_stale_path_does_not_hold_calibration(self):
        s = self.client('calibration')
        self.receive(s, 'HELLO_OK')
        s.close()  # Leave pathname behind, just as a crashed client would.
        time.sleep(1.2)
        second = self.client('calibration')
        self.assertEqual(self.receive(second, 'HELLO_OK'), 'HELLO_OK 1')

    def test_client_limit_and_role_guard(self):
        clients = [self.client('game') for _ in range(8)]
        for s in clients: self.receive(s, 'HELLO_OK')
        s = self.client('game')
        self.assertEqual(self.receive(s, 'ERROR'), 'ERROR CLIENT_LIMIT')
        clients[0].send(self.result().encode())
        self.assertEqual(self.receive(clients[0], 'ERROR'), 'ERROR CALIBRATION_ROLE_REQUIRED')
        clients[0].send(b'BYE')
        time.sleep(.03)
        s.send(b'HELLO game 1')
        self.assertEqual(self.receive(s, 'HELLO_OK'), 'HELLO_OK 1')

    def test_graceful_shutdown_removes_socket(self):
        self.p.terminate()
        self.assertEqual(self.p.wait(timeout=3), 0)
        self.assertFalse(os.path.exists(self.sockpath))


if __name__ == '__main__':
    unittest.main(verbosity=2)
