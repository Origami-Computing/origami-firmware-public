# Promoting changes into kamiOS public releases

This is the maintainer runbook for moving approved work from the private firmware repository into the independent public repository. It covers the first kamiOS 1.0 launch and subsequent updates. Completing a source sync does not publish a repository or qualify a firmware image.

## Repositories, channels and responsibilities

| Destination | Source | Intended download access |
| --- | --- | --- |
| Public | Reviewed snapshot in `Origami-Computing/origami-firmware-public/main` | Production and developer keys |
| Private | `Origami-Computing/origami_firmware/main` | Developer keys |
| Staging | `Origami-Computing/origami_firmware/staging` | Developer keys |

These are the target access rules. During the initial migration, legacy Production remains available to existing production and developer keys until a compatible bridge is verified. Do not describe the transitional deployment as Public-only enforcement. Repository visibility and authenticated OTA access are separate controls: making source public does not grant a key access to Private or Staging.

The public repository has its own history. Never mirror, force-push, merge the private repository's history, or import its branches/tags into the public repository. Preserve private PRs, CI, deployments and development branches. A public release is a deliberate promotion, not an automatic copy of every private commit.

Assign a maintainer to each promotion. That maintainer records the source revision, reviews exclusions and security changes, collects test evidence and approves publication. A repository administrator performs the visibility change. The vulnerability-reporting contact must have access to receive and respond to reports. Keep account IDs, device identifiers, private commit mappings and raw device logs in the private operational record where appropriate.

## 1. Select an exact source revision

1. Fetch private `main` without modifying its working tree or remote branch. Record the full commit SHA and fetch time in the private promotion record. “Latest” means that exact revision, not a moving reference throughout the review.
2. Compare it with the last promoted private revision. Inventory added, changed, renamed and deleted files, including build settings and dependencies. Review changes outside `src/` and `include/`; firmware behavior also depends on configuration, assets and build scripts.
3. Create a branch in the public repository from its current `main`. Keep unrelated local edits intact. Use a separate temporary checkout if the existing checkout is busy.
4. Export reviewed file changes or a patch without private Git metadata. Apply changes individually or as a reviewed three-way patch. Do not blindly replace the public tree or resolve conflicts by choosing an entire side.
5. Reconcile each conflict against the runtime owner and public policy. Preserve production channel restrictions, disabled production LAN listener/setup portal, credential redaction, Lua limits, TLS certificate validation, signature checks and the MbedTLS repair. Keep public workflow and documentation changes unless deliberately superseded.
6. Record an exclusion reason for each omitted change: proprietary feature, credential, internal operation, incompatible dependency, private-only tooling or deferred behavior. Never silently omit a user-facing fix.
7. Keep the public version explicit. Private build numbering is independent: importing private `v0.1.215` changes does not rename kamiOS `v1.0.0` or prove a newer public release is installed.

Commit the resulting snapshot to the public promotion branch. Commit messages and PR descriptions must themselves be safe for eventual publication. Keep sensitive review evidence outside Git. If private `main` advances during qualification, either finish the pinned candidate or start a new review; do not silently add changes to approved bytes.

## 2. Review source, rights and secrets

Review the complete proposed public tree and all reachable public-repository history, not only the final diff. Check branches, tags, PR attachments, release assets and generated archives for accidental private data.

Run the repository secret and architecture checks:

```sh
python3 tools/check_tracked_secrets.py
python3 tools/check_architecture_dependencies.py
python3 tools/test_architecture_dependencies.py
```

Also run the independent secret scanner used by the project with redacted output, including history and extracted release artifacts. Inspect scanner findings rather than treating either a hit or a clean report as conclusive. Never upload device flash dumps, NVS, private keys, credential-bearing installers, local environment files or raw diagnostics containing user data. A leaked credential must be revoked/rotated; deleting the visible file does not revoke it.

For every new dependency or asset, record origin, pinned version/hash, applicable license, modifications, required notices and source/relink materials. MIT covers Origami's original work; it does not replace upstream licenses. Owner-confirmed artwork and Instrument font do not confer ownership of bundled GNU fonts. Retain component-specific licenses, editable font source where required, LGPL library source and the exact materials needed to rebuild/relink.

Use [distribution requirements](distribution.md), `third_party/dependencies.json`, `third_party/sdk-provenance.json` and the retained notices. Resolve nested SDK classification before declaring the binary cleared. Classify security advisories against the actual chip, build configuration and reachable APIs; record mitigations and unresolved exposure. A version number or a successful build alone cannot establish that a vulnerable path is absent.

## 3. Integrate and validate the public snapshot

