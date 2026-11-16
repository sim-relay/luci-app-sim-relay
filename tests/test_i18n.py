"""Language catalogs and separation between the English app and optional translations."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_i18n', ROOT / 'scripts/check-i18n.py')
i18n = importlib.util.module_from_spec(spec)
spec.loader.exec_module(i18n)


class TranslationTests(unittest.TestCase):
    def test_all_menu_and_page_strings_have_chinese_translations(self):
        result = i18n.check()
        self.assertTrue(result['verified'])
        self.assertGreater(result['source_strings'], 100)
        self.assertGreaterEqual(result['chinese_entries'], result['source_strings'])

    def test_english_core_and_no_mandatory_language_dependency(self):
        for path in (ROOT / 'src').glob('*.c'):
            self.assertIsNone(i18n.HAN.search(path.read_text()), path)
        makefile = (ROOT / 'luci-app-sim-relay/Makefile').read_text()
        self.assertIn('include $(TOPDIR)/feeds/luci/luci.mk', makefile)
        for line in makefile.splitlines():
            if 'DEPENDS:=' in line:
                self.assertNotIn('luci-i18n-', line)
        # luci.mk discovers this directory and maps zh_Hans to the zh-cn package.
        directories = {p.name for p in (ROOT / 'luci-app-sim-relay/po').iterdir() if p.is_dir()}
        self.assertTrue({'templates', 'zh_Hans'} <= directories)

    def test_chinese_includes_backend_prompts(self):
        entries = i18n.read_po(ROOT / 'luci-app-sim-relay/po/zh_Hans/sim-relay.po')
        key = 'ModemManager is not installed. Install the modemmanager package before adding a local cellular modem.'
        self.assertIn('安装 modemmanager', entries[key])
        self.assertEqual(entries['Add device'], '添加设备')
        self.assertEqual(entries['Messages'], '短信')

    def test_po_parser_handles_multiline_and_rejects_duplicates(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'test.po'
            path.write_text('msgid "Hello "\n"world"\nmsgstr "你好"\n"世界"\n')
            self.assertEqual(i18n.read_po(path), {'Hello world': '你好世界'})
            path.write_text('msgid "A"\nmsgstr "甲"\n\nmsgid "A"\nmsgstr "乙"\n')
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                i18n.read_po(path)
