# Reporting a security issue

Security maintainer: [@vaibhavmaloo03](https://github.com/vaibhavmaloo03), designated by Origami. This account owns report triage and launch-time notification verification. Do not post vulnerability details on the maintainer’s public profile or in public issues.

Use [Report a vulnerability](https://github.com/Origami-Computing/origami-firmware-public/security/advisories/new) to submit a private report to repository administrators. Do not put vulnerability details, credentials or device dumps in public issues.

**Private reporting is enabled.** Reports submitted through the form are shared privately with repository administrators. The designated maintainer is subscribed to repository notifications.

Include the firmware version, hardware revision, impact, expected/observed behavior and minimal reproduction steps. Remove Wi-Fi credentials, tokens, device identifiers and personal media. Do not flash or reset a device merely to produce a report.

GitHub secret scanning and push protection are enabled. Main requires the security and firmware build checks; force pushes and branch deletion are blocked. Workflows from all outside contributors require approval.

Firmware credentials are provisioned separately into device storage. Public trust roots and signature-verification keys are intentional public inputs. Keep OTA download keys, signing private keys and backend tokens out of all distributed files.
