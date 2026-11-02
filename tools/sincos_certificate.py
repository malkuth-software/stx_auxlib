#!/usr/bin/env python3
"""Error certificate for double sin, cos and tan (src/math/sincos_core.hpp).

Proves, for every double x with 2^-27 <= |x| <= 2^32, a bound on the error of
stx_auxlib::math::sincos(double) and tan(double), in four steps:

  1. Worst-case remainders by construction. For every binade of doubles and
     every residue class that matters (all q; q = 0 mod 128, where sin x is
     small; q = 64 mod 128, where cos x is small and tan has its poles), the
     exact minimum of |x - q pi/128| over the binade's doubles, from the
     search over every q of the binade in tools/sincos_minimum_remainder.cpp
     (baby and giant steps over an ordered stx_btree index, complete over
     the range), with fixed-point error accounted and the answer checked
     exactly here. pi is computed here, with a certified error.
  2. The core's proven error (sincos_rotate; tan_rotate and the split
     division in tan.cpp), by interval error analysis over the table index
     and the reduced argument, every rounding and every truncation counted. From the stated
     ulp budget it follows how much error the core can accept in r, as a
     function of |r| (the reduction contract).
  3. The reduction's absolute error bound per binade, from the exact stored
     parts of pi/128 and every rounding step, with exactness established by
     bit counts and granularity, never assumed.
  4. The composition: each path's error against the contract at the
     certified minimum remainder of every binade it serves, which fixes the
     small-remainder threshold (sincos_small_remainder_limit).

Usage (--search names the built tools/sincos_minimum_remainder.cpp,
target stx_auxlib_sincos_minimum_remainder with STX_AUXLIB_BUILD_TOOLS=ON):
  sincos_certificate.py --search PROGRAM
                                      print the certificate; exit 1 if any
                                      step fails
  sincos_certificate.py --write PATH  also write the worst-case table
                                      (tests/reference/sincos_worst_cases.hpp)
  sincos_certificate.py --check PATH  fail unless PATH is what --write makes

Only the Python standard library: integers and fractions, no floating-point
arithmetic in any bound (floats appear only as exact stored constants and in
printed summaries).
"""

import math
import pathlib
import re
import sys
from fractions import Fraction

# The repository root, two levels above this file (any separator, so the
# script runs as given on Windows too).
ROOT = pathlib.Path(__file__).resolve().parent.parent
TABLE_PATH = ROOT / 'src' / 'math' / 'sincos_table256.hpp'
CORE_PATH = ROOT / 'src' / 'math' / 'sincos_core.hpp'
SUPPORT_PATH = ROOT / 'src' / 'math' / 'kernel_support.hpp'

U = Fraction(1, 2 ** 53)          # unit roundoff, round to nearest
# The stated bound for double sin, cos and tan, in ulps of the exact
# result (provisional choice of the coordinator, 2026-11-02; the owner may
# change it). math.hpp, README.md and tests/math_tests.cpp state the same.
STATED_BOUND_ULPS = Fraction(51, 100)
# What the final rounding leaves for the core and the reduction together.
BUDGET_ULP = STATED_BOUND_ULPS - Fraction(1, 2)

failures = []


def require(condition, message):
    if not condition:
        failures.append(message)
        print('FAIL: ' + message)


def log2(value):
    value = abs(Fraction(value))
    if value == 0:
        return float('-inf')
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    scaled = value / Fraction(2) ** exponent
    return exponent + math.log2(float(scaled))


# ---- pi, certified ------------------------------------------------------

PI_BITS = 720


def atan_inverse(n, bits):
    """atan(1/n) 2^bits as an integer and its error bound in units.

    power_k = floor(2^bits / n^(2k+1)) exactly (floor of a floor), each term
    floor(power_k / (2k+1)) is within 2 of the true term, and the
    alternating tail is below 1 once power_k reaches 0.
    """
    power = (1 << bits) // n
    n_squared = n * n
    total = 0
    k = 0
    while power:
        term = power // (2 * k + 1)
        total += term if k % 2 == 0 else -term
        power //= n_squared
        k += 1
    return total, 2 * k + 1


def pi_interval():
    bits = PI_BITS + 16
    a5, e5 = atan_inverse(5, bits)
    a239, e239 = atan_inverse(239, bits)
    machin = 16 * a5 - 4 * a239          # Machin
    machin_error = 16 * e5 + 4 * e239
    a18, e18 = atan_inverse(18, bits)
    a57, e57 = atan_inverse(57, bits)
    gauss = 48 * a18 + 32 * a57 - 20 * a239  # Gauss
    gauss_error = 48 * e18 + 32 * e57 + 20 * e239
    scale = Fraction(1, 1 << bits)
    low = max(machin - machin_error, gauss - gauss_error) * scale
    high = min(machin + machin_error, gauss + gauss_error) * scale
    require(low < high, 'Machin and Gauss formulas for pi disagree')
    return low, high


PI_LOW, PI_HIGH = pi_interval()
STEP_LOW = PI_LOW / 128             # pi/128
STEP_HIGH = PI_HIGH / 128


# ---- stored constants ----------------------------------------------------

def exact(text):
    return Fraction(float.fromhex(text))


def read_constants():
    table_text = open(TABLE_PATH).read()
    core_text = open(CORE_PATH).read()
    entries = re.findall(r'\{(-?0x[0-9a-fp.+-]+), (-?0x[0-9a-fp.+-]+), (-?0x[0-9a-fp.+-]+), '
                         r'(-?0x[0-9a-fp.+-]+)\}', table_text)
    require(len(entries) == 256, 'sincos_table256.hpp: expected 256 entries')
    table = [tuple(exact(value) for value in entry) for entry in entries]

    def named(name, text):
        match = re.search(r'inline constexpr double ' + name + r' = (-?0x[0-9a-fp.+-]+);', text)
        require(match is not None, 'constant ' + name + ' not found')
        return exact(match.group(1))

    fast_parts = [named('sincos_step_' + str(i), table_text) for i in (1, 2, 3)]
    medium_block = re.search(r'sincos_medium_step\[8\] = \{([^}]*)\}', table_text).group(1)
    medium_parts = [exact(value) for value in re.findall(r'-?0x[0-9a-fp.+-]+', medium_block)]
    require(len(medium_parts) == 8, 'expected 8 medium parts')
    medium_rest = named('sincos_medium_step_rest', table_text)
    inverse_step = named('sincos_inverse_step', core_text)
    # The polynomial coefficients, as sincos_core.hpp spells them: the
    # nearest doubles to -1/6, 1/120, -1/5040, 1/24, -1/720 (IEEE division
    # rounds to nearest in C++ constant evaluation and in Python alike).
    for spelling in ('sin_3 = -1.0 / 6.0', 'sin_5 = 1.0 / 120.0', 'sin_7 = -1.0 / 5040.0',
                     'cos_half = 0.5', 'cos_4 = 1.0 / 24.0', 'cos_6 = -1.0 / 720.0',
                     'tan_3 = 1.0 / 3.0', 'tan_5 = 2.0 / 15.0', 'tan_7 = 17.0 / 315.0',
                     'tan_9 = 62.0 / 2835.0', 'tan_11 = 1382.0 / 155925.0'):
        require(spelling in core_text, 'sincos_core.hpp no longer spells ' + spelling)
    cut_spelling = 'cut_26_mask = ~((std::uint64_t{1} << 27) - 1)'
    require(cut_spelling in open(SUPPORT_PATH).read(), 'kernel_support.hpp no longer spells ' + cut_spelling)
    coefficients = {
        'sin_3': Fraction(-1.0 / 6.0), 'sin_5': Fraction(1.0 / 120.0),
        'sin_7': Fraction(-1.0 / 5040.0), 'cos_4': Fraction(1.0 / 24.0),
        'cos_6': Fraction(-1.0 / 720.0),
        'tan_3': Fraction(1.0 / 3.0), 'tan_5': Fraction(2.0 / 15.0), 'tan_7': Fraction(17.0 / 315.0),
        'tan_9': Fraction(62.0 / 2835.0), 'tan_11': Fraction(1382.0 / 155925.0),
    }
    return table, fast_parts, medium_parts, medium_rest, inverse_step, coefficients


