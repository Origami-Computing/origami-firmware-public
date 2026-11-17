# Contributing

Public contributions open after the migration review is complete. Keep changes focused and include the reason, affected hardware, and verification results.

Apps own workflows; screens own view lifecycles; services own operational sessions and cancellation; core owns reusable models and algorithms; HAL owns device access and bus policy. Preserve cleanup, memory budgets, rendering paths, and resource reservations. Do not put blocking network or device work in drawing callbacks.

Before submitting:

```sh
python3 tools/check_tracked_secrets.py
python3 tools/check_architecture_dependencies.py
python3 tools/test_architecture_dependencies.py
pio run -e esp32-s3-menu-n16r8-production
```

Hardware changes also require focused testing on the matching board. A successful build is not proof of device behavior. Do not upload private logs, credentials, recordings, screenshots of user data, or generated firmware into source control. Preserve third-party notices and document the provenance of new assets.

## Release notes are part of a production promotion

Every production/public firmware version must include a brief entry in
[CHANGELOG.md](CHANGELOG.md), reviewed against the diff from the previous published
release. Use 1–8 short, concrete bullets explaining the changed behavior and the
trigger of each fix. Include additions and relevant upgrade limitations; do not
reuse a feature list from an older release or say only “bug fixes.” Keep private
source mappings and device evidence out of the public log.

Update the version and its log together, before building/qualifying the candidate.
Run `python3 tools/release_notes.py`. CI requires an entry for the configured
version; publication additionally checks its baseline against the latest public
release. Automated checks enforce format and version, not the truth or completeness
of prose: the promotion reviewer must check every user-facing change against the
public diff and actual verification. Do not claim device validation from a build.

Use `## vX.Y.Z`, then `Changes since vA.B.C.`, then plain `- ` bullets. Historical
entries stay as release records. For a tooling-only or repackaging release, say
exactly that rather than inventing firmware fixes. See the [promotion runbook](docs/public-promotion.md).
