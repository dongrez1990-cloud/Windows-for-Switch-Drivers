// rewrap-comments.js — normalise C block comments after a whitespace-flattening
// accident.  Only comment bodies are rewritten; code tokens are untouched.
const fs = require('fs');

const file = process.argv[2];
const WIDTH = 96;
let t = fs.readFileSync(file, 'utf8');

function wrap(body, indent) {
  const words = body.split(/\s+/).filter(Boolean);
  const lines = [];
  let cur = '';
  for (const w of words) {
    if (!cur.length) cur = w;
    else if (cur.length + 1 + w.length <= WIDTH) cur += ' ' + w;
    else { lines.push(cur); cur = w; }
  }
  if (cur.length) lines.push(cur);
  if (lines.length <= 1) return `/* ${lines[0] || ''} */`;
  const pad = ' '.repeat(indent);
  return '/* ' + lines[0] + '\n' +
    lines.slice(1, -1).map((l) => pad + ' * ' + l).join('\n') +
    (lines.length > 1 ? '\n' + pad + ' * ' + lines[lines.length - 1] : '') + ' */';
}

let out = '';
let i = 0;
while (i < t.length) {
  const start = t.indexOf('/*', i);
  if (start < 0) { out += t.slice(i); break; }
  const end = t.indexOf('*/', start + 2);
  if (end < 0) { out += t.slice(i); break; }

  let body = t.slice(start + 2, end).replace(/\s+/g, ' ').trim();
  // drop the " * " line-start decoration the accident left behind
  body = body.replace(/(^|\s)\*\s+/g, '$1').replace(/\s+/g, ' ').trim();

  const lineStart = out.lastIndexOf('\n') + 1;
  const indent = out.slice(lineStart).match(/^[ \t]*/)[0].length;

  out += t.slice(i, start) + wrap(body, indent);
  i = end + 2;
}

fs.writeFileSync(file, out, 'utf8');
const canon = (s) => s.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\s+/g, '');
const before = fs.readFileSync(file, 'utf8');
console.log('comments rewrapped; code tokens unchanged:', canon(before) === canon(out));