TABLE, FAST_PARTS, MEDIUM_PARTS, MEDIUM_REST, INVERSE_STEP, COEFFICIENTS = read_constants()


def significant_bits(value):
    """Bits in the significand of a nonzero dyadic rational, and its lowest bit's weight."""
    value = abs(Fraction(value))
    numerator = value.numerator
    denominator = value.denominator
    require(denominator & (denominator - 1) == 0, 'not a dyadic constant')
    trailing = (numerator & -numerator).bit_length() - 1
    odd = numerator >> trailing
    return odd.bit_length(), Fraction(1 << trailing, denominator)


def magnitude_interval(low, high):
    return max(abs(low), abs(high))


# ---- high-precision sin and cos ----------------------------------------

FUNCTION_BITS = 300


def outward(low, high, bits=FUNCTION_BITS):
    scale = 1 << bits
    low_scaled = math.floor(low * scale)
    high_scaled = math.ceil(high * scale)
    return Fraction(low_scaled, scale), Fraction(high_scaled, scale)


def sin_cos_small(t_low, t_high):
    """Intervals for sin t and cos t, |t| <= 1: Taylor to a term below 2^-310,
    the alternating tails bounded by the next term; sin and cos are
    1-Lipschitz, so the interval of t widens each by (t_high - t_low)."""
    t = (t_low + t_high) / 2
    width = (t_high - t_low) / 2
    require(abs(t) <= 1, 'sin_cos_small: |t| > 1')
    sin_sum = Fraction(0)
    cos_sum = Fraction(0)
    term = Fraction(1)   # t^n / n!
    n = 0
    bound = Fraction(1, 1 << (FUNCTION_BITS + 10))
    while abs(term) > bound or n < 4:
        if n % 4 == 0:
            cos_sum += term
        elif n % 4 == 1:
            sin_sum += term
        elif n % 4 == 2:
            cos_sum -= term
        else:
            sin_sum -= term
        n += 1
        term = term * t / n
        term_low, term_high = outward(term, term, FUNCTION_BITS + 20)
        term = term_high if term > 0 else term_low
    tail = 2 * abs(term) + bound
    sin_bounds = outward(sin_sum - tail - width, sin_sum + tail + width)
    cos_bounds = outward(cos_sum - tail - width, cos_sum + tail + width)
    return sin_bounds, cos_bounds


def sin_cos_of_step_multiple(k):
    """Intervals for sin and cos of k pi/128, k in 0..255, by symmetry to |j| <= 32."""
    quadrant = k // 64
    j = k % 64
    if j > 32:
        j -= 64
        quadrant += 1
    t_low = min(j * STEP_LOW, j * STEP_HIGH)
    t_high = max(j * STEP_LOW, j * STEP_HIGH)
    (s_low, s_high), (c_low, c_high) = sin_cos_small(t_low, t_high)
    quadrant &= 3
    if quadrant == 0:
        result = ((s_low, s_high), (c_low, c_high))
    elif quadrant == 1:
        result = ((c_low, c_high), (-s_high, -s_low))
    elif quadrant == 2:
        result = ((-s_high, -s_low), (-c_high, -c_low))
    else:
        result = ((-c_high, -c_low), (s_low, s_high))
    if j == 0:
        # The exact zeros: sin and cos of multiples of pi/2.
        zero = (Fraction(0), Fraction(0))
        one = (Fraction(1), Fraction(1))
        minus_one = (Fraction(-1), Fraction(-1))
        exact_values = {0: (zero, one), 1: (one, zero), 2: (zero, minus_one), 3: (minus_one, zero)}
        result = exact_values[quadrant]
    return result


def sin_cos_of_double(x):
    """Intervals for sin x and cos x for a double x, |x| <= 2^33, through x - M pi/2."""
    x = Fraction(x)
    half_pi_mid = (PI_LOW + PI_HIGH) / 4
    quadrant_count = round(x / half_pi_mid)
    t_candidates = (x - quadrant_count * PI_LOW / 2, x - quadrant_count * PI_HIGH / 2)
    t_low = min(t_candidates)
    t_high = max(t_candidates)
    (s_low, s_high), (c_low, c_high) = sin_cos_small(t_low, t_high)
    quadrant = quadrant_count % 4
    if quadrant == 0:
        result = ((s_low, s_high), (c_low, c_high))
    elif quadrant == 1:
        result = ((c_low, c_high), (-s_high, -s_low))
    elif quadrant == 2:
        result = ((-s_high, -s_low), (-c_high, -c_low))
    else:
        result = ((-c_high, -c_low), (s_low, s_high))
    return result


# ---- step 1: worst-case remainders --------------------------------------

FIXED_BITS = 128
FIXED_ONE = 1 << FIXED_BITS


def fixed_fraction(value_low, value_high):
    """frac(v) in units of 2^-128, truncated, for v known in [low, high],
    and the bound on |that - frac(v) 2^128| in units."""
    middle = (value_low + value_high) / 2
    fraction = middle - math.floor(middle)
    units = math.floor(fraction * FIXED_ONE)
    error = 1 + math.ceil((value_high - value_low) / 2 * FIXED_ONE)
    return units % FIXED_ONE, error


def search_job(theta_low, theta_high, gamma_low, gamma_high, j_low, j_high):
    f, f_error = fixed_fraction(theta_low, theta_high)
    g, g_error = fixed_fraction(gamma_low, gamma_high)
    # (j F + G) mod 2^128 against frac(j Theta + gamma) 2^128.
    error_units = j_high * f_error + g_error
    line = '%d %d %032x %032x' % (j_low, j_high, f, g)
    return line, error_units


SEARCH_PROGRAM = None


def run_searches(lines):
    """One run of tools/sincos_minimum_remainder.cpp over every job."""
    import subprocess
    require(SEARCH_PROGRAM is not None, 'no --search program given')
    completed = subprocess.run([SEARCH_PROGRAM], input='\n'.join(lines) + '\n', capture_output=True,
                               text=True)
    require(completed.returncode == 0, 'search program failed: ' + completed.stderr.strip())
    results = []
    for output in completed.stdout.split('\n'):
        if output:
            j_text, distance_text = output.split()
            results.append((int(j_text), int(distance_text, 16)))
    require(len(results) == len(lines), 'search program returned %d of %d results' % (len(results), len(lines)))
    return results


