# Frozen operation and presentation baselines

These exact production excerpts let the matching diagnostic-tone, camera-trial,
Morning-quote, list-row and screen-chrome host checks run without Git history.
They are reference inputs, not firmware source files. The camera excerpt includes
its following preprocessor terminator because the existing test selects the body
up to that marker; tone startup is an indented statement block.

The manifest records full source commits, original paths, selection delimiters,
full-source hashes and excerpt hashes. Excerpts were checked byte-for-byte against
those commits when introduced. No cleanup or formatting changes were applied.
Screen chrome uses its complete 100-line original translation unit; other checks
keep only the slices consumed by their existing comparisons.

Keep these references frozen during behavior-preserving work. An intentional
behavior change needs explicit assertions and a documented decision, not a fresh
reference copied from the candidate to obtain a pass. Provenance does not itself
prove correctness; each check compiles these baselines with its declared fakes.
Host results do not establish physical radio, audio, display or camera behavior.
