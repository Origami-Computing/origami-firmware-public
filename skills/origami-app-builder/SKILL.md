---
name: origami-app-builder
description: Build and iterate on native C++ custom apps for Origami/Kami, using the firmware app lifecycle, menu integration and verified application-only USB deployment.
---

# Build a native Origami app

Repository: [Origami firmware](https://github.com/Origami-Computing/origami-firmware-public). Locate the actual checkout containing `platformio.ini`, `include/app.h` and `src/apps/menu_app.cpp`. Use native C++ for new apps. Do not generate Lua widgets or expose new Lua bindings as an app-creation shortcut. Preserve existing device apps, Lua runtime and stored files.

## Set up or resume the custom project

Read `.origami/project.json` from the user's persistent project folder when present. Reuse its firmware checkout, interpreter and workspace after verifying paths. If source is missing, clone the repository above into `firmware/` under the chosen project folder; never overwrite an existing checkout. Read `README.md`, `docs/building.md` and applicable repository instructions for pinned setup and the baseline build. Local development does not require a Kami account.

Use this layout for a new project, or preserve an established equivalent:

```text
project/
  firmware/              # actual firmware checkout and compiled app source
  apps/<slug>/           # custom project brief, assets and test notes
  .venv/                 # isolated build tools
  .origami/project.json  # local paths and verification evidence
  .origami/private/      # private device recovery files, excluded from Git
```

The custom workspace is not a firmware plugin loader. Put compiled source in `firmware/src/apps/<slug>_app.cpp` and `firmware/include/apps/<slug>_app.h`, and register it in `firmware/src/apps/menu_app.cpp`. Record these actual paths and the selected branch. Inspect dirty files first and preserve unrelated work; a new branch does not isolate uncommitted changes.

## Implement and verify

Read [native development](references/cpp.md) and inspect a comparable working app, its service interfaces and focused checks. Keep the user's requested behavior central; choose reasonable defaults and ask only for missing decisions. Use existing App/Screen lifecycle, UI primitives and service ownership. Preserve system controls, menu, account setup and update paths.

Build the actual configured target using the pinned environment. Verify menu discovery, launch, primary action, wheel/select, Back, re-entry and affected resource handoffs. Keep build success separate from device verification.

## Install or recover

Read [installation constraints](../../docs/installation.md) and [application-only USB procedure](references/usb-deployment.md) before device writes. Identify hardware/USB identity and verify current partition/OTA metadata; save a verified known-good application backup before installing. Use the user's existing authorization, preserve NVS/SD/shipping data and write only the selected application slot. Never guess an offset or erase-all.

For recovery, use the intended verified backup and do not overwrite it with a broken application. A healthy supported device can supply its own application backup; a factory installer is not a prerequisite. Missing bootloader/layout/provisioning is a separate maintainer recovery case. Do not claim a public source archive restores those components or grants hosted access.

## Iterate

Save the app brief, source paths, menu path, artifact hashes and test results in the project record or `.origami/apps/<slug>/state.json`, preserving unrelated state. Report authored, built, flashed, booted and physically verified outcomes separately. Continue feedback in the same workspace. Device installation does not authorize release publishing, credential changes or unrelated erasure.