def j_range(theta_low, theta_high, gamma_low, gamma_high, n_low, n_high, j_first):
    """j >= j_first whose nearest integer n to j Theta + gamma lies in [n_low, n_high):
    n_low - 1/2 < j Theta + gamma < n_high - 1/2 (Theta > 1, so j to n is increasing)."""
    require(theta_low > 1, 'Theta <= 1')
    j_low = max(j_first, math.floor((n_low - Fraction(1, 2) - gamma_low) / theta_low) - 2)
    while j_low * theta_high + gamma_high <= n_low - Fraction(1, 2):
        j_low += 1
    require(j_low * theta_low + gamma_low > n_low - Fraction(1, 2) or j_low == j_first,
            'j range: lower end uncertain')
    j_high = math.ceil((n_high - Fraction(1, 2) - gamma_low) / theta_low) + 2
    while j_high * theta_low + gamma_low >= n_high - Fraction(1, 2):
        j_high -= 1
    require(j_high * theta_high + gamma_high < n_high - Fraction(1, 2), 'j range: upper end uncertain')
    return j_low, j_high


# The residue classes of q: (modulus, residue, name).
CLASSES = ((1, 0, 'any q'), (128, 0, 'q = 0 mod 128 (sin small)'), (128, 64, 'q = 64 mod 128 (cos small)'))

FAST_LIMIT = Fraction(2) ** 20
MEDIUM_LIMIT = Fraction(2) ** 32
TINY_LIMIT = Fraction(1, 2 ** 27)


class Binade:
    """Doubles x = n 2^s with n in [n_low, n_high), and the path that serves them."""

    def __init__(self, s, n_low, n_high, path):
        self.s = s
        self.n_low = n_low
        self.n_high = n_high
        self.path = path
        self.x_low = Fraction(n_low) * Fraction(2) ** s
        self.x_high = Fraction(n_high - 1) * Fraction(2) ** s

    def name(self):
        if self.n_high - self.n_low == 1:
            return 'x = %s' % float(self.x_low).hex()
        return '[2^%d, 2^%d)' % (52 + self.s, 53 + self.s)


def binades():
    result = []
    for s in range(-79, -32):          # 2^-27 <= x < 2^20
        result.append(Binade(s, 1 << 52, 1 << 53, 'fast'))
    result.append(Binade(-32, 1 << 52, (1 << 52) + 1, 'fast'))       # x = 2^20
    result.append(Binade(-32, (1 << 52) + 1, 1 << 53, 'medium'))      # 2^20 < x < 2^21
    for s in range(-31, -20):
        result.append(Binade(s, 1 << 52, 1 << 53, 'medium'))
    result.append(Binade(-20, 1 << 52, (1 << 52) + 1, 'medium'))     # x = 2^32
    return result


def remainder_jobs(binade):
    """The search jobs for one binade: per class, None or (job line, error
    units, modulus, residue, theta interval, gamma interval)."""
    scale = Fraction(2) ** binade.s
    theta_low = STEP_LOW / scale
    theta_high = STEP_HIGH / scale
    jobs = {}
    for modulus, residue, name in CLASSES:
        big_low = modulus * theta_low
        big_high = modulus * theta_high
        gamma_low = residue * theta_low
        gamma_high = residue * theta_high
        j_first = 0 if residue else 1
        j_low, j_high = j_range(big_low, big_high, gamma_low, gamma_high, binade.n_low, binade.n_high, j_first)
        if j_high < j_low:
            jobs[name] = None
            continue
        line, error_units = search_job(big_low, big_high, gamma_low, gamma_high, j_low, j_high)
        jobs[name] = (line, error_units, modulus, residue, (big_low, big_high), (gamma_low, gamma_high))
    return jobs


def worst_remainders(all_binades):
    """Per binade and class: (certified lower bound on |x - q pi/128| over the
    binade's doubles with q != 0 in the class, the x attaining the search's
    minimum, its q), or None when no such q reaches the binade.

    The search is exact on its fixed-point values, each within error_units
    of the true frac(j Theta + gamma) 2^128, so the true minimum over the
    range is at least the found distance minus error_units; the attained x
    is then checked exactly."""
    job_table = {}
    lines = []
    for binade in all_binades:
        job_table[binade] = remainder_jobs(binade)
        for name, job in job_table[binade].items():
            if job is not None:
                lines.append(job[0])
    results = run_searches(lines)
    position = 0
    worst = {}
    scale_of = {}
    for binade in all_binades:
        scale = Fraction(2) ** binade.s
        worst[binade] = {}
        for name, job in job_table[binade].items():
            if job is None:
                worst[binade][name] = None
                continue
            line, error_units, modulus, residue, theta, gamma = job
            j, distance_units = results[position]
            position += 1
            lower = Fraction(distance_units - error_units, FIXED_ONE)
            require(lower > 0, 'minimum remainder not separated from zero in ' + binade.name())
            values = (j * theta[0] + gamma[0], j * theta[1] + gamma[1])
            n = round((values[0] + values[1]) / 2)
            attained_low = min(abs(values[0] - n), abs(values[1] - n))
            require(attained_low >= lower, 'search answer inconsistent in ' + binade.name())
            require(binade.n_low <= n < binade.n_high, 'search answer outside ' + binade.name())
            q = modulus * j + residue
            worst[binade][name] = (lower * scale, n * scale, q)
    return worst


# ---- step 3: the reduction's absolute error ----------------------------

def q_bound(binade):
    """max |q| and max |x - q pi/128| over the binade. q is the integer
    nearest fl(x * inverse_step) (the shift trick rounds to nearest for
    |value| < 2^51), so |x/step - q| <= 1/2 + |fl(x inv) - x/step|."""
    x_high = binade.x_high
    q_max = math.floor(x_high * INVERSE_STEP * (1 + U) + Fraction(1, 2))
    inverse_error = max(abs(INVERSE_STEP - 1 / STEP_LOW), abs(INVERSE_STEP - 1 / STEP_HIGH))
    eta = x_high * (inverse_error + U * INVERSE_STEP)
    remainder_max = STEP_HIGH * (Fraction(1, 2) + eta)
    return q_max, remainder_max


def product_exact(q_max, part):
    bits, _ = significant_bits(part)
    return q_max.bit_length() + bits <= 53


def fits_exactly(magnitude, granule):
    """A value that is a multiple of `granule` with |value| <= magnitude is a double."""
    return magnitude < (1 << 53) * granule


def tail_bound(parts):
    total = sum(parts, Fraction(0))
    return max(abs(STEP_LOW - total), abs(STEP_HIGH - total))


def fast_reduction_error(binade, rho=None):
    """sincos_remainder_wide's |(r + r_lo) - (x - q pi/128)| bound over the
    binade's inputs with |x - q pi/128| <= rho (default: every input); 0
    when the binade has q = 0 only (then r = x, exactly). Affine in rho
    wherever the exactness tests come out the same."""
    q_max, remainder_max = q_bound(binade)
    if rho is not None:
        remainder_max = min(remainder_max, rho)
    if q_max == 0:
        return Fraction(0)
    step_1, step_2, step_3 = FAST_PARTS
    require(product_exact(q_max, step_1) and product_exact(q_max, step_2),
            'fast reduction: q * step_1 or q * step_2 not exact in ' + binade.name())
    granule = min(Fraction(2) ** binade.s, significant_bits(step_1)[1])
    r1_max = remainder_max + q_max * tail_bound([step_1])
    require(fits_exactly(r1_max, granule), 'fast reduction: x - q step_1 not exact in ' + binade.name())
    # two_sum(r1, -q step_2): r2 + r2_error = r1 - q step_2 exactly.
    r2_exact_max = remainder_max + q_max * tail_bound([step_1, step_2])
    granule_2 = min(granule, significant_bits(step_2)[1])
    r2_error_max = Fraction(0) if fits_exactly(r2_exact_max, granule_2) else U * r2_exact_max
    product_3 = q_max * abs(step_3)
    rounding_product_3 = U * product_3
    low_max = r2_error_max + product_3 * (1 + U)
    rounding_low = U * low_max
    truncation = q_max * tail_bound([step_1, step_2, step_3])
    return truncation + rounding_product_3 + rounding_low