Open a PR against public `main`. Describe behavior changes, intentional exclusions, retained security differences and the checks actually run. Required CI must pass on the final revision. Keep main protection, required checks, force-push prevention and deletion prevention enabled; do not bypass a failed check to create a release.

Run the relevant host regressions and the production build. CI is the authoritative list of required automated checks; supplement it for the changed behavior. In particular, OTA changes need access-policy, signed-manifest, download, version-selection and candidate-integrity tests. TLS changes need failure-path tests and an actual authenticated network operation on the device.

After review, merge the PR into the public repository. This changes its source snapshot, not repository visibility. If the repository is still private, keep it private through qualification. Keep the evidence tied to the merged source and discard stale approval when source changes.

## 4. Build a signed private candidate

The current first-launch workflow is **Actions → Build firmware draft release**. Run it on `main` with a fresh tag matching `platformio.ini`, for example `v1.0.0-public-review.1`. Increment the review suffix for each candidate. This workflow is intentionally restricted to a private repository; see the post-launch section below before making future releases.

The workflow builds `esp32-s3-menu-n16r8-release-review`, records the tracked source digest, packages source/notices and prepares a signed Public manifest. It creates a draft release and does not change an OTA channel. Signing is performed using the Actions secret; never embed the private signing key in source or firmware.

Verify these candidate assets:

| Asset | Purpose |
| --- | --- |
| `firmware.bin` | Application image; not a full flash backup or address-zero installer |
| `latest.json` | Signed Public OTA manifest identifying version, hardware, layout, path, size and hash |
| `REVIEW.json` | Source revision/digest and firmware SHA-256 |
| `SHA256SUMS.txt` | Integrity inventory for the packaged assets |
| `source-review.tar.gz` | Corresponding source, retained dependency material and rebuild/relink inputs |
| `LICENSE`, `THIRD_PARTY_NOTICES.md`, `NOTICES-REVIEW.txt` | Ownership and component notices; provisional labels are not clearance |
| `REVIEW.txt` | Candidate provenance and its review boundary |

Download the candidate into a private directory and run:

```sh
python3 tools/verify_release_candidate.py --directory dist --version v1.0.0
```

Use the matching reviewed source checkout. A source-digest mismatch, invalid signature, wrong image hash, unexpected manifest target or missing source/notices is a stop condition. Fix the cause and create a fresh candidate. Do not repair the checksums to conceal changed bytes.

## 5. Qualify the exact image on hardware

Identify the board, USB port, installed baseline, hardware revision and partition layout. Verify that another operator/chat is not using the board. Save a private recovery backup and protect it as credential-bearing data. Do not erase NVS, SD content, factory data or eFuses as a shortcut.

A private GitHub draft is not automatically an OTA endpoint. After USB qualification, **Deploy private Public-channel qualification candidate** can deploy the exact signed candidate while the repository is private. This workflow verifies its source digest and signature, does not publish the GitHub release, and does not bypass final publication approval. Deploying a qualification candidate to a live channel is a separate controlled action: it may reach other authorized devices. Establish the test audience and rollback plan first. Do not mark source/binary review approved merely to bypass the publication workflow for testing.

Record each result against the exact candidate hash:

- Production key can select and download Public; Private and Staging are rejected by both device policy and server access.
- Developer key can select Public, Private and Staging. Explicit cross-channel changes work even when a private version number is lower; same-channel rollback rules remain enforced.
- Missing/invalid keys cannot download restricted firmware. Clear temporary test credentials afterward.
- Invalid signature, corrupted image, wrong hardware/layout and interrupted download fail safely without switching the boot partition.
- Perform a real OTA install, reboot and version/hash confirmation. A `verify-download` result proves download validation only; it is not an installation result.
- Confirm screen, buttons/wheel, speaker, networking, storage and the changed user flows. Automated logs do not establish what a person sees or hears.
- Exercise the supported recovery path and confirm the baseline or approved candidate boots afterward with preserved settings/media. Keep USB restoration and OTA rollback results distinct.

Keep raw logs and backups private. Publish only sanitized summaries. If a step is unavailable, record the gap and keep the candidate unqualified.

## 6. Migrate existing devices safely

Download access keys and manifest signing keys have different roles. Existing production/developer download keys can keep working if the server retains their authorization. Devices verify manifests with the embedded public verification key; that is not the private signing key used by the publisher.

Older devices may request the legacy Production path and require a Production manifest. A new Public manifest alone does not redirect them. Before closing legacy access:

