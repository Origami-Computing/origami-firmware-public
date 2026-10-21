# Publication status

The repository remains private and the firmware release remains a draft. Actions support manual draft builds from main.

## Completed

- Independent main-only snapshot; the original private repository is unchanged.
- MIT selected for original work. Origami ownership of artwork and font explicitly confirmed on 21 October 2026; the two required SD art assets are now included.
- Original runtime source preserved; production compile-policy reproduction target added.
- Available third-party notices retained, including the missing ST header notice and Arduino LGPL notice; dependency sources assembled for review.
- Stronger source and extracted firmware-string scans run with redacted output. No live credential was identified; binary PEM delimiter hits were triaged as parser strings.
- Security reporting route and installation/recovery prerequisites documented.

## Concrete remaining items

1. Finish the nested vendor SDK license/notice classification before treating the original production binary as fully cleared. Package identity is now verified: all 3,407 compared framework/SDK files match the checksum-verified registry package named in the production logs. The source companion addresses the identified LGPL libraries; this is not an exhaustive legal clearance.
2. Qualify installation, settings preservation and recovery on an identified test device. A connected USB serial device was observed but not opened: the firmware guide warns that opening it can reset the board, and its role/baseline has not been established.
3. At public launch, enable GitHub private vulnerability reporting and verify a maintainer's notifications. GitHub rejected activation while private; notification access also requires an unavailable token scope.
4. Configure required release reviewers when supported. GitHub rejected the rule on the current private-repository billing plan (422). A review environment exists without protection rules; it must not be treated as an approval barrier. The publication approval flag remains false; the manual workflow creates drafts only.

No blanket claim of secret absence or third-party legal clearance is made. The source ownership instruction does not relicense upstream components.

Every new candidate must be tied to an exact source digest and firmware hash. A successful manual build does not constitute publication or binary/hardware approval; releases remain drafts. Do not change visibility until the applicable remaining items have been resolved and publication is explicitly authorized.

## Creating a release candidate

Open **Actions → Build firmware draft release → Run workflow**, select **main**, and enter an unused review tag such as `v0.1.213-public-review.2`. The workflow checks source, builds the custom production-policy firmware, and creates a **draft** GitHub Release with the application binary, checksums, source companion and notices. It never publishes a release or changes repository visibility. Use a new tag for each candidate; existing releases are not overwritten.

The version prefix must describe the firmware version configured in `platformio.ini`; increment the review suffix for another build of that version. These candidates are custom builds, not signed official OTA updates. The original production image is retained separately in the first review draft.

Before any public launch, complete the outstanding review above and run `python3 tools/check_release_approval.py` against the approved tree. Draft creation deliberately does not set or bypass publication approval. GitHub administrators can manually publish drafts, so the documented review remains required.