def medium_reduction_error(binade, rho=None):
    """sincos_remainder_medium's |(r + r_lo) - (x - q pi/128)| bound over the
    binade's inputs with |x - q pi/128| <= rho (default: every input)."""
    q_max, remainder_max = q_bound(binade)
    if rho is not None:
        remainder_max = min(remainder_max, rho)
    if q_max == 0:
        return Fraction(0)
    parts = MEDIUM_PARTS
    for part in parts:
        require(product_exact(q_max, part), 'medium reduction: q * part not exact in ' + binade.name())
    granule = min(Fraction(2) ** binade.s, significant_bits(parts[0])[1])
    head_max = remainder_max + q_max * tail_bound(parts[:1])
    require(fits_exactly(head_max, granule), 'medium reduction: x - q part_0 not exact in ' + binade.name())
    accumulated = Fraction(0)     # bound on the sum of two_sum errors so far
    roundings = Fraction(0)       # bound on the roundings of `low`
    low_max = Fraction(0)
    exact_so_far = True
    for index in range(1, 8):
        granule = min(granule, significant_bits(parts[index])[1])
        value_max = remainder_max + q_max * tail_bound(parts[:index + 1]) + accumulated
        if exact_so_far and fits_exactly(value_max, granule):
            error_max = Fraction(0)
        else:
            exact_so_far = False
            error_max = U * value_max
        accumulated += error_max
        if error_max:
            low_max = low_max + error_max
            roundings += U * low_max
    rest_product = q_max * abs(MEDIUM_REST)
    roundings += U * rest_product
    final_low = low_max + rest_product * (1 + U)
    roundings += U * final_low
    truncation = q_max * tail_bound(parts + [MEDIUM_REST])
    return truncation + roundings


# ---- step 2: the core's proven error -----------------------------------

class Value:
    """A computed double: an interval holding it, and a bound on its distance
    from the same expression evaluated exactly on the same inputs."""

    def __init__(self, low, high, error=Fraction(0)):
        self.low = low
        self.high = high
        self.error = error

    def magnitude(self):
        return magnitude_interval(self.low, self.high)


def constant(value):
    return Value(value, value)


def half_ulp(magnitude):
    """A bound on |fl(v) - v| for every |v| <= magnitude (normal range):
    ulp(v)/2 <= 2^(e - 53) with 2^e <= magnitude < 2^(e+1)."""
    if magnitude == 0:
        return Fraction(0)
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if Fraction(2) ** exponent > magnitude:
        exponent -= 1
    return Fraction(2) ** (exponent - 53)


def ulp_floor(lower):
    """ulp(y) for every |y| >= lower > 0: at least 2^(e - 52), 2^e <= lower."""
    return 2 * half_ulp(lower)


def rounded(low, high, error):
    magnitude = magnitude_interval(low, high)
    widen = half_ulp(magnitude)
    return Value(low - widen, high + widen, error + widen)


def add(a, b):
    return rounded(a.low + b.low, a.high + b.high, a.error + b.error)


def subtract(a, b):
    return rounded(a.low - b.high, a.high - b.low, a.error + b.error)


def multiply(a, b):
    products = (a.low * b.low, a.low * b.high, a.high * b.low, a.high * b.high)
    error = a.magnitude() * b.error + b.magnitude() * a.error + a.error * b.error
    return rounded(min(products), max(products), error)


def cut_26(value):
    """sincos_core.hpp's cut_26: the double's leading 26 significant bits,
    toward zero."""
    if value == 0:
        return Fraction(0)
    magnitude = abs(value)
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if Fraction(2) ** exponent > magnitude:
        exponent -= 1
    quantum = Fraction(2) ** (exponent - 25)
    kept = math.floor(magnitude / quantum) * quantum
    return kept if value > 0 else -kept


def nearest_double(value):
    """value rounded to the nearest double (exact: Python's float() of a
    Fraction rounds correctly)."""
    if value == 0:
        return Fraction(0)
    return Fraction(float(value))


def table_cut(head, low):
    """make_sincos_cuts: h1 = cut_26(head) and h2 = fl((head - h1) + low), h2
    as a Value whose error is its distance from (head - h1) + low."""
    h1 = cut_26(head)
    exact_rest = (head - h1) + low
    h2 = nearest_double(exact_rest)
    return h1, Value(h2, h2, abs(h2 - exact_rest))


def cut_parts(r_low, r_high):
    """r1 = cut_26(r) and r2 = r - r1 (exact): |r1| <= |r| with r's sign, and
    |r2| < 2^-25 |r|. Returns r1's magnitude bound and r2 as a Value."""
    m = magnitude_interval(r_low, r_high)
    d = m * Fraction(1, 2 ** 25)
    return m, Value(-d, d)


def reduced_polynomials(r_low, r_high, r_lo_max):
    """r^2, sin r - r and cos r - 1 as sincos_rotate forms them (Values), with
    their model errors and bounds on the true values."""
    r_magnitude = magnitude_interval(r_low, r_high)
    r_lo_value = Value(-r_lo_max, r_lo_max)
    r_value = Value(r_low, r_high)
    squares = (r_low * r_low, r_high * r_high)
    square_low = Fraction(0) if r_low <= 0 <= r_high else min(squares)
    r2 = rounded(square_low, max(squares), Fraction(0))
    co = COEFFICIENTS
    # sin_r_minus_r = r_lo + r * (r2 * (sin_3 + r2 * (sin_5 + r2 * sin_7)))
    t = multiply(r2, constant(co['sin_7']))
    t = add(constant(co['sin_5']), t)
    t = multiply(r2, t)
    t = add(constant(co['sin_3']), t)
    t = multiply(r2, t)
    t = multiply(r_value, t)
    sp = add(r_lo_value, t)
    # cos_r_minus_1 = (r2 * r2) * (cos_4 + r2 * cos_6) - r2 * cos_half
    a = multiply(r2, r2)
    b = multiply(r2, constant(co['cos_6']))
    b = add(constant(co['cos_4']), b)
    a = multiply(a, b)
    half = Value(r2.low / 2, r2.high / 2, r2.error / 2)   # exact: a power of two
    cp = subtract(a, half)
    # Model errors: the exact polynomials against sin(r + r_lo) - r and
    # cos(r + r_lo) - 1 (Taylor remainders, coefficient roundings, and the
    # cross terms of r_lo).
    m = r_magnitude
    rho = r_lo_max
    model_sp = (rho * m * m / 2 + m * rho * rho / 2 + rho ** 3 / 6 +
                m ** 3 * abs(Fraction(-1, 6) - co['sin_3']) + m ** 5 * abs(Fraction(1, 120) - co['sin_5']) +
                m ** 7 * abs(Fraction(-1, 5040) - co['sin_7']) + m ** 9 / math.factorial(9))
    model_cp = (rho * m + rho * rho / 2 + m ** 4 * abs(Fraction(1, 24) - co['cos_4']) +
                m ** 6 * abs(Fraction(-1, 720) - co['cos_6']) + m ** 8 / math.factorial(8))
    sp_true_max = rho + (m + rho) ** 3 / 6
    cp_true_max = (m + rho) ** 2 / 2
    return r_value, r2, sp, cp, model_sp, model_cp, sp_true_max, cp_true_max


