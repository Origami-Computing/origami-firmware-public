# Frozen alarm catalog reference

Exact function excerpts from production commit `cae54b08a92641a674ec685ca8a1f99f375eb93a` preserve
pre-extraction Alarm/Timer labels and previews, alert ring/gap selection and
interval feedback fallback. These are host-test inputs, not firmware sources.
The manifest records original paths, function selectors and full-source/excerpt
SHA-256 hashes. Excerpts were compared byte-for-byte to that commit when added;
only blank separators between selected functions were inserted.

`tools/test_alarm_catalog.py` verifies excerpt hashes and compiles the reference
separately from the current catalog. It needs no Git history. Keep references
frozen when editing the candidate: intentional behavior changes need explicit
assertions and a documented decision, not replacement baselines. Stub sound
identities verify routing; this test does not prove synthesis or acoustic parity.
