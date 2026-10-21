# Reporting a security issue

After public launch, use [Report a vulnerability](https://github.com/Origami-Computing/origami-firmware-public/security/advisories/new) to submit a private report to repository administrators. Do not put vulnerability details, credentials or device dumps in public issues.

**Private staging status:** GitHub private vulnerability reporting is available for public repositories. Activation was attempted while this repository remained private and GitHub returned 404. The form above is a launch-time route, not a claim that reporting is enabled now. During private review, use your existing private maintainer channel.

Include the firmware version, hardware revision, impact, expected/observed behavior and minimal reproduction steps. Remove Wi-Fi credentials, tokens, device identifiers and personal media. Do not flash or reset a device merely to produce a report.

Before launch, enable private vulnerability reporting and have a maintainer subscribe to security notifications and verify delivery. No email address or response-time commitment is invented here. GitHub's [configuration guide](https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/configure-vulnerability-reporting/configure-for-a-repository) describes the setup.

Firmware credentials are provisioned separately into device storage. Public trust roots and signature-verification keys are intentional public inputs. Keep OTA download keys, signing private keys and backend tokens out of all distributed files.
