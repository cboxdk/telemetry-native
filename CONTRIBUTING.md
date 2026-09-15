# Contributing

1. Fork, branch off `main`.
2. Build and run the suite on Linux, not just your laptop — the production timer
   backend only exists there:
   ```bash
   phpize && ./configure --enable-cbox-telemetry && make clean && make -j8 && make test
   ```
3. If you touched the sampler, the hooks or the crash recorder, also run:
   ```bash
   php benchmarks/run.php modules/cbox_telemetry.so 21 1000
   ```
   and include the numbers in the PR. A change that costs overhead needs to say
   how much.
4. Memory safety is not optional in C. Before proposing anything that touches
   the arena, the frame table, the trie or the crash record:
   ```bash
   USE_ZEND_ALLOC=0 valgrind --error-exitcode=99 php -n -d extension=modules/cbox_telemetry.so your-fixture.php
   ```
5. Describe **why** in the PR, not what — the diff already says what.

Read `AGENTS.md` first. The invariants there are not style preferences; each one
is there because breaking it produced a bug.
