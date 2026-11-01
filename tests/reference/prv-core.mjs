const equal = (left, right) => left.length === right.length && left.every((value, index) => value === right[index]);

function compareVectors(left, right) {
  for (let index = 0; index < Math.max(left.length, right.length); index += 1) {
    const difference = (left[index] ?? 0) - (right[index] ?? 0);
    if (difference) return Math.sign(difference);
  }
  return 0;
}

function counts(values) {
  const result = new Map();
  for (const value of values) result.set(value, (result.get(value) ?? 0) + 1);
  return result;
}

export function subsequenceMatch(pattern, donor) {
  const positions = [];
  let start = 0;
  for (const candidate of pattern) {
    const index = donor.indexOf(candidate, start);
    if (index < 0) break;
    positions.push(index);
    start = index + 1;
  }
  return { length: positions.length, positions };
}

function candidatePositionVector(candidate, states, activeSize, excluded = -1) {
  return Array.from({ length: activeSize }, (_, rank) => states.reduce(
    (total, ballot, index) => total + (index !== excluded && ballot[rank] === candidate ? 1 : 0), 0,
  ));
}

function ballotPriority(index, states, active) {
  return states[index].flatMap((candidate) => candidatePositionVector(candidate, states, active.length, index));
}

function failure(code, detail = null) { return { code, detail }; }

export function inferOne(targetIndex, states, active) {
  const pattern = states[targetIndex];
  const missing = active.filter((candidate) => !pattern.includes(candidate));
  if (!missing.length) return { candidate: null, audit: {} };
  if (missing.length === 1) return { candidate: missing[0], audit: { reason: "sole remaining candidate" } };

  const donors = [];
  for (let donorIndex = 0; donorIndex < states.length; donorIndex += 1) {
    if (donorIndex === targetIndex) continue;
    const donor = states[donorIndex];
    const residual = donor.filter((candidate) => !pattern.includes(candidate));
    if (!residual.length) continue;
    const match = subsequenceMatch(pattern, donor);
    donors.push({ ...match, donorIndex, residual });
  }
  if (!donors.length) return { failure: failure("NO_EXPANSION_EVIDENCE", { target: pattern, missing }) };

  const bestLength = Math.max(...donors.map((donor) => donor.length));
  const lengthClass = donors.filter((donor) => donor.length === bestLength);
  const bestPositions = lengthClass.map((donor) => donor.positions).reduce(
    (best, vector) => compareVectors(vector, best) < 0 ? vector : best,
  );
  const group = lengthClass.filter((donor) => equal(donor.positions, bestPositions));
  const proposals = counts(group.map((donor) => donor.residual[0]));
  const bestCount = Math.max(...proposals.values());
  let tied = [...proposals].filter(([, count]) => count === bestCount).map(([candidate]) => candidate);
  const audit = {
    target: [...pattern], matchPrefixLength: bestLength, matchPositions: bestPositions,
    donorIndices: group.map((donor) => donor.donorIndex), proposalCounts: Object.fromEntries(proposals),
  };
  if (tied.length === 1) return { candidate: tied[0], audit };

  const residualLength = Math.max(...group.map((donor) => donor.residual.length));
  const vectors = Object.fromEntries(tied.map((candidate) => [candidate, Array.from(
    { length: residualLength }, (_, rank) => group.filter((donor) => donor.residual[rank] === candidate).length,
  )]));
  const bestVector = tied.map((candidate) => vectors[candidate]).reduce(
    (best, vector) => compareVectors(vector, best) > 0 ? vector : best,
  );
  tied = tied.filter((candidate) => equal(vectors[candidate], bestVector));
  audit.tieVectors = vectors;
  if (tied.length !== 1) return { failure: failure("PERFECT_EXPANSION_TIE", { target: pattern, candidates: tied, vectors }), audit };
  return { candidate: tied[0], audit };
}