def table_sigmas(k):
    """|S + S_lo - sin(k pi/128)| and the same for cos, bounded."""
    s, c, s_lo, c_lo = TABLE[k]
    (sin_low, sin_high), (cos_low, cos_high) = sin_cos_of_step_multiple(k)
    sigma_s = magnitude_interval(sin_low - s - s_lo, sin_high - s - s_lo)
    sigma_c = magnitude_interval(cos_low - c - c_lo, cos_high - c - c_lo)
    return sigma_s, sigma_c


def core_errors(k, r_low, r_high, r_lo_max, parts=None):
    """Absolute error bounds of the core's sin and cos double-doubles for
    table index k and r in [r_low, r_high] (the remainder's head, a double),
    |r_lo| <= r_lo_max, against sin and cos of k pi/128 + r + r_lo. The
    rotation is the owner's 26-bit split: c1 r1 and s1 r1 are exact and join
    S and C by fast two-sums; the cross terms c1 r2 + c2 r and s1 r2 + s2 r
    are rounded, and each of their roundings is counted."""
    s, c, s_lo, c_lo = TABLE[k]
    sigma_s, sigma_c = table_sigmas(k)
    r_value, _, sp, cp, model_sp, model_cp, sp_true_max, cp_true_max = reduced_polynomials(
        r_low, r_high, r_lo_max)
    m, r2_cut = cut_parts(r_low, r_high)
    s1, s2 = table_cut(s, s_lo)
    c1, c2 = table_cut(c, c_lo)

    # sin: fast_two_sum(S, c1 r1) needs |S| >= |c1 r1| unless S = 0. The
    # exact expression evaluated is S + C r + C_lo r + C sp + S cp + S_lo.
    rotation = abs(c1) * m
    require(s == 0 or abs(s) >= rotation, 'fast_two_sum precondition (sin) at k = %d' % k)
    head_error = half_ulp(abs(s) + rotation)
    sin_head_error = Value(-head_error, head_error)
    cross = add(multiply(constant(c1), r2_cut), multiply(c2, r_value))
    small = add(multiply(constant(c), sp), multiply(constant(s), cp))
    inner = add(cross, small)
    tail = add(add(sin_head_error, inner), constant(s_lo))
    neglected = (sigma_s + sigma_c * m + (abs(s_lo) + sigma_s) * cp_true_max +
                 (abs(c_lo) + sigma_c) * sp_true_max)
    sin_error = tail.error + abs(c) * model_sp + abs(s) * model_cp + neglected
    if parts is not None:
        parts['sin'] = [('roundings in r2 and sin_r_minus_r', sp.error),
                        ('roundings in cos_r_minus_1', cp.error),
                        ('cross terms c1 r2 + c2 r, cumulative', cross.error),
                        ('sin_small: its products and sums, cumulative', small.error),
                        ('inner sum (cross + small), cumulative', inner.error),
                        ('tail sums (head error, inner, S_lo), cumulative', tail.error),
                        ('|C| * polynomial model error of sin r - r', abs(c) * model_sp),
                        ('|S| * polynomial model error of cos r - 1', abs(s) * model_cp),
                        ('table errors sigma and their products (neglected terms)', neglected)]

    # cos: fast_two_sum(C, -s1 r1); the exact expression is C - S r - S_lo r
    # + C cp - S sp + C_lo.
    rotation = abs(s1) * m
    require(c == 0 or abs(c) >= rotation, 'fast_two_sum precondition (cos) at k = %d' % k)
    head_error = half_ulp(abs(c) + rotation)
    cos_head_error = Value(-head_error, head_error)
    cross = add(multiply(constant(s1), r2_cut), multiply(s2, r_value))
    small = subtract(multiply(constant(c), cp), multiply(constant(s), sp))
    inner = subtract(small, cross)
    tail = add(add(cos_head_error, inner), constant(c_lo))
    neglected = (sigma_c + sigma_s * m + (abs(c_lo) + sigma_c) * cp_true_max +
                 (abs(s_lo) + sigma_s) * sp_true_max)
    cos_error = tail.error + abs(c) * model_cp + abs(s) * model_sp + neglected
    if parts is not None:
        parts['cos'] = [('roundings in r2 and sin_r_minus_r', sp.error),
                        ('roundings in cos_r_minus_1', cp.error),
                        ('cross terms s1 r2 + s2 r, cumulative', cross.error),
                        ('cos_small: its products and sums, cumulative', small.error),
                        ('inner difference (small - cross), cumulative', inner.error),
                        ('tail sums (head error, inner, C_lo), cumulative', tail.error),
                        ('|C| * polynomial model error of cos r - 1', abs(c) * model_cp),
                        ('|S| * polynomial model error of sin r - r', abs(s) * model_sp),
                        ('table errors sigma and their products (neglected terms)', neglected)]
    return sin_error, cos_error


TAN_TRUE = (Fraction(1, 3), Fraction(2, 15), Fraction(17, 315), Fraction(62, 2835), Fraction(1382, 155925))
TAN_NAMES = ('tan_3', 'tan_5', 'tan_7', 'tan_9', 'tan_11')
TAN_NEXT = Fraction(21844, 6081075)  # the r^13 coefficient of tan r


def tan_errors(k, r_low, r_high, r_lo_max):
    """Absolute error bounds of tan_rotate's numerator S + C t and denominator
    C - S t (t = tan r), each normalized, against (sin x, cos x) / cos(r +
    r_lo) with x = k pi/128 + r + r_lo."""
    s, c, s_lo, c_lo = TABLE[k]
    sigma_s, sigma_c = table_sigmas(k)
    r_value, r2, _, _, _, _, _, _ = reduced_polynomials(r_low, r_high, r_lo_max)
    rho = r_lo_max
    co = COEFFICIENTS
    # t_minus_r = r * (r2 * (tan_3 + r2 * (tan_5 + ... + r2 * tan_11))) + r_lo * (1 + r2)
    t = multiply(r2, constant(co['tan_11']))
    for name in ('tan_9', 'tan_7', 'tan_5'):
        t = add(constant(co[name]), t)
        t = multiply(r2, t)
    t = add(constant(co['tan_3']), t)
    t = multiply(r2, t)
    t = multiply(r_value, t)
    one_plus = add(constant(Fraction(1)), r2)
    t_minus_r = add(t, multiply(Value(-rho, rho), one_plus))
    m, r2_cut = cut_parts(r_low, r_high)
    # Model: tan(r + r_lo) - r against the exact polynomial: coefficient
    # roundings, the series' truncation (the r^13 term and the rest, at most
    # 10% more for |r| < 2^-6), r_lo (sec^2 r - 1 - r^2) <= r_lo r^4, and the
    # second-order term in r_lo.
    model_t = (sum(abs(TAN_TRUE[i] - co[TAN_NAMES[i]]) * m ** (2 * i + 3) for i in range(5)) +
               TAN_NEXT * m ** 13 * Fraction(11, 10) + rho * m ** 4 + 2 * rho * rho * (m + rho))
    t_true_max = rho * (1 + (m + rho) ** 2) + (m + rho) ** 3 / 3 * Fraction(11, 10)
    s1, s2 = table_cut(s, s_lo)
    c1, c2 = table_cut(c, c_lo)
    # n = S + c1 r1 (fast two-sum) + [(c1 r2 + c2 r) + C t_minus_r] + S_lo
    rotation = abs(c1) * m
    require(s == 0 or abs(s) >= rotation, 'fast_two_sum precondition (tan numerator) at k = %d' % k)
    head_error = half_ulp(abs(s) + rotation)
    cross = add(multiply(constant(c1), r2_cut), multiply(c2, r_value))
    n_tail = add(add(Value(-head_error, head_error), add(cross, multiply(constant(c), t_minus_r))),
                 constant(s_lo))
    n_error = (n_tail.error + abs(c) * model_t + sigma_s + sigma_c * m +
               (abs(c_lo) + sigma_c) * t_true_max)
    # d = C - s1 r1 (fast two-sum) - [(s1 r2 + s2 r) + S t_minus_r] + C_lo
    rotation = abs(s1) * m
    require(c == 0 or abs(c) >= rotation, 'fast_two_sum precondition (tan denominator) at k = %d' % k)
    head_error = half_ulp(abs(c) + rotation)
    cross = add(multiply(constant(s1), r2_cut), multiply(s2, r_value))
    d_tail = add(subtract(Value(-head_error, head_error), add(cross, multiply(constant(s), t_minus_r))),
                 constant(c_lo))
    d_error = (d_tail.error + abs(s) * model_t + sigma_c + sigma_s * m +
               (abs(s_lo) + sigma_s) * t_true_max)
    return n_error, d_error


