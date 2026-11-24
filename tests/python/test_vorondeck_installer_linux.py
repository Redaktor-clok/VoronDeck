"""Exercise the CLI with a loopback Moonraker fixture; never control real services."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
import importlib.util
import pwd

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from vorondeck_distribution import elf_arch


@unittest.skipUnless(sys.platform == 'linux', 'Linux CLI integration')
class LinuxInstallerTests(unittest.TestCase):
    def test_language_selection_in_real_terminal_before_host_operations(self):
        import pty
        for answer, code, expected in [('0\n', 0, 'Language'),
                                       ('1\n', 2, 'Недопустимый порт Moonraker'),
                                       ('invalid\n2\n', 2, 'Invalid Moonraker port')]:
            with self.subTest(answer=answer):
                master, slave = pty.openpty()
                try:
                    # Invalid port exits before service detection or contacting a printer.
                    with subprocess.Popen(['sh', str(ROOT / 'install.sh'), '--moonraker-port', '0'],
                                          stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                          text=True) as process:
                        os.write(master, answer.encode())
                        try:
                            stdout, stderr = process.communicate(timeout=10)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.communicate()
                            raise
                        self.assertEqual(process.returncode, code, stdout + stderr)
                        self.assertIn(expected, stdout + stderr)
                finally:
                    os.close(master)
                    os.close(slave)

    def test_generated_service_syntax(self):
        spec = importlib.util.spec_from_file_location('installer', ROOT / 'scripts/install-vorondeck.py')
        installer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(installer)
        with tempfile.TemporaryDirectory(prefix='vorondeck-service-') as temporary:
            root = Path(temporary)
            (root / 'bin').mkdir()
            shutil.copyfile(shutil.which('true'), root / 'bin/helix-screen')
            (root / 'bin/helix-screen').chmod(0o755)
            for kind in ('systemd', 'openrc'):
                service = root / ('vorondeck.service' if kind == 'systemd' else 'vorondeck.init')
                service.write_text(installer.Services(kind).text(pwd.getpwuid(os.getuid()).pw_name,
                                                                 root, root / 'config', [], 7125))
                command = (['systemd-analyze', '--man=no', 'verify', str(service)] if kind == 'systemd'
                           else ['sh', '-n', str(service)])
                result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cli_stage_first_run_and_repeated_install(self):
        if not Path('/run/systemd/system').is_dir():
            self.skipTest('This integration case uses the systemd development VM')
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path == '/server/info':
                    result = {'klippy_state': 'ready'}
                else:
                    result = {'status': {'heaters': {'available_heaters': ['extruder']},
                              'extruder': {'target': 0}, 'print_stats': {'state': 'standby'},
                              'idle_timeout': {'state': 'Idle'}}}
                data = json.dumps({'result': result}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *_):
                pass

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with tempfile.TemporaryDirectory(prefix='vorondeck-cli-') as folder:
                home = Path(folder)
                # A harmless host ELF tests ABI + loader checks without launching a UI.
                executable = Path(shutil.which('true')).read_bytes()
                payload = {'bin/helix-screen': executable,
                           'config/settings.json.template': b'{"config_version":32,"wizard_completed":false}'}
                manifest = {'schema': 1, 'project': 'VoronDeck', 'version': '0.1.0-beta.1',
                            'architecture': elf_arch(executable),
                            'files': {p: hashlib.sha256(d).hexdigest() for p, d in payload.items()}}
                package = home / 'fixture.tar.gz'
                with tarfile.open(package, 'w:gz') as archive:
                    for name, data in {**payload, 'vorondeck-release.json': json.dumps(manifest).encode()}.items():
                        info = tarfile.TarInfo(name)
                        info.size = len(data)
                        archive.addfile(info, io.BytesIO(data))
                digest = hashlib.sha256(package.read_bytes()).hexdigest()
                args = [sys.executable, str(ROOT / 'scripts/install-vorondeck.py'), '--package', str(package),
                        '--sha256', digest, '--moonraker-port', str(server.server_port)]
                environment = {**os.environ, 'HOME': str(home)}
                # If a stage/check unexpectedly tries sudo, fail rather than allowing changes.
                fake_bin = home / 'fake-bin'
                fake_bin.mkdir()
                trap = fake_bin / 'sudo'
                trap.write_text('#!/bin/sh\necho unexpected-sudo >&2\nexit 99\n')
                trap.chmod(0o755)
                environment['PATH'] = str(fake_bin) + os.pathsep + os.environ['PATH']
                for language in ('ru', 'en'):
                    for mode in ([], ['--install'], ['--install']):
                        result = subprocess.run(args + mode + ['--language', language],
                                                capture_output=True, text=True, env=environment, timeout=20)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn('Пакет проверен' if language == 'ru' else 'Package verified', result.stdout)
                        self.assertNotIn('unexpected-sudo', result.stderr)
                base = home / '.local/share/vorondeck'
                self.assertEqual(len(list((base / 'releases').iterdir())), 1)
                self.assertFalse((base / 'current').exists())
                self.assertFalse((home / 'printer_data').exists())
                # One-command entry point also supports offline check with no Git access.
                for language in ('ru', 'en'):
                    result = subprocess.run(['sh', str(ROOT / 'install.sh'), '--check', *args[2:],
                                             '--language', language],
                                            capture_output=True, text=True, env=environment, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('Пакет проверен' if language == 'ru' else 'Package verified', result.stdout)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)


if __name__ == '__main__':
    unittest.main()