export function expandBallots(projectedBallots, active) {
  const states = projectedBallots.map((ballot) => ballot.filter((candidate) => active.includes(candidate)));
  const trace = [];
  let step = 0;
  while (true) {
    const incomplete = states.map((ballot, index) => ballot.length < active.length ? index : -1).filter((index) => index >= 0);
    if (!incomplete.length) return { ballots: states, trace };
    const maximumLength = Math.max(...incomplete.map((index) => states[index].length));
    const level = incomplete.filter((index) => states[index].length === maximumLength);
    const signatures = new Map(level.map((index) => [index, ballotPriority(index, states, active)]));
    const highest = [...signatures.values()].reduce((best, vector) => compareVectors(vector, best) > 0 ? vector : best);
    const batch = level.filter((index) => equal(signatures.get(index), highest));
    const snapshot = states.map((ballot) => [...ballot]);
    const decisions = [];
    for (const index of batch) {
      const inferred = inferOne(index, snapshot, active);
      if (inferred.failure) return { failure: inferred.failure, trace: [...trace, { phase: "expansion", step, batch, failure: inferred.failure, audit: inferred.audit }] };
      decisions.push({ ballot: index, candidate: inferred.candidate, audit: inferred.audit });
    }
    for (const decision of decisions) states[decision.ballot] = [...states[decision.ballot], decision.candidate];
    trace.push({ phase: "expansion", step, currentLength: maximumLength, batch, decisions });
    step += 1;
  }
}

function perfectCycle(ballots, remaining) {
  if (remaining.length < 3) return null;
  const projected = ballots.map((ballot) => ballot.filter((candidate) => remaining.includes(candidate)));
  const positional = Object.fromEntries(remaining.map((candidate) => [candidate, candidatePositionVector(candidate, projected, remaining.length)]));
  const first = positional[remaining[0]];
  if (!remaining.every((candidate) => equal(positional[candidate], first))) return null;
  const margins = new Map();
  const wins = new Map(remaining.map((candidate) => [candidate, new Set()]));
  for (let leftIndex = 0; leftIndex < remaining.length; leftIndex += 1) {
    for (let rightIndex = leftIndex + 1; rightIndex < remaining.length; rightIndex += 1) {
      const left = remaining[leftIndex]; const right = remaining[rightIndex];
      const leftVotes = projected.filter((ballot) => ballot.indexOf(left) < ballot.indexOf(right)).length;
      const margin = leftVotes - (projected.length - leftVotes);
      margins.set(`${left}\u0000${right}`, margin); margins.set(`${right}\u0000${left}`, -margin);
      if (margin > 0) wins.get(left).add(right); else if (margin < 0) wins.get(right).add(left);
    }
  }
  const reachesAll = (start) => {
    const seen = new Set([start]); const stack = [start];
    while (stack.length) for (const next of wins.get(stack.pop())) if (!seen.has(next)) { seen.add(next); stack.push(next); }
    return seen.size === remaining.length;
  };
  if (!remaining.every(reachesAll)) return null;
  const signatures = Object.fromEntries(remaining.map((candidate) => [candidate, remaining.filter((other) => other !== candidate).map((other) => margins.get(`${candidate}\u0000${other}`)).sort((a, b) => a - b)]));
  if (!remaining.every((candidate) => equal(signatures[candidate], signatures[remaining[0]]))) return null;
  return { positionalVectors: positional, marginSignatures: signatures };
}

export function eliminate(ballots, active) {
  const remaining = [...active]; const trace = []; let round = 0;
  while (true) {
    const projected = ballots.map((ballot) => ballot.filter((candidate) => remaining.includes(candidate)));
    const firstCounts = Object.fromEntries(remaining.map((candidate) => [candidate, projected.filter((ballot) => ballot[0] === candidate).length]));
    const cycle = perfectCycle(ballots, remaining);
    if (cycle) return { failure: failure("PERFECT_CONDORCET_CYCLE", cycle), trace: [...trace, { phase: "elimination", round, remaining: [...remaining], firstCounts, failure: "PERFECT_CONDORCET_CYCLE" }] };
    const majority = remaining.find((candidate) => 2 * firstCounts[candidate] > ballots.length);
    if (majority) return { winner: majority, trace: [...trace, { phase: "elimination", round, remaining: [...remaining], firstCounts, winner: majority, reason: "majority" }] };
    if (remaining.length === 2) {
      const [left, right] = remaining;
      if (firstCounts[left] === firstCounts[right]) return { failure: failure("PERFECT_FINAL_TIE", firstCounts), trace };
      const winner = firstCounts[left] > firstCounts[right] ? left : right;
      return { winner, trace: [...trace, { phase: "elimination", round, remaining: [...remaining], firstCounts, winner, reason: "final pair" }] };
    }
    const vectors = Object.fromEntries(remaining.map((candidate) => [candidate, candidatePositionVector(candidate, projected, remaining.length)]));
    const lowestVector = remaining.map((candidate) => vectors[candidate]).reduce((best, vector) => compareVectors(vector, best) < 0 ? vector : best);
    const lowest = remaining.filter((candidate) => equal(vectors[candidate], lowestVector));
    if (lowest.length !== 1) return { failure: failure("PERFECT_ELIMINATION_TIE", { candidates: lowest, vectors }), trace };
    trace.push({ phase: "elimination", round, remaining: [...remaining], firstCounts, vectors, eliminated: lowest[0] });
    remaining.splice(remaining.indexOf(lowest[0]), 1); round += 1;
  }
}