def function_lower_bounds(k, t_low, t_high):
    """Lower bounds of |sin| and |cos| of k pi/128 + t over t in [t_low,
    t_high]. Where the function has no zero in the interval, |f| is concave
    there and its minimum is at an end; at the four indices where it has
    one, |f| = |sin t| >= |t| (1 - t^2/6) with t of one sign."""
    (s_low, s_high), (c_low, c_high) = sin_cos_of_step_multiple(k)

    def endpoint_values(t):
        (st_low, st_high), (ct_low, ct_high) = sin_cos_small(t, t)
        sin_candidates = [a * b for a in (s_low, s_high) for b in (ct_low, ct_high)]
        sin_candidates2 = [a * b for a in (c_low, c_high) for b in (st_low, st_high)]
        sin_low_value = min(sin_candidates) + min(sin_candidates2)
        sin_high_value = max(sin_candidates) + max(sin_candidates2)
        cos_candidates = [a * b for a in (c_low, c_high) for b in (ct_low, ct_high)]
        cos_candidates2 = [a * b for a in (s_low, s_high) for b in (st_low, st_high)]
        cos_low_value = min(cos_candidates) - max(cos_candidates2)
        cos_high_value = max(cos_candidates) - min(cos_candidates2)
        sin_bound = Fraction(0) if sin_low_value <= 0 <= sin_high_value else min(abs(sin_low_value), abs(sin_high_value))
        cos_bound = Fraction(0) if cos_low_value <= 0 <= cos_high_value else min(abs(cos_low_value), abs(cos_high_value))
        return sin_bound, cos_bound

    t_min = min(abs(t_low), abs(t_high)) if t_low * t_high > 0 else Fraction(0)
    t_max = magnitude_interval(t_low, t_high)
    small_sin_bound = t_min * (1 - t_max * t_max / 6)
    if k in (0, 128):
        sin_bound = small_sin_bound
    else:
        sin_bound = min(endpoint_values(t_low)[0], endpoint_values(t_high)[0])
    if k in (64, 192):
        cos_bound = small_sin_bound
    else:
        cos_bound = min(endpoint_values(t_low)[1], endpoint_values(t_high)[1])
    return sin_bound, cos_bound


# The split division in tan (tan.cpp `divide_split`), for normalized
# n + n_lo, d + d_lo (|n_lo| <= u|n|, |d_lo| <= u|d|, from fast two-sums):
# q1 = cut_26(fl(n fl(1/d))) is within 2^-24.9 (1 + 3u) of n/d, and d1 =
# cut_26(d) within 2^-25 of d, so q1 d1 (exact: 26 x 26 bits) is within
# 2^-23.9 of n and n - q1 d1 is exact (Sterbenz). Then q1 d2 (below 2^-24.9
# |n|), the difference, n_lo, and q1 d_lo (below u|n|) and the last
# difference: roundings of terms at most 2^-25, 2^-23, 2^-23, u and 2^-23
# times |n| (with slack). The correction fl(residual fl(1/d)) against
# rho / (d + d_lo): three relative roundings (1/d, the product, d_lo
# dropped from the divisor) of a term at most 2^-22.9 |n/d|. Slack 1.01
# on every magnitude.
SPLIT_SLACK = Fraction(101, 100)
SPLIT_RESIDUAL = U * SPLIT_SLACK * (Fraction(1, 2 ** 25) + 3 * Fraction(1, 2 ** 23) + U)
DIVIDE_RELATIVE = SPLIT_RESIDUAL * SPLIT_SLACK + 3 * U * Fraction(1, 2 ** 23) * SPLIT_SLACK * SPLIT_SLACK


BUDGET_ULPS = BUDGET_ULP


