# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts'))
from check_lvgl_cleanup import findings


class CleanupTests(unittest.TestCase):
    def test_flags_all_three_calls_including_trailing_comments(self):
        source = 'lv_display_delete(d); // ordinary comment\nlv_group_delete(g);\nlv_indev_delete(i);'
        self.assertEqual(list(findings(source)), [(1, 'lv_display_delete'),
                                                 (2, 'lv_group_delete'), (3, 'lv_indev_delete')])

    def test_ignores_comments_and_strings_but_preserves_line_numbers(self):
        source = '/*\n lv_display_delete(d);\n*/\n// lv_group_delete(g);\n"lv_indev_delete(i)";\nlv_display_delete(d);'
        self.assertEqual(list(findings(source)), [(6, 'lv_display_delete')])

    def test_requires_reason_for_exception(self):
        self.assertEqual(len(list(findings('lv_display_delete(d); // NOLINT(helix-shutdown)'))), 1)
        self.assertEqual(list(findings('lv_display_delete(d); // NOLINT(helix-shutdown) init failure cleanup')), [])

    def test_preceding_annotation_applies_only_to_next_statement(self):
        source = '// NOLINT(helix-shutdown): delete failed display before replacement\nlv_display_delete(d);\nlv_group_delete(g);'
        self.assertEqual(list(findings(source)), [(3, 'lv_group_delete')])

    def test_multiline_call_is_checked(self):
        self.assertEqual(list(findings('lv_display_delete\n(d);')), [(1, 'lv_display_delete')])

    def test_unrelated_nolint_does_not_hide_call(self):
        self.assertEqual(len(list(findings('lv_display_delete(d); // NOLINT(other): reason'))), 1)


if __name__ == '__main__':
    unittest.main()
