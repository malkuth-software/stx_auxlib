// Runs the reference PRV count (prv-core.mjs, copied unchanged from
// paymenottowork deploy/site, ideas/preference-ranked-voting) on
// deterministic random elections and prints one case per line for
// tests/voting_tests.cpp:
//   candidates threshold interval | ballot;ballot;... | outcome
// A ballot is space-separated candidate numbers (a number >= candidates or
// a repeat makes it invalid; "-" is an empty ballot). The outcome is
// "winner N" or "failure CODE".
//   node prv_reference.mjs > prv_cases.txt
import { runPrv } from "./prv-core.mjs";

let state = 0x9e3779b9n;
function next(bound) {
  state = (state * 6364136223846793005n + 1442695040888963407n) & ((1n << 64n) - 1n);
  return Number((state >> 33n) % BigInt(bound));
}

function election(candidates, voters, style) {
  const ballots = [];
  // A few voter blocs with shared orders make inference meaningful.
  const blocs = Array.from({ length: 1 + next(4) }, () => {
    const order = Array.from({ length: candidates }, (_, i) => i);
    for (let i = order.length - 1; i > 0; i -= 1) { const j = next(i + 1); [order[i], order[j]] = [order[j], order[i]]; }
    return order;
  });
  for (let v = 0; v < voters; v += 1) {
    let ballot = [...blocs[next(blocs.length)]];
    if (next(4) === 0) { const i = next(candidates); const j = next(candidates); [ballot[i], ballot[j]] = [ballot[j], ballot[i]]; }
    const length = style === "full" ? candidates : 1 + next(candidates);
    ballot = ballot.slice(0, length);
    if (style === "messy" && next(12) === 0) ballot = next(2) === 0 ? [] : [...ballot, ballot[0]];
    if (style === "messy" && next(20) === 0) ballot = [...ballot.slice(0, 1), candidates + next(3)];
    ballots.push(ballot);
  }
  return ballots;
}

const lines = [];
function record(candidates, ballots, options) {
  const names = Array.from({ length: candidates }, (_, i) => `c${i}`);
  const named = ballots.map((ballot) => ballot.map((choice) => `c${choice}`));
  const result = runPrv(named, names, options);
  const outcome = result.failure ? `failure ${result.failure.code}` : `winner ${result.winner.slice(1)}`;
  const text = ballots.map((ballot) => ballot.length ? ballot.join(" ") : "-").join(";");
  lines.push(`${candidates} ${options.bracketThreshold ?? 30} ${options.bracketInterval ?? 10} | ${text} | ${outcome}`);
}

for (let index = 0; index < 400; index += 1) {
  const candidates = 2 + next(6);
  const voters = candidates + 1 + next(40);
  const style = ["full", "partial", "messy"][index % 3];
  record(candidates, election(candidates, voters, style), {});
}
// Small exact ties, cycles and degenerate fields.
record(3, [[0, 1, 2], [1, 2, 0], [2, 0, 1], [0, 1, 2], [1, 2, 0], [2, 0, 1]], {});
record(2, [[0], [1], [0], [1]], {});
record(2, [[0], [1], [0]], {});
record(3, [[0], [0], [1]], {});
record(4, [[0], [1]], {});
// Bracketed fields: more candidates than the threshold.
for (let index = 0; index < 12; index += 1) {
  const candidates = 8 + next(10);
  const voters = 60 + next(120);
  record(candidates, election(candidates, voters, index % 2 ? "full" : "partial"), { bracketThreshold: 6, bracketInterval: 4 });
}
for (let index = 0; index < 3; index += 1) {
  const candidates = 31 + next(10);
  record(candidates, election(candidates, 200 + next(200), "partial"), {});
}
console.log(lines.join("\n"));
