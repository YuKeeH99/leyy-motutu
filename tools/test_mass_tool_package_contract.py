import os
import types
import unittest
from pathlib import Path

from PyInstaller.archive.readers import CArchiveReader


ROOT = Path(__file__).resolve().parents[1]
EXE = Path(os.environ.get(
    "LIFELIKE_MASS_TOOL_EXE",
    ROOT / "dist/LifelikeCoreMassTool/LifelikeCoreMassTool.exe",
))


def nested_code(root, name):
    pending = [root]
    while pending:
        code = pending.pop()
        if code.co_name == name:
            return code
        pending.extend(item for item in code.co_consts if isinstance(item, types.CodeType))
    raise AssertionError(f"missing packaged function: {name}")


class MassToolPackageContractTest(unittest.TestCase):
    def test_packaged_tool_supports_transaction_and_factory_kv_fallback(self):
        archive = CArchiveReader(str(EXE))
        module = archive.open_embedded_archive("PYZ.pyz").extract("test_tool.mass_tool_support")
        persist = nested_code(module, "persist_lingxin_config_via_adb")
        constants = repr(persist.co_consts)
        self.assertIn("config-set", constants)
        self.assertIn("_persist_lingxin_config_via_kv", persist.co_names)
        nested_code(module, "_persist_lingxin_config_via_kv")
        plan = nested_code(module, "_lingxin_kv_write_plan")
        self.assertIn("user.lingxin.provisioned", repr(plan.co_consts))

    def test_packaged_tool_contains_drag_drop_download_and_config_clear_backend(self):
        archive = CArchiveReader(str(EXE))
        module = archive.open_embedded_archive("PYZ.pyz").extract("test_tool.mass_tool_support")
        for function in (
            "materialize_download_lpk",
            "ensure_adb_sync_available",
            "clear_lingxin_config_via_adb",
        ):
            nested_code(module, function)
        self.assertIn("ChipID", repr(module.co_consts))


if __name__ == "__main__":
    unittest.main()
