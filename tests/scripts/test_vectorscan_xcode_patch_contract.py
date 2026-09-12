#!/usr/bin/env python3

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
THIRD_PARTY_CMAKE = ROOT / "3rdparty" / "CMakeLists.txt"
PATCH = ROOT / "3rdparty" / "patches" / "fix_vectorscan_xcode_ragel_targets.patch"


class VectorscanXcodePatchContractTest(unittest.TestCase):
    def test_patch_is_registered_before_vectorscan_configuration(self):
        cmake = THIRD_PARTY_CMAKE.read_text(encoding="utf-8")
        patch_name = PATCH.name
        self.assertIn(patch_name, cmake)
        self.assertLess(
            cmake.index(patch_name),
            cmake.index("add_subdirectory(${vectorscan_SOURCE_DIR}"),
        )

    def test_patch_removes_every_redundant_ragel_root_target(self):
        patch = PATCH.read_text(encoding="utf-8")
        deleted = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("-") and not line.startswith("---")
        )
        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )

        redundant_roots = (
            "add_custom_target(ragel_${src_file} DEPENDS ${rl_out})",
            "add_dependencies(hs ragel_Parser)",
            "add_dependencies(hs_shared ragel_Parser)",
            "add_dependencies(expressionutil ragel_ExpressionParser)",
            "add_dependencies(hscollider ragel_ColliderCorporaParser)",
        )
        for statement in redundant_roots:
            with self.subTest(statement=statement):
                self.assertIn(statement, deleted)
                self.assertNotIn(statement, added)

        self.assertNotIn("CMAKE_GENERATOR", added)
        self.assertNotIn("Xcode", added)


if __name__ == "__main__":
    unittest.main()
