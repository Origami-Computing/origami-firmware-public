# Frozen Agent behavior references

These exact production excerpts preserve the behavior before transport, Thinking
and completion ownership changed. The three matching `tools/test_agent_*.py`
checks compile them against the current functions with identical dependency
fakes and compare operation order and state. They require no Git history.

`manifest.json` records each full source commit, original path, selection and
SHA-256 of the excerpt. It documents provenance, not correctness by itself.
The excerpts were checked byte-for-byte against their recorded commits when
introduced. No formatting or cleanup was applied to the function bodies.

Keep these references frozen during behavior-preserving refactors. An intentional
behavior change needs explicit expected assertions and a documented decision;
do not regenerate the reference from the candidate just to make a check pass.
These are fragments compiled inside the host fixtures, not firmware sources.

The checks model hardware and rendering dependencies. Passing them does not
establish radio timing, real allocation failure, physical appearance or audio.
