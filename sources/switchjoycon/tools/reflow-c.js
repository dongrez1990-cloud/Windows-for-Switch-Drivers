// reflow-switchjoycon.js — restore line breaks in a whitespace-flattened C file.
// The file content is untouched; only newlines are re-inserted (token stream identical).
const fs = require('fs');

const src = process.argv[2];
const dst = process.argv[3] || src;
let t = fs.readFileSync(src, 'utf8');
const original = t;

// Preprocessor directives have no ';' terminator: fence them first.
t = t
  .replace(/#include\s*<[^>]+>/g, (m) => `\n${m}\n`)
  .replace(/#include\s*"[^"]+"/g, (m) => `\n${m}\n`)
  .replace(/#pragma\s+pack\(push,\s*1\)/g, (m) => `\n${m}\n`)
  .replace(/#pragma\s+pack\(pop\)/g, (m) => `\n${m}\n`)
  .replace(/#define\s+MS_TO_100NS\(ms\)\s*\(\(ULONGLONG\)\(ms\)\s*\*\s*10000\)/g, (m) => `\n${m}\n`);

let out = '';
let i = 0;
let paren = 0;
const n = t.length;

function lastChar() { return out.length ? out[out.length - 1] : ''; }
function newlineIfNeeded() {
  if (lastChar() && lastChar() !== '\n') out += '\n';
}

while (i < n) {
  const c = t[i];
  const c2 = i + 1 < n ? t[i + 1] : '';

  if (c === '/' && c2 === '*') {                 // block comment
    const end = t.indexOf('*/', i + 2);
    const stop = end < 0 ? n : end + 2;
    out += t.slice(i, stop);
    i = stop;
    newlineIfNeeded();
    continue;
  }
  if (c === '"' || c === "'") {                 // string / char literal
    let j = i + 1;
    while (j < n) {
      if (t[j] === '\\') { j += 2; continue; }
      if (t[j] === c) { j++; break; }
      j++;
    }
    out += t.slice(i, j);
    i = j;
    continue;
  }

  if (c === '(') paren++;
  if (c === ')') paren = Math.max(0, paren - 1);

  if (c === ';' && paren === 0) { out += ';\n'; i++; continue; }
  if (c === '{') { out += '{\n'; i++; continue; }
  if (c === '}') { newlineIfNeeded(); out += '}\n'; i++; continue; }
  if (c === '\r') { i++; continue; }
  if (c === '\n') { newlineIfNeeded(); i++; continue; }

  out += c;
  i++;
}

out = out.replace(/\n{3,}/g, '\n\n').replace(/[ \t]+\n/g, '\n');
if (!out.endsWith('\n')) out += '\n';

// Safety: the token stream (ignoring whitespace) must be unchanged.
const canon = (s) => s.replace(/\s+/g, '');
if (canon(original) !== canon(out)) {
  console.error('REFUSING: token stream would change');
  process.exit(2);
}
fs.writeFileSync(dst, out, 'utf8');
console.log(`rewrote ${dst}: ${original.length} -> ${out.length} chars, ${out.split('\n').length} lines (tokens identical)`);
