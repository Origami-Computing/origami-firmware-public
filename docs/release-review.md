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

1. The selected SDK archive set is classified in [SDK review](sdk-review.md); release CI verifies its notice inventory and repaired TLS linkage. Final qualification must use the newly built candidate, not a historical production binary.
2. Finish final physical confirmation after the successful USB recovery round trip documented in [installation](installation.md). Normal operation was confirmed by the user before recovery; restoration and reinstallation passed boot, SD mount, flash hash and live NVS-entry comparison checks. Network OTA rollback was not tested.
3. At public launch, enable GitHub private vulnerability reporting and verify a maintainer's notifications. GitHub rejected activation while private; notification access also requires an unavailable token scope.
4. Configure required release reviewers when supported. GitHub rejected the rule on the current private-repository billing plan (422). A review environment exists without protection rules; it must not be treated as an approval barrier. The publication approval flag remains false; the manual workflow creates drafts only.

No blanket claim of secret absence or third-party legal clearance is made. The source ownership instruction does not relicense upstream components.

Every new candidate must be tied to an exact source digest and firmware hash. A successful manual build does not constitute publication or binary/hardware approval; releases remain drafts. Do not change visibility until the applicable remaining items have been resolved and publication is explicitly authorized.

## Creating and promoting a release candidate

Follow the [private-to-public promotion runbook](public-promotion.md) for source sync, review, qualification, legacy-device migration and launch. The candidate workflow now builds a signed production-policy image and prepares its manifest without changing an OTA channel. Use an unused tag such as `v1.0.0-public-review.1` matching `platformio.ini`.

The publication workflow verifies and promotes the exact qualified candidate bytes. It requires the stable version and candidate tag, plus approval of the exact tracked tree. Draft creation does not set publication approval. A signed candidate is not hardware qualification or permission to change visibility.

The draft workflow is restricted to a private repository. A reviewed post-launch qualification path is required for subsequent releases after this repository becomes public. See the runbook's future-promotions section.