def core_certificate(r_min, r_max):
    """The core's worst error in ulps of the result (sin, cos, tan) over every
    table index and every r with r_min <= |r| <= r_max (geometric pieces at
    the indices where a function vanishes, equal pieces elsewhere), and the
    reduction contract: the largest |delta r| each piece accepts within the
    stated bound's budget, as kappa (delta <= kappa |r| where the function
    vanishes at r = 0) and general (delta <= general elsewhere).

    An error E against y, with |y| >= L, is at most E / ulp_floor(L) ulps.
    A reduction error delta moves sin and cos by at most |delta| and tan by
    at most |delta| / cos^2 (second-order terms below 2^-100 of these for
    delta <= 2^-60, which the contract keeps), so a piece accepts
    delta <= (budget - core ulps) * ulp_floor(L) (sin, cos) and
    (budget - core ulps) * cos_min^2 * ulp_floor(L_tan) (tan)."""
    worst = {'sin': Fraction(0), 'cos': Fraction(0), 'tan': Fraction(0)}
    worst_at = {}
    kappa = None
    general = None
    pieces_general = 8
    ratio = Fraction(17, 16)
    special_pieces = []
    low = r_min
    while low < r_max:
        # Dyadic ends (the ratio rounded up to 2^-12) keep the arithmetic small.
        high = min(outward(low * ratio, low * ratio, 12 - math.floor(log2(low)))[1], r_max)
        special_pieces.append((low, high))
        low = high
    for k in range(256):
        special = k % 64 == 0
        pieces = []
        if special:
            for low, high in special_pieces:
                pieces.append((low, high))
                pieces.append((-high, -low))
        else:
            width = 2 * r_max / pieces_general
            for index in range(pieces_general):
                pieces.append((-r_max + index * width, -r_max + (index + 1) * width))
        for r_low, r_high in pieces:
            r_lo_max = U * magnitude_interval(r_low, r_high)
            sin_error, cos_error = core_errors(k, r_low, r_high, r_lo_max)
            n_error, d_error = tan_errors(k, r_low, r_high, r_lo_max)
            t_low = r_low - r_lo_max
            t_high = r_high + r_lo_max
            sin_bound, cos_bound = function_lower_bounds(k, t_low, t_high)
            require(sin_bound > 0 and cos_bound > 0, 'core: zero lower bound at k = %d' % k)
            t_max = magnitude_interval(t_low, t_high)
            # |sin(kP + t)| <= |S| + |C| |t| and |cos(kP + t)| <= |C| + |S| |t|,
            # with |S|, |C| bounded by the certified intervals (exact zeros stay 0).
            (s_true_low, s_true_high), (c_true_low, c_true_high) = sin_cos_of_step_multiple(k)
            s_true = magnitude_interval(s_true_low, s_true_high)
            c_true = magnitude_interval(c_true_low, c_true_high)
            sin_upper = min(Fraction(1), s_true + c_true * t_max)
            cos_upper = min(Fraction(1), c_true + s_true * t_max)
            sin_ulps = sin_error / ulp_floor(sin_bound)
            cos_ulps = cos_error / ulp_floor(cos_bound)
            # tan_rotate's n and d are sin x and cos x over cos(r + r_lo),
            # so |n| >= |sin x| and |d| >= |cos x|.
            n_relative = n_error / sin_bound
            d_relative = d_error / cos_bound
            tan_relative = (n_relative + d_relative) / (1 - d_relative) + DIVIDE_RELATIVE
            tan_low = sin_bound / cos_upper
            tan_high = sin_upper / cos_bound
            tan_ulps = tan_relative * tan_high / ulp_floor(tan_low)
            for name, value in (('sin', sin_ulps), ('cos', cos_ulps), ('tan', tan_ulps)):
                if value > worst[name]:
                    worst[name] = value
                    worst_at[name] = (k, r_low, r_high)
            accepted = min((BUDGET_ULPS - sin_ulps) * ulp_floor(sin_bound),
                           (BUDGET_ULPS - cos_ulps) * ulp_floor(cos_bound),
                           (BUDGET_ULPS - tan_ulps) * cos_bound * cos_bound * ulp_floor(tan_low))
            require(accepted > 0, 'core alone exceeds the budget at k = %d, r in [%.6g, %.6g]'
                    % (k, float(r_low), float(r_high)))
            if special:
                coefficient = accepted / magnitude_interval(r_low, r_high)
                if kappa is None or coefficient < kappa:
                    kappa_at = (k, r_low, r_high, sin_ulps, cos_ulps, tan_ulps)
                kappa = coefficient if kappa is None else min(kappa, coefficient)
            else:
                general = accepted if general is None else min(general, accepted)
    # Special indices with |r| beyond the special pieces' range do not
    # occur; general indices accept `general` at every r, and so do special
    # ones above the smallest geometric piece's ratio: the contract is
    # delta <= min(kappa |r|, general).
    print('step 2: kappa set at k = %d, r in [%.4g, %.4g] (core ulps sin %.6f cos %.6f tan %.6f)'
          % (kappa_at[0], float(kappa_at[1]), float(kappa_at[2]), float(kappa_at[3]), float(kappa_at[4]),
             float(kappa_at[5])))
    for name in ('sin', 'cos', 'tan'):
        k, r_low, r_high = worst_at[name]
        r_lo_max = U * magnitude_interval(r_low, r_high)
        parts = {}
        core_errors(k, r_low, r_high, r_lo_max, parts)
        sin_bound, cos_bound = function_lower_bounds(k, r_low - r_lo_max, r_high + r_lo_max)
        print('breakdown (%s): worst at k = %d, r in [%.6g, %.6g]; |sin| >= %.6g (ulp >= 2^%d), '
              '|cos| >= %.6g (ulp >= 2^%d)' % (name, k, float(r_low), float(r_high), float(sin_bound),
                                                 round(log2(ulp_floor(sin_bound))), float(cos_bound),
                                                 round(log2(ulp_floor(cos_bound)))))
        for function in ('sin', 'cos'):
            for label, value in parts[function]:
                print('    %s: %-58s 2^%.2f' % (function, label, log2(value) if value else float('-inf')))
    return worst, kappa, general


# ---- the double-double values of the worst cases -----------------------

def round_to_double(value):
    """The nearest double to a rational (no ties: callers pass an interval
    whose two ends must round alike)."""
    value = Fraction(value)
    if value == 0:
        return Fraction(0)
    sign = -1 if value < 0 else 1
    magnitude = abs(value)
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if Fraction(2) ** exponent > magnitude:
        exponent -= 1
    quantum = Fraction(2) ** (exponent - 52)
    scaled = magnitude / quantum
    integer = math.floor(scaled)
    if scaled - integer > Fraction(1, 2) or (scaled - integer == Fraction(1, 2) and integer % 2 == 1):
        integer += 1
    return sign * integer * quantum


def round_down_double(value):
    """The largest double not above a positive rational."""
    candidate = round_to_double(value)
    if candidate > value:
        quantum = Fraction(2) ** (math.floor(log2(candidate)) - 52)
        candidate -= quantum
    require(candidate <= value, 'round_down_double')
    return candidate


def double_double(low, high):
    """hi + lo for a value known in [low, high]: hi and lo both certain."""
    hi = round_to_double(low)
    require(hi == round_to_double(high), 'worst-case value straddles a rounding boundary')
    lo_low = round_to_double(low - hi)
    lo_high = round_to_double(high - hi)
    require(lo_low == lo_high, 'worst-case remainder straddles a rounding boundary')
    return hi, lo_low


def hex_double(value):
    return float(value).hex()


# ---- the certificate ----------------------------------------------------

