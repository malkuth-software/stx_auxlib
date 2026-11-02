# stx_auxlib

`stx_auxlib` is the owned seam between portable code and its platform. Every
call whose behaviour depends on the operating system, the C library or the
C++ library's host services (threads, clocks, files, environment, process
control, console output) goes through it, so that:

- GUI.Forms programs behave identically on every host, with platform
  differences settled once, here, instead of in each caller;
- the surface that could in theory be abused (format strings, unchecked
  buffers, environment and path injection) is small, typed and owned;
- a program can become mostly freestanding, and eventually run where no
  operating system is present (UEFI is the long-term target), by replacing
  one implementation directory.

Core mathematics stays with the toolchain, where it is already proven and
fast: `<cmath>`'s exact operations (`sqrt`, `floor`, `ceil`, `round`,
`fma`, `fabs`...), the `<cstring>` memory primitives, `<bit>`, `<numeric>`,
`<algorithm>`, `std::atomic` and `std::chrono` arithmetic are out of scope.
Transcendental functions (`sin`, `cos`, `exp`, `log`, `pow`...), whose
results differ between C libraries, are the exception: their owned,
reproducible kernels live here (`math.hpp`).

Public headers name no platform type. `src/hosted/` implements the surface
over the C++ standard library and its host; a freestanding implementation
would replace it whole.

## Modules

| Module | Seals | State |
|---|---|---|
| `hash.hpp` | (new) BLAKE2s-256, keyed and unkeyed, and secure erasure | done |
| `precedence.hpp` | (new) the precedence chain: forward-secure, publicly verifiable proof that one record preceded another | done |
| `voting.hpp` | (new) Preference Ranked Voting (PRV 1.0) | done: every outcome matches the reference on 420 elections |
| `stx_btree/` | (brought in) bbtree's ordered maps and the optimal search trees, from malkuth-software/stx_btree | done: its own tests pass in every mode |
| `memory.hpp` | `malloc` and `free` where our code wants its own storage (named calls; nothing is redirected) | done: mimalloc v3.5.3 behind the seam; host malloc under sanitizers; tag check mode |
| `thread_pool.hpp` | the owner's `threadpool_atomic_fast` pool | done: conformance-tested against the C reference |
| `text.hpp` | `snprintf`/`sprintf`/`vsnprintf`, `sscanf`, `std::to_string`, `stoi`/`stod`/`stoull`..., `atoi`/`atof`/`strtod`/`strtol`, `setw`/`setprecision`/`setfill`/`fixed`/`hex`, `setlocale` | done; not covered yet: `std::quoted`, general string streams |
| `threads.hpp` | `std::thread`/`jthread`, `this_thread::get_id`, `sleep_for`, `sleep_until`, `yield`, `hardware_concurrency`, `std::mutex`, `lock_guard`/`scoped_lock` of one mutex, `condition_variable`, `call_once` | done; not yet covered: `std::async`/`future`/`promise`, detached threads, multi-mutex `scoped_lock`, `recursive_mutex` |
| `clocks.hpp` | `steady_clock::now`, `system_clock::now` | done |
| `calendar.hpp` | `gmtime`, `localtime`/`localtime_r`/`localtime_s`, `mktime`, `timegm`, `strftime`, `std::put_time` | done: calendar host-free; local zone POSIX tested, Windows compile-checked only |
| `math.hpp` | `sin`, `cos` (as `sincos`), `tan`, `exp`, `log`, `log2`, `log10`, `pow`, `hypot`, `cbrt`, `atan`, `atan2`, `asin`, `acos`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, for float and double | done; speed pass and Payne–Hanek reduction for `sincos` and `tan` above 2^20 to come |
| `files.hpp` | `fopen`/`fread`/`fwrite`/`fclose`/`fseek`/`ftell`/`fflush`, `std::ifstream`/`ofstream`/`istreambuf_iterator`, `fsync`, `rename`, `unlink`, the `std::filesystem` operations | done: POSIX back end tested; Windows back end compile-checked only |
| `environment.hpp` | `getenv`, `setenv`, `unsetenv` | done: POSIX tested; Windows (wide Win32 block) compile-checked only |
| `process.hpp` | `exit`, `abort`, `atexit`, `assert` | done: `exit_process`, `fail`/`ensure` (one fault handler), exit tasks with context |
| `console.hpp` | `printf`/`fprintf`/`puts`/`fputs`/`fwrite`/`fflush` to standard streams, `std::cout`/`std::cerr`, `std::getline(std::cin)`, `isatty` | done: POSIX tested; Windows (console API as UTF-16) compile-checked only |
| `paths.hpp` | `std::filesystem::path` | done: owned UTF-8 path |
| `random.hpp` | `rand`, `srand`, `std::random_device`, `std::mt19937`, the standard distributions, `std::shuffle` | done; Windows entropy compile-checked only |
| `capabilities.hpp`, `children.hpp`, `desktop.hpp`, `terminal.hpp`, `events.hpp` | `posix_spawn`/`fork`/`exec`/`waitpid`/`kill`/`CreateProcessW`, `ShellExecuteW`/`open`/`xdg-open`, `_NSGetExecutablePath`/`/proc/self/exe`/`GetModuleFileNameW`, `termios`/`TIOCGWINSZ`/console modes, `sigaction`/`sigprocmask`/`pthread_sigmask`/`SetConsoleCtrlHandler`, SIGPIPE | done: POSIX tested; Windows compile-checked only. Not covered: dynamic loading (`dlsym`; only GUI.Forms host adapters use it), `renameat2` and `syscall` in SwiftEdit's file code |

