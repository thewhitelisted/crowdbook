import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# The pages that tell an outsider which release to install.
PAGES = ["README.md", "docs/quickstart.md", "docs/tutorial.md", "python/README.md"]
RELEASE = re.compile(r"crowdbook-(?:engine|analysis)(?:\[\w+\])?==([\w.]+)"
                     r"|expanded_assets/v([\w.]+)|--branch v([\w.]+)")


def project_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text()
    return re.search(r"^ +VERSION ([0-9.]+)$", text, re.MULTILINE).group(1)


class DocsTest(unittest.TestCase):
    def test_the_install_instructions_name_the_projects_version(self):
        version = project_version()
        for page in PAGES:
            found = [next(g for g in m.groups() if g)
                     for m in RELEASE.finditer((ROOT / page).read_text())]
            with self.subTest(page=page):
                self.assertTrue(found, "no release named")
                self.assertEqual(set(found), {version},
                                 "the release to install is the project's version, bumped with it")

    def test_a_stale_version_is_caught(self):
        found = [next(g for g in m.groups() if g) for m in RELEASE.finditer(
            "pip install crowdbook-engine==0.12.0 --find-links .../expanded_assets/v0.12.0\n"
            "git clone --branch v0.12.0 ...")]
        self.assertEqual(found, ["0.12.0", "0.12.0", "0.12.0"])


if __name__ == "__main__":
    unittest.main()