def main(arguments):
    global SEARCH_PROGRAM
    if '--search' in arguments:
        SEARCH_PROGRAM = arguments[arguments.index('--search') + 1]
    print('pi certified to %.0f bits (Machin and Gauss agree)' % -log2(PI_HIGH - PI_LOW))
    all_binades = binades()

    # Step 1.
    worst = worst_remainders(all_binades)
    global_minimum = None
    for binade in all_binades:
        for name, item in worst[binade].items():
            if item is not None and (global_minimum is None or item[0] < global_minimum):
                global_minimum = item[0]
    print('step 1: smallest |x - q pi/128| over every double 2^-27 <= |x| <= 2^32, q != 0: 2^%.2f'
          % log2(global_minimum))

    # Step 2: the core, over every |r| from below the smallest remainder to
    # the largest the reduction can return.
    r_max = max(q_bound(binade)[1] for binade in all_binades) * (1 + Fraction(1, 2 ** 30))
    r_min = outward(global_minimum / 2, global_minimum / 2, 8 - math.floor(log2(global_minimum)))[0]
    r_max = outward(r_max, r_max, 40)[1]
    core_worst, kappa, general = core_certificate(r_min, r_max)
    for name in ('sin', 'cos', 'tan'):
        print('step 2: core error (%s) <= %.6f ulp beyond the final rounding' % (name, float(core_worst[name])))
    print('step 2: contract: |delta r| <= min(2^%.2f |r|, 2^%.2f)' % (log2(kappa), log2(general)))

    # Steps 3 and 4: each path against the contract. A path's error bound
    # A(rho) over inputs with |r| <= rho grows with rho, and the contract
    # min(kappa rho, general) is concave, so on any [rho_a, rho_b]
    #   A(rho) <= A(rho_b) <= min(contract(rho_a), contract(rho_b)) <= contract(rho)
    # once the middle inequality holds. The remainder range is covered by
    # such intervals in steps of 2 (A grows like a constant plus u^2 rho, the
    # contract like kappa rho with kappa >> u^2).
    def contract(rho):
        return min(kappa * rho, general)

    def holds(error_at, rho_low, rho_high, slack=Fraction(0)):
        if rho_low - slack <= 0:
            return False
        low = rho_low
        while low < rho_high:
            high = min(2 * low, rho_high)
            error_high = error_at(high)
            if not (error_high <= contract(low - slack) and error_high <= contract(high - slack)):
                return False
            low = high
        return True

    fast_binades = [b for b in all_binades if b.path == 'fast']
    medium_binades = [b for b in all_binades if b.path == 'medium']
    any_class = CLASSES[0][2]
    worst_fast = max(fast_reduction_error(b) for b in fast_binades)
    print('step 3: three-part reduction (|x| <= 2^20): absolute error <= 2^%.2f' % log2(worst_fast))
    # The three-part path serves inputs whose computed |r| >= limit, whose
    # true remainder is then >= limit (1 - u) - worst_fast. The smallest
    # power of two at which the three-part path meets the contract over
    # [limit, R] in every fast binade:
    def fast_holds_at(limit):
        slack = limit * U + worst_fast
        for binade in fast_binades:
            r_cap = q_bound(binade)[1]
            if q_bound(binade)[0] == 0 or r_cap < limit:
                continue
            def error_at(rho, binade=binade):
                return fast_reduction_error(binade, rho)
            if not holds(error_at, limit, r_cap, slack):
                return False
        return True

    limit = Fraction(1, 2 ** 60)
    while limit < 1 and not fast_holds_at(limit):
        limit *= 2
    require(limit < 1, 'three-part reduction never meets the contract')
    print('step 4: derived small-remainder limit: 2^%d' % round(log2(limit)))
    match = re.search(r'sincos_small_remainder_limit = (0x[0-9a-fp.+-]+);', open(CORE_PATH).read())
    if match is None:
        require(False, 'sincos_core.hpp has no sincos_small_remainder_limit')
    else:
        stored_limit = exact(match.group(1))
        require(fast_holds_at(stored_limit), 'three-part reduction exceeds the contract at the stored limit')
        print('step 4: stored sincos_small_remainder_limit = 2^%d holds' % round(log2(stored_limit)))
        limit = stored_limit

    # The eight-part path: every medium binade over [r_min, R], and every
    # fast binade's small remainders, [r_min, limit + slack].
    worst_margin = None
    required_bits = 0
    for binade in all_binades:
        item = worst[binade][any_class]
        if item is None:
            continue
        r_min_binade = item[0]
        r_cap = q_bound(binade)[1]
        if binade.path == 'fast':
            if r_min_binade >= limit:
                continue
            r_high = min(r_cap, limit + limit * U + worst_fast)
        else:
            r_high = r_cap
        def error_at(rho, binade=binade):
            return medium_reduction_error(binade, rho)
        require(holds(error_at, r_min_binade, r_high),
                'eight-part reduction exceeds the contract in ' + binade.name())
        error_near = medium_reduction_error(binade, 2 * r_min_binade)
        margin = log2(contract(r_min_binade)) - log2(error_near)
        worst_margin = margin if worst_margin is None else min(worst_margin, margin)
        q_max = q_bound(binade)[0]
        needed = math.ceil(log2(q_max) - log2(contract(r_min_binade)) + log2(STEP_LOW))
        required_bits = max(required_bits, needed)
    represented = -log2(tail_bound(MEDIUM_PARTS + [MEDIUM_REST]) / STEP_LOW)
    print('step 4: at the certified minima, the truncation alone needs pi/128 to %d bits; '
          'the eight-part split holds %.1f' % (required_bits, represented))
    print('step 4: eight-part reduction within the contract at every certified minimum, margin 2^%.1f'
          % worst_margin)
    print('composed bound: sin, cos, tan within %.4f ulp for every double 2^-27 <= |x| <= 2^32'
          % float(STATED_BOUND_ULPS))

    header = make_header(all_binades, worst, kappa, general)
    if '--write' in arguments:
        path = arguments[arguments.index('--write') + 1]
        open(path, 'w').write(header)
        print('wrote ' + path)
    if '--check' in arguments:
        path = arguments[arguments.index('--check') + 1]
        require(open(path).read() == header, path + ' differs from the generated table')
    if failures:
        print('%d failure(s)' % len(failures))
        return 1
    print('certificate holds')
    return 0


def make_header(all_binades, worst, kappa, general):
    lines = []
    lines.append('#pragma once')
    lines.append('')
    lines.append('// Generated by tools/sincos_certificate.py. Do not edit by hand.')
    lines.append('// The certified worst cases of the double sin/cos reduction: for every')
    lines.append('// binade of 2^-27 <= x <= 2^32 and each residue class of q (any; q = 0')
    lines.append('// mod 128; q = 64 mod 128), the double x closest to a multiple q pi/128')
    lines.append('// (q != 0), with the exact remainder x - q pi/128 and sin, cos and tan')
    lines.append('// of x, each as hi + lo (hi the nearest double, lo the nearest double to')
    lines.append('// the rest). Exact to far below lo\'s last bit.')
    lines.append('')
    lines.append('#include <cstdint>')
    lines.append('')
    lines.append('namespace sincos_worst_cases {')
    lines.append('')
    lines.append('// The reduction contract (tools/sincos_certificate.py, step 2), rounded')
    lines.append('// down: |(r + r_lo) - (x - q pi/128)| <= min(kappa |x - q pi/128|, general).')
    lines.append('inline constexpr double contract_kappa = %s;' % hex_double(round_down_double(kappa)))
    lines.append('inline constexpr double contract_general = %s;' % hex_double(round_down_double(general)))
    lines.append('// The stated bound in ulps (STATED_BOUND_ULPS).')
    lines.append('inline constexpr double stated_bound_ulps = %r;' % float(STATED_BOUND_ULPS))
    lines.append('')
    lines.append('struct Case final {')
    lines.append('    double x;')
    lines.append('    double q;')
    lines.append('    double remainder_hi;')
    lines.append('    double remainder_lo;')
    lines.append('    double sin_hi;')
    lines.append('    double sin_lo;')
    lines.append('    double cos_hi;')
    lines.append('    double cos_lo;')
    lines.append('    double tan_hi;')
    lines.append('    double tan_lo;')
    lines.append('};')
    lines.append('')
    entries = []
    seen = set()
    for binade in all_binades:
        for name, item in worst[binade].items():
            if item is None:
                continue
            _, x, q = item
            if x in seen:
                continue
            seen.add(x)
            remainder_low = min(x - q * STEP_LOW, x - q * STEP_HIGH)
            remainder_high = max(x - q * STEP_LOW, x - q * STEP_HIGH)
            remainder = double_double(remainder_low, remainder_high)
            (s_low, s_high), (c_low, c_high) = sin_cos_of_double(x)
            sin_pair = double_double(s_low, s_high)
            cos_pair = double_double(c_low, c_high)
            tan_ends = [a / b for a in (s_low, s_high) for b in (c_low, c_high)]
            tan_pair = double_double(min(tan_ends), max(tan_ends))
            values = (x, Fraction(q)) + remainder + sin_pair + cos_pair + tan_pair
            entries.append('    {' + ', '.join(hex_double(v) for v in values) + '},')
    lines.append('inline constexpr Case cases[%d] = {' % len(entries))
    lines.extend(entries)
    lines.append('};')
    lines.append('')
    lines.append('} // namespace sincos_worst_cases')
    lines.append('')
    return '\n'.join(lines)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