Owner decisions of 2026-10-31:

- Allocation goes behind a host-backed seam, now mimalloc.
- Text formatting and parsing get an in-house locale-free implementation.
- Exact mathematics stays with the toolchain; transcendental kernels
  move here, written in a separate session.
- The consumers' own platform files (swiftedit, plan-paint, PlaySuite,
  Pocket Library) are sealed here; GUI.Forms' host adapters
  (`src/host/<platform>/`) stay the platform layer.
- Paths get an owned UTF-8 type with `/` separators, converted to the
  native form only inside `src/hosted/`.
- Randomness is hoisted whole: entropy, generators, distributions and
  shuffle, each with a named, frozen algorithm so a stored seed reproduces
  forever. The default stream is ChaCha12 (unpredictable from its output,
  reproducible from a 256-bit seed); xoshiro256++ is offered by name for
  hot loops where speed matters and security does not. Bounded integers
  use Lemire's unbiased method, floats take 53 random bits, and shuffles
  are Fisher–Yates in a specified direction.
- Entropy fails closed: when the platform has no entropy source (UEFI
  firmware without EFI_RNG_PROTOCOL, for example), a request for an
  unpredictable seed returns a named failure, never a weak seed.
- Exceptions and RTTI stay with the toolchain for the mostly-freestanding
  phase. UEFI prerequisite: ship an unwinder and the C++ ABI runtime there,
  or move subsystem boundaries to explicit results first.
- Desktop-only services (opening a URL, spawning a process, raw terminal
  mode, interrupt notification) become named capabilities that report
  themselves unsupported where a platform lacks them.

## Verification

Every change runs six builds of the whole suite: plain, mimalloc's secure
mode, UBSan, ASan with UBSan, TSan, and integer checks
(`STX_AUXLIB_INTEGER_CHECKS=ON`). The integer build halts on any unsigned
wrap, signed overflow, out-of-range shift or value-changing implicit
conversion. Every wrap in the library is therefore either impossible by
construction or inside a function marked `STX_AUXLIB_MODULAR`
(`src/checked.hpp`). Only ChaCha, SplitMix64, xoshiro, BLAKE2s, the
environment's change counter and the math kernels' table-offset arithmetic
carry the mark.
`tests/integer_ignorelist.txt` names the exempt code: mimalloc, the C++
library's headers, and the SplitMix64 generator of stx_btree's unchanged
tests. The math kernels carry their own marks and are not exempt.

