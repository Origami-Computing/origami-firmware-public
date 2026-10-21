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
