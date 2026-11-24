"""Language selection must not weaken installation confirmations or checks."""
from contextlib import ExitStack, redirect_stdout
import importlib.util
import io
from pathlib import Path
import sys
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import vorondeck_installer_i18n as messages

spec = importlib.util.spec_from_file_location('language_bootstrap', ROOT / 'scripts/vorondeck-bootstrap.py')
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class InstallerLanguageTests(unittest.TestCase):
    def tearDown(self):
        messages.select_language('en')

    def test_moonraker_runtime_loads_without_scripts_on_import_path(self):
        result = subprocess.run([sys.executable, '-I', '-c',
                                 'import runpy, sys; runpy.run_path(sys.argv[1])',
                                 str(ROOT / 'scripts/moonraker-vorondeck-runtime.py')],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_selection_retries_and_does_not_consume_menu_answer(self):
        with patch.object(sys.stdin, 'isatty', return_value=True), \
                patch('builtins.input', side_effect=['', 'bad', '1', '3']) as answer, \
                redirect_stdout(io.StringIO()) as output:
            self.assertEqual(messages.select_language(interactive=True), 'ru')
            self.assertEqual(answer.call_count, 3)
            self.assertEqual(input('menu'), '3')
        self.assertIn('Enter 1, 2 or 0', output.getvalue())

    def test_locale_and_explicit_language_never_prompt_without_tty(self):
        for locale, expected in [('ru_RU.UTF-8', 'ru'), ('de_DE.UTF-8', 'en'), ('C', 'en')]:
            with patch.dict('os.environ', {'LC_ALL': locale}, clear=True), \
                    patch.object(sys.stdin, 'isatty', return_value=False), \
                    patch('builtins.input', side_effect=AssertionError('unexpected prompt')):
                self.assertEqual(messages.select_language(interactive=True), expected)
                self.assertEqual(messages.select_language('en', interactive=True), 'en')

    def test_cancel_before_host_checks_or_download(self):
        with patch.object(sys, 'argv', ['install.sh']), \
                patch.object(sys.stdin, 'isatty', return_value=True), \
                patch('builtins.input', return_value='0'), \
                patch.object(bootstrap.installer.Services, 'detect') as detect, \
                patch.object(bootstrap, 'fetch_package') as fetch, redirect_stdout(io.StringIO()):
            bootstrap.main()
        detect.assert_not_called()
        fetch.assert_not_called()

    def test_bootstrap_propagates_language_for_each_install_mode(self):
        for language in messages.LANGUAGES:
            for choice in ('0', '1', '2', '3', 'check'):
                with self.subTest(language=language, choice=choice), ExitStack() as stack:
                    argv = ['install.sh', '--language', language, '--package', 'fixture.tar.gz',
                            '--sha256', 'a' * 64]
                    if choice == 'check':
                        argv.append('--check')
                    stack.enter_context(patch.object(sys, 'argv', argv))
                    stack.enter_context(patch.object(sys, 'platform', 'linux'))
                    stack.enter_context(patch.object(bootstrap.os, 'geteuid', return_value=1000, create=True))
                    stack.enter_context(patch.object(bootstrap.installer.Services, 'detect'))
                    stack.enter_context(patch.object(bootstrap.installer, 'host_arch', return_value='aarch64'))
                    stack.enter_context(patch.object(bootstrap, 'libc_name', return_value='glibc'))
                    idle = stack.enter_context(patch.object(bootstrap.installer, 'check_printer'))
                    verify = stack.enter_context(patch.object(bootstrap.installer, 'verify_package',
                                                             return_value=({'version': '1.0.1'}, {})))
                    run = stack.enter_context(patch.object(bootstrap.subprocess, 'run'))
                    answer = stack.enter_context(patch('builtins.input', return_value=choice))
                    output = stack.enter_context(redirect_stdout(io.StringIO()))
                    bootstrap.main()
                    idle.assert_called_once_with(7125)
                    verify.assert_called_once()
                    if choice == 'check':
                        answer.assert_not_called()
                    else:
                        self.assertIn('Выберите' if language == 'ru' else 'Choose', answer.call_args.args[0])
                        self.assertIn('Установить' if language == 'ru' else 'Install', output.getvalue())
                    if choice == '0':
                        run.assert_not_called()
                        continue
                    command = run.call_args.args[0]
                    self.assertEqual(command[command.index('--language') + 1], language)
                    self.assertEqual('--install' in command, choice != 'check')
                    self.assertEqual('--activate' in command, choice in ('1', '2'))
                    self.assertEqual('--autostart' in command, choice == '1')

    def test_busy_printer_still_stops_in_both_languages(self):
        for language in messages.LANGUAGES:
            messages.select_language(language)
            with self.assertRaises(ValueError):
                bootstrap.installer.assert_idle({'print_stats': {'state': 'printing'}})

    def test_every_console_message_has_russian_translation(self):
        import ast
        for filename in ('vorondeck-bootstrap.py', 'install-vorondeck.py'):
            tree = ast.parse((ROOT / 'scripts' / filename).read_text(encoding='utf-8'))
            for node in ast.walk(tree):
                if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id in ('print', 'input'):
                    for arg in node.args:
                        if isinstance(arg, ast.Call) and isinstance(arg.func, ast.Name) and arg.func.id == 'tr':
                            self.assertIn(ast.literal_eval(arg.args[0]), messages.RU)


if __name__ == '__main__':
    unittest.main()
