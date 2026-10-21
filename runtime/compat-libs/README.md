This directory contains the legacy 32-bit compatibility libraries required by
the Loki Tribes 2 executable: Loki-era SDL 1.2, SMPEG, and the GCC 2.95 C++
runtime (`libstdc++-libc6.2-2.so.3`). `runtime/run-game` puts this directory on
`LD_LIBRARY_PATH` ahead of the host's 32-bit libraries.
