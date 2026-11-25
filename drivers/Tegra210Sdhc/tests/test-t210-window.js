// Equivalence test for the circular "widest passing tap window" selection in
// drivers/Tegra210Sdhc/src/t210_uhs.h (T210BestWindow). The implementation is
// transcribed statement-for-statement from the C code.
function bestWindow(bits) {
  const get = (i) => (bits[(i >> 5)] >> (i & 31)) & 1;
  let pass = 0;
  for (let i = 0; i < 128; i++) pass += get(i);
  if (pass === 0) return { first: 0, width: 0, tap: 0 };
  if (pass === 128) return { first: 0, width: 128, tap: 64 };
  let bestFirst = 0, bestWidth = 0;
  for (let i = 0; i < 128; i++) {
    if (!get(i)) continue;
    if (get((i + 127) & 127)) continue; // not a run start
    let run = 0, j = i;
    for (; run < 128; run++) {
      if (!get(j & 127)) break;
      j++;
    }
    if (run > bestWidth) { bestWidth = run; bestFirst = i; }
  }
  return { first: bestFirst, width: bestWidth, tap: (bestFirst + Math.floor(bestWidth / 2)) & 127 };
}

function makeBits(set) {
  const bits = [0, 0, 0, 0];
  for (const i of set) bits[i >> 5] |= 1 << (i & 31);
  return bits;
}

function check(name, set, wantFirst, wantWidth, wantTap) {
  const r = bestWindow(makeBits(set));
  const ok = r.first === wantFirst && r.width === wantWidth && r.tap === wantTap;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}  first=${r.first} width=${r.width} tap=${r.tap} (want ${wantFirst}/${wantWidth}/${wantTap})`);
  return ok;
}

let allOk = true;
// A single clean run 10..41 (32 taps) -> centre 10 + 16 = 26.
allOk &= check('single run 10..41', Array.from({length: 32}, (_, i) => i + 10), 10, 32, 26);
// Widest run, flanked by two shorter runs.
const multi = [...Array.from({length: 6}, (_, i) => i + 2),
               ...Array.from({length: 20}, (_, i) => i + 40),
               ...Array.from({length: 8}, (_, i) => i + 90)];
allOk &= check('widest of three runs', multi, 40, 20, 50);
// Wrapping run: taps 120..127 and 0..3 are contiguous in time (width 12).
const wrap = [...Array.from({length: 8}, (_, i) => i + 120),
              ...Array.from({length: 4}, (_, i) => i + 0)];
allOk &= check('wrapping run 120..127+0..3', wrap, 120, 12, 126);
// Nothing passes.
allOk &= check('no taps pass', [], 0, 0, 0);
// Everything passes: no edge to follow, middle tap 64.
allOk &= check('all taps pass', Array.from({length: 128}, (_, i) => i), 0, 128, 64);
// Adjacent runs with a single failing tap keep the longer run.
const gap = [...Array.from({length: 3}, (_, i) => i + 5),
             ...Array.from({length: 15}, (_, i) => i + 20)];
allOk &= check('two runs, one gap', gap, 20, 15, 27);

console.log(allOk ? 'ALL PASS' : 'SOME FAILED');
process.exit(allOk ? 0 : 1);