1. Verify a signed bridge accepted by the old client, with compatible hardware/layout and the expected legacy path/channel.
2. Ensure future private-main builds publish to Private rather than replacing that bridge with private firmware.
3. Install the bridge from an actual old device, reboot, and prove it reaches Public with its existing key.
4. Prove production keys cannot obtain Private or Staging, including direct asset URLs. Avoid a broad legacy Production exemption that exposes private firmware history.
5. Narrow or retire legacy access only after migration evidence and a supported recovery route exist.

Retain existing OIDC-provider ownership during AWS updates. Inspect the exact change set and account, use least-privilege channel publisher roles, and verify deployed behavior rather than assuming a template has taken effect. Never disable certificate verification or signature enforcement to work around a failed migration.

## 7. Approve and publish the release

Only after source, license, security-contact and hardware review are complete, update `release-approval.json` with a real reviewer and the exact digest from:

```sh
python3 tools/check_release_approval.py --digest
```

The approval file itself is excluded from that digest so the approval-only commit can refer to the qualified tree. All other tracked changes invalidate the candidate's source match. Record hardware evidence separately: the approval script explicitly does not certify binary/hardware review.

Run **Publish approved public firmware** on `main` with stable tag `v1.0.0` and the qualified candidate tag. It verifies the candidate and promotes the same firmware bytes, rather than rebuilding after hardware qualification. It creates the release draft, publishes/verifies the Public OTA bytes, then makes the release visible within the repository's current visibility. An AWS failure can leave a draft or staged objects; inspect before retrying. Never overwrite an already published version with different bytes.

Check the release page contains the application binary, source companion, notices and checksums. Confirm the downloaded GitHub binary and authenticated OTA binary match the approved hash. Confirm the live signed channel pointer and actual device upgrade. Tag creation or an Actions success badge alone is insufficient.

## 8. First public visibility change

This is a separate owner decision after the release is ready. Before changing visibility:

- Review all reachable history, branches, tags, releases and attachments one final time. Remove only unneeded private review material through an explicitly reviewed cleanup; do not rewrite the original private repository.
- Verify `main` protection and required CI remain active, collaborator permissions are appropriate, and Actions secrets are not exposed to untrusted PR code.
- Confirm `SECURITY.md` names a usable reporting route and a maintainer can receive notifications. Enable GitHub private vulnerability reporting as soon as repository eligibility permits and test the reporting route.
- Configure release/environment protection where supported. An unprotected environment is not an approval barrier. Document plan restrictions rather than claiming a rejected setting is enabled.
- Verify README installation/recovery instructions, product name, version, release links and license notices describe the actual released image.
- Obtain explicit approval to make this specific repository public. Public source can be copied permanently; reverting visibility is not recall.

After the administrator changes visibility, check the repository and release from a signed-out browser. Recheck main protection, security reporting and download assets. Verify production-key access to Public and denial of Private/Staging independently of GitHub visibility. Keep the original private repository private.

## 9. Future promotions after launch

Repeat this process for every new public version. Increment the version deliberately and use an independent public PR. Never publish private work merely because private CI passed. Preserve an internal source-to-public mapping and a public changelog containing only publishable information.

**Current workflow limitation:** `release.yml` runs only while this repository is private. Before the second release, implement and review a post-launch qualification path. A private qualification repository or isolated private build process can produce candidates for the public main digest; its trusted workflow, source verification, signing-key access and exact-byte promotion must be designed together. Do not simply remove the privacy condition and assume draft releases hide private source or Actions logs in a public repository.

If a release fails, stop further promotion, preserve evidence, and use the verified recovery/migration procedure. Restore a channel pointer only to a compatible signed image that clients accept; version checks may reject a downgrade. If a signing key is compromised, plan a trust-key transition and device recovery, not just a new GitHub secret. Keep the vulnerability handling process separate from ordinary feature promotion.

## Promotion record template

Store the detailed record privately; publish a sanitized release summary.

```text
Product/version:
Private source revision and fetch time:
Previous promoted source revision:
Public PR and merged commit:
Reviewed inclusions/exclusions:
Source digest:
Candidate tag and Actions run:
Firmware SHA-256, size, hardware/layout:
Dependency/license review and source companion hash:
Secret scan scope/results and triaged findings:
Security review, mitigations and remaining risks:
Host tests/build result:
Identified test board and baseline (private):
Production/developer access matrix:
Actual OTA install/reboot evidence:
Physical behavior confirmation:
Recovery and baseline restoration evidence:
Legacy bridge migration result:
Reviewer and approval date:
Release URL and matching download hashes:
Main protection/security reporting verification:
Visibility approval and signed-out verification:
Remaining blockers and owners:
```
