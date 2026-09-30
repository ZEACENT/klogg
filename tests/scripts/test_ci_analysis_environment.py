import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
RECIPE = ROOT / "docker/ubuntu24.04-analysis/Dockerfile"


class AnalysisEnvironmentRecipeTest(unittest.TestCase):
    def recipe(self):
        self.assertTrue(RECIPE.is_file(), "the shared analysis environment recipe is missing")
        return RECIPE.read_text(encoding="utf-8")

    def test_packages_are_installed_from_verified_offline_materials(self):
        recipe = self.recipe()
        self.assertIn("FROM ${UBUNTU_IMAGE}", recipe)
        self.assertIn("ARG APT_RUNTIME_LOCK_SHA256", recipe)
        self.assertIn("ci_install_locked_apt.sh", recipe)
        self.assertIn("RUN --network=none", recipe)
        self.assertNotIn("apt-get update", recipe)
        self.assertNotIn("apt-get install", recipe)
        self.assertNotIn("curl ", recipe)
        self.assertNotIn("wget ", recipe)

    def test_tools_are_hash_checked_and_safely_extracted_before_execution(self):
        recipe = self.recipe()
        check = recipe.index("sha256sum --check --strict")
        extract = recipe.index("python3 /opt/klogg-ci/extract_verified_tar.py")
        probe = recipe.index("cmake --version")
        self.assertLess(check, extract)
        self.assertLess(extract, probe)
        self.assertIn("ARG TOOLS_ARCHIVE_SHA256", recipe)
        self.assertIn("prefetch_adb_helper_sources.py", recipe)

    def test_shared_analysis_tools_match_existing_roles_without_codeql_redistribution(self):
        recipe = self.recipe()
        for version in ("3.31.6", "1.12.1", "6.9.3"):
            self.assertIn(version, recipe)
        self.assertIn("CC=gcc-13", recipe)
        self.assertIn("CXX=g++-13", recipe)
        self.assertIn("CMAKE_PREFIX_PATH=/opt/klogg-tools/qt/6.9.3/gcc_64", recipe)
        self.assertIn("LANG=C.UTF-8", recipe)
        active = "\n".join(line for line in recipe.splitlines() if not line.lstrip().startswith("#"))
        self.assertNotIn("codeql", active.lower())
        self.assertNotIn("/usr/local/cpm_cache", active)
        self.assertNotIn("GITHUB_SHA", active)


if __name__ == "__main__":
    unittest.main()
