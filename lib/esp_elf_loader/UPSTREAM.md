# Espressif ELF loader

Vendored from https://github.com/espressif/esp-iot-solution at commit
`cc06c1c383ba78f913596a176a4513c07657ede5`, component `elf_loader` version 2.1.0. Apache-2.0.

Only the Xtensa loader, structural validator, platform adapter and symbol lookup
are compiled. Dynamic filesystem lookup and arbitrary host symbol discovery are
not enabled. Kami registers an explicit game import table. PSRAM execution uses
the upstream ESP32-S3 adapter and the pinned IDF 4.4 cache path.

Local compatibility change: use GCC atomic pointer builtins for the symbol
resolver. The pinned GCC 8.4/newlib stdatomic wrapper rejects direct function
pointer initialization; sequentially consistent load/store semantics are retained.
Component version macros are supplied by library.json instead of IDF CMake.
