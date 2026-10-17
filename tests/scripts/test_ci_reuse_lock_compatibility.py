"""Reuse must not silently invalidate the already-published environment locks."""
import json
from pathlib import Path
import unittest

from test_consume_ci_environment import MODULE as CONSUMER

ROOT = Path(__file__).resolve().parents[2]


class ProductionEnvironmentCompatibilityTest(unittest.TestCase):
    def test_current_recipe_inputs_and_policy_match_authenticated_receipts(self):
        def read(relative):
            return json.loads((ROOT / relative).read_text())

        catalog = read("ci/environments/recipes.json")
        profiles = read("ci/environments/profiles.json")
        lock = read("ci/environments/lock.json")
        CONSUMER.core.validate_production_lock(lock, catalog)
        for family, locked in lock["families"].items():
            with self.subTest(family=family):
                self.assertEqual(CONSUMER.core.recipe_identity(catalog, family, ROOT),
                                 locked["recipe_digest"], "changed recipes require genuine requalification")
                inputs = read("ci/environments/inputs/" + family + ".json")
                self.assertEqual(CONSUMER.core.input_identity(inputs), locked["input_digest"])
                needs_materials = any(profile["role_materials"] for profile in profiles["families"][family].values())
                materials = read("ci/environments/role-materials.json") if needs_materials else None
                policy = CONSUMER.profiles_module.build_policy(catalog, profiles, family, ROOT,
                                                              role_materials=materials)
                evidence = locked["qualification"]
                receipt_path = ROOT / evidence["receipt"]
                self.assertEqual(CONSUMER._digest(receipt_path.read_bytes()), evidence["digest"])
                receipt = read(evidence["receipt"])
                self.assertEqual(CONSUMER.core.policy_identity(policy), receipt["policy_digest"],
                                 "do not change locked verifier bytes in an unrelated performance change")


if __name__ == "__main__":
    unittest.main()
