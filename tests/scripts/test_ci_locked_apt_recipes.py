import json
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
FAMILIES = {
    "focal-qt5-gcc13": "ubuntu20.04",
    "jammy-qt5": "ubuntu22.04",
    "noble-qt6": "ubuntu24.04",
    "resolute-qt6": "ubuntu26.04",
}


class LockedAptRecipeTest(unittest.TestCase):
    def test_qualified_recipes_select_verified_network_free_package_stage(self):
        catalog = json.loads((ROOT / "ci/environments/recipes.json").read_text())
        for family, directory in FAMILIES.items():
            with self.subTest(family=family):
                entry = catalog["families"][family]
                self.assertEqual(entry.get("build_args", {}).get("KLOGG_APT_STAGE"), "locked")
                self.assertIn("scripts/ci_install_locked_apt.sh", entry["recipe_files"])
                recipe = (ROOT / "docker" / directory / "Dockerfile").read_text()
                self.assertIn("ARG UBUNTU_IMAGE=", recipe)
                self.assertIn("FROM ${UBUNTU_IMAGE} AS locked-apt", recipe)
                self.assertIn("FROM ${KLOGG_APT_STAGE}-apt AS build-environment", recipe)
                locked = recipe.split("FROM ${UBUNTU_IMAGE} AS locked-apt", 1)[1].split("\nFROM ", 1)[0]
                self.assertIn("ARG APT_RUNTIME_LOCK_SHA256", locked)
                self.assertIn("RUN --network=none", locked)
                self.assertIn("ci_install_locked_apt.sh", locked)
                for forbidden in ("apt-get", "apt_install", "curl ", "wget ", "||"):
                    self.assertNotIn(forbidden, locked)

    def test_focal_replays_bootstrap_before_the_ppa_package_layer(self):
        recipe = (ROOT / "docker/ubuntu20.04/Dockerfile").read_text()
        locked = recipe.split("FROM ${UBUNTU_IMAGE} AS locked-apt", 1)[1].split("\nFROM ", 1)[0]
        self.assertIn("ARG APT_BOOTSTRAP_LOCK_SHA256", locked)
        self.assertIn("COPY inputs/bootstrap/", locked)
        bootstrap = locked.index("${APT_BOOTSTRAP_LOCK_SHA256:")
        runtime = locked.index("${APT_RUNTIME_LOCK_SHA256:")
        self.assertLess(bootstrap, runtime)

    def test_online_source_build_is_explicit_not_a_locked_build_fallback(self):
        for directory in FAMILIES.values():
            with self.subTest(directory=directory):
                recipe = (ROOT / "docker" / directory / "Dockerfile").read_text()
                self.assertIn("ARG KLOGG_APT_STAGE=online", recipe)
                self.assertIn("FROM ${UBUNTU_IMAGE} AS online-apt", recipe)
                self.assertEqual(recipe.count("FROM ${KLOGG_APT_STAGE}-apt"), 1)
                self.assertNotRegex(recipe, r"if .*APT_RUNTIME_LOCK_SHA256")
                self.assertIn("CCACHE_DIR=/usr/local/.ccache", recipe)

    def test_cmake_installers_are_checked_before_execution(self):
        checksum = "ea497b4658816010e5850a3ed53845e430654640aabbe10d93fe67def9503e4d"
        for directory in ("ubuntu20.04", "ubuntu22.04", "ubuntu24.04"):
            with self.subTest(directory=directory):
                recipe = (ROOT / "docker" / directory / "Dockerfile").read_text()
                self.assertIn(checksum, recipe)
                self.assertLess(recipe.index("sha256sum --check --strict"),
                                recipe.index("/tmp/cmake-installer.sh --prefix"))
        resolute = (ROOT / "docker/ubuntu26.04/Dockerfile").read_text()
        active = "\n".join(line for line in resolute.splitlines() if not line.lstrip().startswith("#"))
        self.assertNotIn("cmake-3.20.2", active)
        self.assertRegex(active, r"ninja-build\s+cmake")


if __name__ == "__main__":
    unittest.main()