function runMini(ballots, active) {
  const expansion = expandBallots(ballots, active);
  if (expansion.failure) return { failure: expansion.failure, trace: expansion.trace };
  const result = eliminate(expansion.ballots, active);
  return { ...result, expandedBallots: expansion.ballots, trace: [...expansion.trace, ...result.trace] };
}

function seedCandidates(ballots, candidates) {
  const scale = 1n << BigInt(candidates.length - 1);
  const scores = {}; const vectors = {};
  for (const candidate of candidates) {
    const vector = candidatePositionVector(candidate, ballots, candidates.length);
    vectors[candidate] = vector;
    scores[candidate] = vector.reduce((sum, count, rank) => sum + BigInt(count) * (scale >> BigInt(rank)), 0n);
  }
  for (let left = 0; left < candidates.length; left += 1) for (let right = left + 1; right < candidates.length; right += 1) {
    if (scores[candidates[left]] === scores[candidates[right]] && equal(vectors[candidates[left]], vectors[candidates[right]])) return { failure: failure("PERFECT_SEED_TIE", { candidates: [candidates[left], candidates[right]] }) };
  }
  return { order: [...candidates].sort((left, right) => scores[left] === scores[right] ? -compareVectors(vectors[left], vectors[right]) : scores[left] > scores[right] ? -1 : 1), scores, vectors };
}

export function runPrv(rawBallots, candidates, options = {}) {
  const active = [...candidates];
  if (active.length < 2) return { failure: failure("TOO_FEW_CANDIDATES"), trace: [] };
  if (new Set(active).size !== active.length) return { failure: failure("DUPLICATE_CANDIDATE_ID"), trace: [] };
  const ballots = []; const rejected = [];
  rawBallots.forEach((ballot, index) => {
    const valid = Array.isArray(ballot) && ballot.length && new Set(ballot).size === ballot.length && ballot.every((candidate) => active.includes(candidate));
    if (valid) ballots.push([...ballot]); else rejected.push(index);
  });
  const trace = [{ phase: "validation", validBallots: ballots.length, rejectedBallots: rejected, candidateCount: active.length }];
  if (ballots.length <= active.length) return { failure: failure("TOO_FEW_VALID_BALLOTS", { validBallots: ballots.length, candidates: active.length }), trace };
  const threshold = options.bracketThreshold ?? 30; const interval = options.bracketInterval ?? 10;
  if (active.length <= threshold) { const result = runMini(ballots, active); return { ...result, trace: [...trace, { phase: "bracketing", enabled: false }, ...result.trace] }; }
  if (interval < 2) return { failure: failure("INVALID_BRACKET_INTERVAL", interval), trace };
  const seeded = seedCandidates(ballots, active);
  if (seeded.failure) return { failure: seeded.failure, trace };
  const width = interval - 1; const segments = [];
  for (let start = 0; start < seeded.order.length; start += width) segments.push(seeded.order.slice(start, start + width));
  trace.push({ phase: "seeding", order: seeded.order, segments });
  let promoted = null;
  for (let index = segments.length - 1; index >= 0; index -= 1) {
    const bracket = [...segments[index], ...(promoted ? [promoted] : [])];
    if (bracket.length === 1) { promoted = bracket[0]; trace.push({ phase: "bracket", index, active: bracket, winner: promoted }); continue; }
    const result = runMini(ballots.map((ballot) => ballot.filter((candidate) => bracket.includes(candidate))), bracket);
    trace.push({ phase: "bracket", index, active: bracket, winner: result.winner ?? null, failure: result.failure ?? null, trace: result.trace });
    if (result.failure) return { failure: result.failure, trace };
    promoted = result.winner;
  }
  return { winner: promoted, trace };
}
