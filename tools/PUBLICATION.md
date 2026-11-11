# Early source-preview publication checklist

## Intended scope

Publish emulator source and openly distributable build/test tooling only.
MH2P is a **developer source preview**, not a supported turnkey HMI release.
The [MH2P guide](mh2p/README.md) records current functionality and limitations.
This fork is not an upstream QEMU contribution.

Do not include:

- Firmware updates, extracted QNX/vendor libraries, prepared NOR/mNAND disks,
  device captures, keys, phone backups, navigation maps or provisioning data.
- Firmware-derived CarPlay ModKit overlays or generated installer packages.
  Research/build helpers in the source tree do not make those payloads
  distributable or establish physical-unit installation safety.
- Session-state directories, private launchers, workstation configuration,
  raw traces, screenshots, JVM/core dumps or local test outputs.
- Personal VINs, serials, MAC addresses, phone/contact data or credentials.

The existing upstream QEMU firmware and test-key fixtures have separate
provenance and licensing. Do not remove upstream copyright notices,
author attribution or license files as a supposed PII cleanup.
The small historical MHI2 boot logs are diagnostic text, not firmware
payloads; one embedded vendor build path is explicitly redacted.

## Current-tree checks

From the checkout root:

```sh
python3 tools/check_publication.py
python3 -m unittest discover -s tools -p 'test_check_publication.py'
git diff --check
git status --short --untracked-files=all
```

The checker inspects tracked and nonignored untracked current files.
It flags common credential formats, private session paths, workstation
paths in head-unit tooling/changed/untracked files, external symlinks, and likely
head-unit binary payloads. It prints rule names and locations rather than
the matched values. It is heuristic: a pass is not proof of absence of
PII, secrets or copyrighted payloads. Review unknown text, generated
interface metadata, synthetic identities and every proposed release asset
manually. Ignored files are **not** release assets.

Keep all generated/private data outside the checkout. Do not publish a
recursive ZIP/tar of a developer working directory. Use an explicitly
reviewed Git tree/source archive; verify the archive's file inventory too.

### Source-only preview candidate

A history-free source archive is a separate publication route: it does not
expose old Git blobs or require rewriting the research branch. Assemble it
from an explicit source inventory, not by recursively archiving a checkout
or a tested snapshot (tests can leave caches and generated files there).
Include required pinned build dependencies only from their verified source
trees, preserving their licenses and attribution.

Normalize archive ownership and timestamps so workstation account metadata
is not embedded in archive headers. Reject absolute paths, escaping links,
Git metadata, build products, caches and private head-unit assets. Extract
the finished archive into a new directory and compare every member's content,
mode and link target with the reviewed inventory. Retain the archive checksum
and inventory beside the candidate.

This route does not make the existing research branch safe to push. A local
candidate is not a GitHub release or a licensing certification. Hosted CI
and any separately proposed binaries still require their own validation.

## Git history is a separate publication boundary

Cleaning the current files does not remove older copies from Git history.
The available shallow branch history has pre-existing workstation paths
in prior source revisions and local author/committer identity metadata.
Other local refs also contain session-labelled commits; those are not
reachable from the current branch and must not be pushed with `--all`
or `--mirror`. A current-tree scan must not be represented as a
clean-history verdict. Missing older history and the public-base history
also need consideration, not just the latest dirty diff.

Before any push, explicitly choose and review either:

1. Retaining history after reviewing its personal-data/provenance exposure;
   or
2. Preparing a clean publication branch/import from an appropriate base,
   with reviewed commit metadata and attribution preserved.

History rewriting, force-pushes, replacing public branches and inventing
author identities are not implicit cleanup operations. Coordinate with the
repository owner. Deleting a secret from history does not revoke it; any
real exposed credential must be rotated.

## Validation and release wording

- Review the full candidate diff, including previously untracked files.
- Build from a clean source snapshot in a new out-of-tree directory.
- Run firmware-free MH2P board tests and related Python/graphics regressions.
- Preserve shared-helper regression checks. Validate the Linux/macOS GitHub
  matrix rather than claiming portability from a local Linux run.
- Do not describe private HMI controls as a user-reproducible launcher.
- Describe graphics as bridge-assisted, audio/backend applications as
  incomplete, performance as variable and sustained health as unverified.
- Do not package a locally built emulator as matching cleaned source until
  that exact source revision has been built and tested.

Publication preparation does not itself commit, push, tag, upload assets or
create a GitHub release. The remaining watchdog investigation can proceed
separately after this source-publication boundary is agreed.
