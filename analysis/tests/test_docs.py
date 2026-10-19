import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# The pages that tell an outsider which release to install. They name the latest published
# release, which trails the project's version while the next one is being made
# (docs/releases/README.md).
PAGES = ["README.md", "docs/quickstart.md", "docs/tutorial.md", "python/README.md"]
RELEASE = re.compile(r"crowdbook(?:-engine|-analysis)?(?:\[\w+\])?==([\w.]+)"
                     r"|releases/(?:expanded_assets|download|tag)/v([\w.]+)"
                     r"|(?:--branch[ =]|-b )v([\w.]+)")


def named(text: str) -> list[str]:
    return [next(g for g in m.groups() if g) for m in RELEASE.finditer(text)]


def version(text: str) -> tuple[int, ...]:
    return tuple(int(part) for part in text.split("."))


def project_version() -> str:
    found = re.search(r"^ +VERSION ([0-9.]+)\s*$", (ROOT / "CMakeLists.txt").read_text(),
                      re.MULTILINE)
    assert found, "CMakeLists.txt names no project VERSION"
    return found.group(1)


class DocsTest(unittest.TestCase):
    def test_the_install_instructions_name_one_published_release(self):
        found = {page: named((ROOT / page).read_text()) for page in PAGES}
        for page, releases in found.items():
            self.assertTrue(releases, f"{page} names no release")
        releases = {r for page in found.values() for r in page}
        self.assertEqual(len(releases), 1, f"the pages name different releases: {found}")
        release = releases.pop()
        self.assertTrue((ROOT / f"docs/releases/v{release}.md").exists(),
                        f"v{release} has no release notes")
        self.assertLessEqual(version(release), version(project_version()))

    def test_every_form_of_a_release_is_read(self):
        text = ("pip install crowdbook-engine==0.12.0 \"crowdbook-engine[gymnasium]==0.12.0\" "
                "crowdbook==0.12.0 --find-links .../releases/expanded_assets/v0.12.0\n"
                "git clone --branch v0.12.0 ... ; git clone -b v0.12.0 ... ; --branch=v0.12.0\n"
                ".../releases/download/v0.12.0/x.whl .../releases/tag/v0.12.0")
        self.assertEqual(named(text), ["0.12.0"] * 9)


if __name__ == "__main__":
    unittest.main()