Under TSan, stx_btree's shared map is the one exception. Its optimistic
lock coupling reads node bytes that a latched writer may be changing and
discards them when the version moves (`btree/docs/DESIGN.md`). These reads
are races in the C++ memory model and, by design, not atomics.
`tests/tsan_btree_suppressions.txt`, which only the btree tests use,
suppresses reports by the name of the optimistic reader (key counting,
`get_copied`, `try_append`, descent, batches). A race that involves none of
them, such as two writers racing, still fails. On 2026-10-31 every one of
about 1,000 reports paired a latched writer with one of those readers.

`stx_auxlib_performance_bench` (built with `STX_AUXLIB_BUILD_TOOLS=ON`)
times every operation against its platform equivalent and fails when any
is more than twice the platform's time (owner's ceiling, 2026-10-31). On
the M4, 2026-10-31, under load, the slowest ratios are reading an
environment variable (1.5–1.7x: a lock and validation on top of `getenv`),
setting one (1.6x) and `inspect` (1.4x); allocation, die rolls, shuffles,
civil time and ISO text are faster than the platform. ChaCha12 is 6–8x
faster than the platform's secure generator (`arc4random`) and 2.3–2.9x
slower than `mt19937_64`, the fast generator's counterpart, which
xoshiro256++ matches within 1.5x. BLAKE2s hashes 64 bytes in 0.6x the time
of CommonCrypto's SHA-256, and stx_btree's insert-and-find workload takes
0.74x the time of `std::map`.

The math kernels' own gate (`stx_auxlib_math_tests --bench-gate`) is a
report, not part of the failing matrix. On 2026-10-31 the owner accepted the
math speeds as they stand, though a few ratios are still near or above 2x
(double cbrt about 2.3x, double hypot about 2.1x, double log2 on wide ranges
about 2.2x). Every one-argument float function is verified correctly
rounded over all floats against the test reference, except sin, cos and tan
on 2^20 < |x| <= 2^32, which are sampled; pow, hypot and atan2 (two
arguments) are sampled. Of the double bounds, only sin, cos and tan's are
proved (see Mathematics); the others are measured over sampled inputs.

## Building

```text
cmake -S stx_auxlib -B build/stx_auxlib
cmake --build build/stx_auxlib
ctest --test-dir build/stx_auxlib --output-on-failure
```

## Memory

Call `stx_auxlib::memory::allocate(size, alignment)` and `release(block)` for
storage our code owns: substantial or runtime-sized buffers held by explicit
owners (heap arrays, arenas, pools). Nothing is redirected to this allocator:
`operator new`, `delete`, `malloc` and `free` stay the toolchain's, and foreign
modules keep their own allocators. Only our code allocates from it and only our
code releases to it (docs/ALLOCATOR_OWNERSHIP.md). The backend is mimalloc by
default (`STX_AUXLIB_ALLOCATOR=mimalloc`); a build whose flags enable address or
thread sanitizers defaults to `host`, which uses the C library's malloc so the
sanitizers keep checking the whole heap. `STX_AUXLIB_MIMALLOC_SECURE=ON` turns
on mimalloc's security mitigations (mimalloc cites under 25% cost).

`STX_AUXLIB_ALLOCATION_TAGS` (`AUTO`, on in Debug and sanitizer builds) puts a
16-byte tag below every block. `release` of a block from anywhere else, or a
second release, then fails the process with a message, and a foreign `free` of
one of our blocks traps; `stx_auxlib_ownership_death_tests` checks all of these.
Release builds carry no tag.

On MinGW every module links the C++ runtime statically: stx_auxlib adds
`-static` to its consumers' links and refuses flags that ask for the shared
runtime, whose DLL would free across the module boundary.

