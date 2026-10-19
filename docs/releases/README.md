# Releasing

A release is a tag, `vX.Y.Z`, on a commit of `main` whose `CMakeLists.txt` says `VERSION X.Y.Z`.
Pushing the tag builds every wheel and the source packages, runs the tutorial on them, and
publishes the GitHub release with `docs/releases/vX.Y.Z.md` as its notes
([.github/workflows/wheels.yml](../../.github/workflows/wheels.yml)); a release made by hand for
the tag first, in GitHub's page for a new release, is filled in the same way. Then the release's
files go to PyPI, as `crowdbook-engine`, `crowdbook-analysis` and `crowdbook`, by trusted
publishing from the environments `pypi`, `pypi-analysis` and `pypi-client`. A release published
before that step existed goes to PyPI by running the workflow on `main` by hand with its tag as
`publish`.

1. On `main`: set `VERSION` in `CMakeLists.txt` (the engine package takes its version from it),
   `version` in `analysis/pyproject.toml` and `clients/python/pyproject.toml`, run
   `uv lock --project analysis`, and write `docs/releases/vX.Y.Z.md`.
2. Tag that commit `vX.Y.Z` and push the tag, or publish a release for a new tag `vX.Y.Z` on
   `main` from GitHub's page.
3. Once the release is on PyPI, point the install instructions at it: `README.md`,
   `docs/quickstart.md`, `docs/tutorial.md` and `python/README.md` name the latest published
   release, so that what an outsider installs and the tutorial they clone match. The analysis
   tests check that these pages name one release, with notes, no newer than the project.
