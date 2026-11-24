# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared console messages for the bootstrap and Linux installer."""
import os
import sys

LANGUAGES = ('ru', 'en')
language = 'en'
RU = {
    'VoronDeck — installation:': 'VoronDeck — установка:',
    '1. Install VoronDeck with autostart (the previous screen will be disabled).': '1. Установить и включить автозапуск VoronDeck (прежний экран отключится).',
    '2. Run for testing until the next reboot.': '2. Запустить для проверки до перезагрузки.',
    '3. Prepare files and leave the previous screen running.': '3. Подготовить файлы, оставить прежний экран работающим.',
    '0. Cancel. KlipperScreen files are kept for rollback.': '0. Отмена. Файлы KlipperScreen сохраняются для отката.',
    'Choose [1/2/3/0]: ': 'Выберите [1/2/3/0]: ',
    'Installation stopped: ': 'Установка остановлена: ',
    'Required system packages:': 'Необходимые системные пакеты:',
    'Install these packages from the configured OS repositories? [y/N] ': 'Установить эти пакеты из настроенных репозиториев ОС? [y/N] ',
    'Host:': 'Хост:',
    'init:': 'система служб:',
    'config:': 'настройки:',
    'Package verified; printer is idle': 'Пакет проверен; принтер не занят',
    'Restore screen services and settings from this backup? [y/N] ': 'Восстановить службы и настройки экрана из этой копии? [y/N] ',
    'Previous screen restored': 'Предыдущий экран восстановлен',
    'Package staged:': 'Пакет подготовлен:',
    'Screen unchanged. Add --activate to switch after confirmation.': 'Экран не изменён. Добавьте --activate для переключения после подтверждения.',
    'Active screens:': 'Активные экраны:',
    'none': 'нет',
    'Existing screen files are kept. Boot services change only with --autostart.': 'Файлы прежнего экрана сохраняются. Автозапуск меняется только с --autostart.',
    'Switch to VoronDeck now? [y/N] ': 'Переключиться на VoronDeck сейчас? [y/N] ',
    'VoronDeck service stayed active for 5 seconds. Check the physical display and touch.': 'Служба VoronDeck работает 5 секунд. Проверьте изображение и сенсор на экране.',
    'Rollback backup:': 'Резервная копия для отката:',
    'VoronDeck updates connected to Moonraker; channel:': 'Обновления VoronDeck подключены к Moonraker; канал:',
    'Update configuration backup:': 'Резервная копия настроек обновлений:',
    'Run on Linux as the printer user, not root': 'Запустите в Linux от пользователя принтера, не от root',
    'Offline installation requires both --package and --sha256': 'Для установки без сети нужны --package и --sha256',
    'Invalid Moonraker port': 'Недопустимый порт Moonraker',
    'Python 3.9 or newer is required': 'Требуется Python 3.9 или новее',
    'Run this installer on the Linux printer host': 'Запустите установщик на Linux-хосте принтера',
    'Run as the printer user, not root; sudo is used only for service management': 'Запустите от пользователя принтера, не от root; sudo используется только для управления службами',
    'Printer must be idle; printing, paused and unknown states are refused': 'Принтер должен быть свободен; печать, пауза и неизвестное состояние запрещают установку',
    'Wait until the printer is idle before installing': 'Перед установкой дождитесь завершения работы принтера',
    'Cannot verify heater state': 'Не удалось проверить состояние нагревателей',
    'All heater targets must be zero before installing': 'Перед установкой все заданные температуры нагревателей должны быть равны нулю',
    'Klipper is not ready': 'Klipper не готов',
    'Another VoronDeck installer is running': 'Уже запущен другой установщик VoronDeck',
    'Required runtime libraries are missing': 'Отсутствуют необходимые библиотеки',
}


def tr(message):
    return RU.get(message, message) if language == 'ru' else message


def select_language(requested=None, interactive=False):
    global language
    locale = os.environ.get('LC_ALL') or os.environ.get('LC_MESSAGES') or os.environ.get('LANG', '')
    language = requested or ('ru' if locale.lower().startswith('ru') else 'en')
    if interactive and requested is None and sys.stdin.isatty():
        print('VoronDeck — Язык / Language\n1. Русский\n2. English\n0. Отмена / Cancel')
        while True:
            choice = input('Выберите / Choose [1/2/0]: ').strip().lower()
            if choice == '0':
                return None
            if choice in ('1', 'ru', '2', 'en'):
                language = 'ru' if choice in ('1', 'ru') else 'en'
                break
            print('Введите 1, 2 или 0 / Enter 1, 2 or 0')
    return language