## Threads and clocks

`MonotonicClock` and `WallClock` meet the standard Clock requirements, so
`std::chrono` durations and arithmetic work unchanged; they replace
`steady_clock` and `system_clock`. Both count signed 64-bit nanoseconds;
`WallClock` is Unix time.

`Thread::start(entry, context)` returns a `ThreadStart` holding the thread
or its failure. The entry is a named `noexcept` function receiving the
thread's `StopSignal` and its context. Destruction and move assignment
request a stop and join, as `std::jthread` does; `join` refuses a
self-join instead of deadlocking. `current_thread()` returns a
`ThreadIdentity` that is never reused, unlike `std::thread::id`, and a
`Thread`'s identity is known before it runs. `Mutex`, `ScopedLock`,
`ConditionVariable` (with `wait_until` and `wait_for`, which saturate
rather than overflow) and `OnceFlag`/`call_once` keep their host objects in
fixed private storage, so they allocate nothing and expose no host type.

## Calendar

Civil dates and times are proleptic Gregorian, computed by owned,
host-free code (Hinnant's civil-day algorithms), so they agree on every
host and on UEFI; the tests check every day of WallClock's range against
`std::chrono::year_month_day`. `civil_from_wall` and `wall_from_civil`
convert exactly between `WallClock` and a `CivilTime` (1677-09-21 to
2262-04-11, no leap seconds), refusing invalid fields instead of
normalizing them as `mktime` does. `local_time` is the only host call: the
host's zone (`TZ` or the system zone) gives the local reading and its offset
east of UTC, reported as unsupported where a host has no zone. On POSIX
hosts it reads `TZ` under the environment lock, so it cannot race with
`set_variable`. `append_iso8601` writes ISO 8601 / RFC 3339 text (the
common `"%Y-%m-%d %H:%M:%S"` is `{.separator = ' ', .zone =
ZoneDesignator::none}`); `parse_iso8601` reads it back to an instant and
requires a zone, since text without one names no instant. Month and
weekday names are left to translated language files, not to a C locale.

## Mathematics

`stx_auxlib::math::sincos(x, sine, cosine)` for `float` and `double` is a
256-entry table with a cubic correction, giving the same bits on every host
with or without FMA: the rotation uses no exact product (the owner's 26-bit
split, 2026-11-02: table values and r cut to 26 significant bits, so their
product is exact as a plain multiply), and `tan` forms (S + C tan r) /
(C - S tan r) the same way. Every `float` with 2^-27 <= |x| <= 2^20 is correctly
rounded (788,529,154 values, checked exhaustively against the test
reference). For `double`, sin, cos and `tan` are within 0.51 ulp at every
double 2^-27 <= |x| <= 2^32, and that bound is proved, not sampled:
`tools/sincos_certificate.py` finds the smallest remainder |x - q pi/128|
of every binade (2^-66.49 at worst) over every q, bounds the rotation's
error by interval analysis of every rounding (at most 0.0011 ulp for sin
and cos, 0.00098 for tan), and bounds each reduction path's error from its
exact constants; inputs within 2^-30 of a multiple of pi/128 take the
eight-part reduction. The certified worst cases run in ctest
(`tests/reference/sincos_worst_cases.hpp`). To rerun the certificate (about
15 s), build with `STX_AUXLIB_BUILD_TOOLS=ON` and run
`python3 tools/sincos_certificate.py --search
<build>/stx_auxlib_sincos_minimum_remainder --check
tests/reference/sincos_worst_cases.hpp`; it fails if any step, or the
table, no longer holds. The 0.51 ulp bound is a stated budget (the
coordinator's provisional choice, 2026-11-02), named once in the
certificate as `STATED_BOUND_ULPS`. Measured over 120 million
sampled inputs, the largest error is 0.500415 ulp, against 0.87 for the
host library (before the split, which changed 46 of 8 million sincos and 64
of 4 million tan results, all within the bound); about 2.5 ns (`float`) and 5.5 ns (`double`) per pair
against 7–12 ns for `std::sin` plus `std::cos` on an A18 Pro (the
author's measurement).

`stx_auxlib::math::exp` for `float` and `double` owns its whole input
range, overflow and gradual underflow included. Measured by its author:
every finite `float` is correctly rounded (4,278,190,080 values,
exhaustive); `double` is within 0.500001 ulp over 160 million inputs in
eight regions, and every subnormal result is correctly rounded.

`log`, `log2` and `log10` own every input (zero, negatives, infinities,
NaN, subnormals) and are exact where the result is representable:
`log(1)` is +0, `log2(2^k)` is k, and `log10(10^n)` is n for n <= 22 in
`double` and n <= 10 in `float`. Measured by their author: at most 0.500000
ulp in every For `float`, each is correctly rounded for every positive finite value
(2,139,095,039 each, subnormals included, exhaustive). For `double`, over
240 million inputs in four regions, the largest errors are 0.500000 ulp
(`log`), 0.500003 (`log2`) and 0.500012 (`log10`); 10 results are not
correctly rounded.

`pow` owns every input as C99 Annex F specifies; its special cases match
the host bit for bit wherever the result is a zero, an infinity, one or
NaN. Exact powers are exact (`2^n`, `10^n` for n <= 22, small integer
powers below 2^53, `pow(4, 0.5)`). `double` is within 0.5 + 0.076 ulp by
an analytic bound over every rounding of its one pass in log2 units (no
rounding test, so no second path; the same bits at every SIMD level), and
measured within 0.5027 ulp over 2 million inputs in five regions against
MPFR; every sampled subnormal result is correctly rounded. `float` is
correctly rounded at 189,000 random pairs.

`hypot` owns every input per Annex F (an infinite operand gives +inf even
beside a NaN) and is exact for Pythagorean triples at every scale from
2^-1060 to 2^1010. `double` is correctly rounded at every sampled input
(500,000 over five regions, near overflow and subnormal results included,
against the host's worst of 1.18 ulp); `float` is correctly rounded at
300,000 random pairs.

`atan`, `atan2`, `asin` and `acos` match the host bit for bit on the
Annex F special grids (`acos(1)` is +0, `atan(±inf)` is ±pi/2). `double`
is correctly rounded at every sampled input (700,000, including the regions
next to ±1 for `asin` and `acos`; worst 0.499998 ulp, against the host's
0.66–0.92); `float` is correctly rounded at every 997th value of each
domain and at 300,000 `atan2` pairs.

Speed is not yet tuned: on a loaded A18 Pro, `pow` takes about 30 ns
against the host's 12.5 (`double`) and 3.7 (`float`), and `log` and `exp`
about 6 ns against 2.5–7. A speed pass, `pow` and the `float` paths first,
follows the remaining families.

`tan` is within 0.51 ulp for `double`, proved as above (0.499996 in the
sampled regions, about 0.003% misrounded in a wider probe, against the
host's 2.7), and correctly rounded for every `float` up to 2^20
(exhaustive against the test reference). `cbrt` and the six hyperbolic
functions own every input (`acosh(1)` is +0, `atanh(±1)` is ±inf, NaN
outside their domains), are within 0.500000 ulp at every sampled `double`
(the host reaches 0.52–1.96), and are correctly rounded at sampled `float`
values; `cbrt` is exact on exact cubes.

Angles above 2^32 radians are refused with NaN (owner, 2026-10-31: "the
most a person should need is 1080 degrees of rotation"; a long-running
phase accumulator should reset itself). No function in `math.hpp` returns
a C++ library result.

`math.hpp` holds declarations only; each family's kernel and table live in
`src/math/`, compiled with floating-point contraction off. That setting is
private to the library: consumers keep their own fused arithmetic, which is
desirable outside the replaced calls (owner, 2026-10-31). The cost is one
call per evaluation; link-time optimization may inline it back without
changing its bits, since contraction is recorded per instruction. `stx_auxlib_math_tests` checks a frozen output hash per family, and
`stx_auxlib_math_dekker_tests` checks that the no-FMA exact product gives
the same hashes. `STX_AUXLIB_BUILD_TOOLS=ON` builds the table generators
(`stx_auxlib_gen_<family>_table <output path>`), which regenerate their
tables in `src/math/` byte for byte.

## Text

`stx_auxlib::text` formats and parses numbers without format strings or
locales. `append_integer`, `append_real` and `append_text` append to a
caller's `std::string` under a typed record (`IntegerFormat`, `RealFormat`,
`Field`): `%02X` is `{.base = Base::hexadecimal, .minimum_digits = 2,
.letters = Letters::upper}`, and `%-14s` is `{.width = 14, .alignment =
Alignment::left}`. Reals use `std::to_chars`, whose shortest, fixed and
scientific forms the standard defines exactly, so output is identical on
every host; NaN is always `nan`. Widths, digit counts and precision are
capped, so no format can request an unbounded allocation.

`parse_signed`, `parse_unsigned`, `parse_double` and `parse_float` read a
whole string and return the value or a named failure (`empty`, `invalid`,
`out_of_range`, `trailing_text`); they accept a leading `+` and never skip
whitespace (`trim` does that). Where libraries disagree on out-of-range
input, the result is always `out_of_range`: a nonzero literal never parses
as zero or infinity. `TextScanner` replaces `sscanf` with field-by-field
reads that consume nothing when they fail.

Measured on the M4 under load (1 million values, `--bench`): shortest
`double` about 43 ns against 300 for `snprintf("%.17g")`; two fixed digits
about 37 ns against 106 for `snprintf("%.2f")` and 131 for
`std::to_string`; a signed integer about 17 ns against 37. Parsing is
slower than the host's: `parse_double` about 38 ns against 29 for
`strtod`. `stx_auxlib_text_tests` checks fixed and scientific output
against `snprintf` and shortest output by round trip over 300,000
pseudo-random values.

## Random

`ChaCha12Stream(seed, stream)` is the default generator: ChaCha with 12
rounds, keyed by a 256-bit `Seed`, with 2^64 independent streams per seed.
Its output cannot be predicted from earlier output. `Xoshiro256PlusPlus`
(with `jump` and `long_jump`) is the fast generator, by name, for hot
loops where predictability does not matter. `seed_from_number(n)` expands
a 64-bit number (a deal or level number) with SplitMix64;
`entropy_seed()` reads the host's entropy source (`getentropy`,
`BCryptGenRandom`) and fails closed with a named failure, never a weaker
seed.

The distributions are specified here, not by a standard library, since
libc++, libstdc++ and MSVC produce different numbers from the same engine
and seed: `uniform_below` (Lemire's unbiased method), `uniform_between`,
`uniform_unit` (53 random bits), `uniform_unit_float` (24 bits),
`uniform_real` (never returns its upper bound), `chance`, and `shuffle`
(Fisher–Yates from the last element down). A seed and a generator name
therefore fix a sequence on every host and toolchain; changing an
algorithm would break stored seeds, so a new one gets a new name.
Generators opt in to the distributions explicitly
(`is_stx_auxlib_generator`).

`tests/reference/random_reference.py` implements the algorithms
independently; its ChaCha core reproduces the RFC 8439 ChaCha20 block, and
`stx_auxlib_random_tests` checks the library's outputs against its values.

## Capabilities

Desktop-only services are named capabilities: `capability_available`
reports whether the host has one at all, and every operation a host lacks
returns `CapabilityStatus::unsupported` instead of failing its own way.

- `Child::start(request)` runs a program from an argument list with no
  shell, so arguments cannot inject commands (on Windows they are quoted
  for `CommandLineToArgvW`). A missing program is reported by `start`, not
  by a child that exits with 127. Children get the null device as input,
  output and error inherited, discarded or captured, default signal
  handling and an empty signal mask; `detached` starts its own session.
  `wait(timeout)` works the same on every host, `kill` ends a child, and a
  child dropped without waiting is reaped later rather than left a zombie.
- `open_with_desktop` accepts only `http`, `https` and `mailto` links and
  absolute paths, so input can never become an option or an arbitrary
  protocol handler; `reveal_in_file_manager` selects a file (Linux: opens
  its directory); `current_executable` returns the program's own path.
- `RawTerminal::enter()` holds the terminal in raw mode (on Windows, with
  virtual-terminal sequences both ways) and restores it when destroyed;
  `terminal_size()` reports columns and rows.
- `EventWatch::watch(events)` records interrupt, terminate, hangup, resize
  and suspend events for the program to `take()` when it chooses, so no
  program code runs in a signal handler; `EventDeferral` holds them back
  during a critical section; `ignore_broken_pipes()` makes closed-pipe
  writes report an error. Windows has interrupt, terminate and hangup only.

`stx_auxlib_capabilities_tests` starts itself as the child to check
arguments, output, exit status, timeouts, kills, detached children and
signal resets; it checks only what the desktop functions refuse, so it
never opens an application.

## Precedence chain

`precedence::Chain` proves that something was recorded before something
else, using only hash functions. The holder keeps a tiny secret, one
32-byte seed and the epoch number, in secure storage (`save` and
`restore` move it; `ChainState` erases its seed). `record(info)` commits an
info digest to the open epoch; `advance(now)` closes the epoch into a
`Certificate` and ratchets the seed forward, erasing the old one, so no one,
not even a later thief of the state, can sign for a past epoch again.
Anyone holding the chain's `Anchor` checks certificates with a `Verifier`
(`verify`), finds an info in one (`recorded_in`), orders two
(`precedes`, which also gives their epoch offset) and checks that two are
consecutive (`follows_directly`).

Each epoch has a WOTS+ one-time key (w = 16, 67 chains, BLAKE2s) derived
from its seed; it signs one leaf, which binds the epoch, its timestamp
(never allowed to go backwards), up to 16 info digests, the previous leaf,
a running digest of all completed blocks' summaries, and on a block's last
epoch the next block's key root. Blocks hold 256 epochs; a block's 256
public keys are Merkle-hashed into its key root, the anchor holds block
0's, and each block vouches for the next. Verification is one WOTS+ check
and an 8-hash path; a verifier learns each block's key root from that
block's predecessor's last certificate. On the M4 an epoch costs about
240 microseconds to advance and verify together.

What it does not do: it does not prove that time passed between epochs,
and whoever copies the secret state can sign epochs from that point on
(two validly signed, conflicting certificates for one block's end are
reported as invalid, since they show the state was copied). It orders
events only while the local state stays secret, needs no consensus, and
stores no ledger: certificates are outputs their holders keep.
`tests/reference/precedence_reference.py` implements the same bytes in
Python, and the tests check the C++ against it.

## Preference Ranked Voting

`voting::count_preference_ranked(candidates, ballots, options)` is PRV
1.0, the counting rule of the owner's Preference Ranked Voting design
(paymenottowork, `ideas/preference-ranked-voting/prv-core.mjs`): ballot
validation, expansion of incomplete ballots by inference from agreeing
donors (so exhausted ballots keep counting), elimination by positional
vectors with majority and final-pair rules, and seeded bracketing above 30
candidates. Ties it cannot break deterministically and perfect Condorcet
cycles are named failures; it never resolves by lot. Each distinct ballot
is counted once with its multiplicity, which is exactly equivalent (a
ballot is never its own donor, and copies infer alike), so the cost grows
with distinct rankings rather than voters: about two million voters with
400 distinct rankings count in 18 ms on the M4. `tests/voting_tests.cpp`
checks every outcome against the original JavaScript, run by
`tests/reference/prv_reference.mjs` on 420 deterministic elections.

## Ordered maps (stx_btree)

`btree/` brings in stx_btree, the C++ form of Xiangpeng Hao's bbtree
(`btree/README` is upstream's; `btree/PROVENANCE.md`): the single-threaded
and shared ordered maps (`stx_btree::unsync::BBTreeMap`,
`stx_btree::BBTreeMap`), the page allocators and the optimal search trees,
included as `"stx_btree/..."`. Three calls are routed through stx_auxlib
(the latch's yield, the pooled allocator's mutex and one fault) and are
marked "stx_auxlib adaptation" in the source; its own tests run in every
build mode.

## Environment, console and process

Environment variable names are limited to the portable set (a letter or
`_`, then letters, digits and `_`); values are UTF-8, bounded by a caller
limit, and read and written under one lock, since the host's `getenv` and
`setenv` race. Windows uses the wide Win32 block, which child processes
inherit, not the C runtime's ANSI copy.

`write_console` and `write_console_line` write whole UTF-8 text to standard
output or error, unbuffered and never interleaved within a call, after
flushing the C library's buffers so that output keeps its order with
remaining `printf` calls. On a Windows console the text goes through the
console API as UTF-16 and displays whatever the code page. `read_console_line`
returns lines without `\n` or `\r\n`, refuses lines over a caller limit
(skipping them) and invalid UTF-8. On POSIX hosts a write to a closed pipe
raises SIGPIPE unless the program calls `ignore_broken_pipes()` (events.hpp).

`fail(message)` writes one line, `fatal: <message> (<file>:<line>,
<function>)`, to standard error and aborts; on POSIX hosts it allocates
nothing. `ensure(condition, message)` calls it and, unlike `assert`, is
never compiled out. `exit_process(status)` takes a portable 0–255 status
and runs the exit tasks, which take a context pointer, newest first. The
console, environment and exit-task state is created on first use and never
destroyed, so it stays valid in exit tasks and static destructors.

## Files and paths

`stx_auxlib::Path` is UTF-8 with `/` separators, validated and normalized
when parsed: `.` vanishes, `..` folds lexically and may never climb above
an absolute root, and `\`, `:` (outside an `X:/` drive root), NUL and
invalid UTF-8 are refused, so a path means the same on every host. POSIX
hosts report drive paths unsupported; UNC names are not supported.

Every file operation returns a `FileStatus` (or a `FileValue<T>`, the value
or a status), mapped the same way on every host; none throws for a
filesystem condition. `read_file` and `list_directory` take a caller limit
and refuse larger content before allocating for it. `write_file` replaces a
file atomically and durably through a temporary file in the same directory
(`F_FULLFSYNC` on Apple hosts, `fsync` on other POSIX hosts, write-through
on Windows) and leaves the old file untouched on failure. `list_directory`
sorts entries by name bytes and counts, rather than lists, names that are
not valid Path segments. Modification times are `WallClock` (Unix time).
`File` is a move-only handle with explicit `close()` for callers that need
its result.

The Windows back end (`src/hosted/file_windows.cpp`, with `\\?\` long-path
names) compiles cleanly with MinGW GCC but has not run on Windows yet.

## Thread pool

`stx_auxlib::AtomicPool::create(threads)` returns a `PoolCreation` holding
the pool or its failure. `run(tasks)` blocks until a batch finishes;
`begin(tasks)` and `wait()` split it so the owner can request cancellation
in between. Batches of at most 64 tasks per thread, and every batch of a
one-thread pool, run on the caller in index order; larger batches and all
`begin` batches run on the workers. Tasks are borrowed until the batch
finishes and must not throw. See `include/stx_auxlib/thread_pool.hpp` for
the full contract.

`tests/thread_pool_conformance.cpp` is written against the C reference's
interface and runs against both the library and
`tests/reference/atomic_pool.c`; `--bench` prints a timing table for
comparing the two.
