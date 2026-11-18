// 把 base.apk（aapt2 产物）复制一份，并塞入 classes.dex，输出新的 zip。
// 用法: node zipadd.mjs <in.apk> <out.apk> <file> <entryName>
import fs from "node:fs";
import zlib from "node:zlib";

const [inApk, outApk, addFile, entryName] = process.argv.slice(2);
if (!inApk || !outApk || !addFile || !entryName) {
  console.error("usage: zipadd.mjs <in.apk> <out.apk> <file> <entryName>");
  process.exit(2);
}

const crc32 =
  typeof zlib.crc32 === "function"
    ? (buf) => zlib.crc32(buf) >>> 0
    : (() => {
        const table = new Uint32Array(256);
        for (let i = 0; i < 256; i++) {
          let c = i;
          for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
          table[i] = c >>> 0;
        }
        return (buf) => {
          let c = 0xffffffff;
          for (let i = 0; i < buf.length; i++) c = table[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
          return (c ^ 0xffffffff) >>> 0;
        };
      })();

function readEntries(buf) {
  // 找 EOCD
  let eocd = -1;
  for (let i = buf.length - 22; i >= 0 && i > buf.length - 22 - 65536; i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error("EOCD not found");
  const count = buf.readUInt16LE(eocd + 10);
  let ptr = buf.readUInt32LE(eocd + 16);
  const entries = [];
  for (let i = 0; i < count; i++) {
    if (buf.readUInt32LE(ptr) !== 0x02014b50) throw new Error("bad central header");
    const method = buf.readUInt16LE(ptr + 10);
    const crc = buf.readUInt32LE(ptr + 16);
    const csize = buf.readUInt32LE(ptr + 20);
    const usize = buf.readUInt32LE(ptr + 24);
    const nameLen = buf.readUInt16LE(ptr + 28);
    const extraLen = buf.readUInt16LE(ptr + 30);
    const commentLen = buf.readUInt16LE(ptr + 32);
    const lho = buf.readUInt32LE(ptr + 42);
    const name = buf.slice(ptr + 46, ptr + 46 + nameLen).toString("utf8");
    // 局部头
    if (buf.readUInt32LE(lho) !== 0x04034b50) throw new Error("bad local header " + name);
    const lNameLen = buf.readUInt16LE(lho + 26);
    const lExtraLen = buf.readUInt16LE(lho + 28);
    const dataStart = lho + 30 + lNameLen + lExtraLen;
    const raw = buf.slice(dataStart, dataStart + csize);
    entries.push({ name, method, crc, csize, usize, raw, data: raw });
    ptr += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

function writeZip(entries, outPath) {
  const chunks = [];
  const central = [];
  let offset = 0;

  for (const e of entries) {
    const nameBuf = Buffer.from(e.name, "utf8");
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6); // UTF-8
    local.writeUInt16LE(e.method, 8);
    local.writeUInt16LE(0, 10);
    local.writeUInt16LE(0, 12);
    local.writeUInt32LE(e.crc >>> 0, 14);
    local.writeUInt32LE(e.data.length, 18);
    local.writeUInt32LE(e.usize, 22);
    local.writeUInt16LE(nameBuf.length, 26);
    local.writeUInt16LE(0, 28);
    chunks.push(local, nameBuf, e.data);

    const cen = Buffer.alloc(46);
    cen.writeUInt32LE(0x02014b50, 0);
    cen.writeUInt16LE(20, 4);
    cen.writeUInt16LE(20, 6);
    cen.writeUInt16LE(0x0800, 8);
    cen.writeUInt16LE(e.method, 10);
    cen.writeUInt16LE(0, 12);
    cen.writeUInt16LE(0, 14);
    cen.writeUInt32LE(e.crc >>> 0, 16);
    cen.writeUInt32LE(e.data.length, 20);
    cen.writeUInt32LE(e.usize, 24);
    cen.writeUInt16LE(nameBuf.length, 28);
    cen.writeUInt16LE(0, 30);
    cen.writeUInt16LE(0, 32);
    cen.writeUInt16LE(0, 34);
    cen.writeUInt16LE(0, 36);
    cen.writeUInt32LE(0, 38);
    cen.writeUInt32LE(offset, 42);
    central.push(cen, nameBuf);

    offset += local.length + nameBuf.length + e.data.length;
  }

  const centralBuf = Buffer.concat(central);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(centralBuf.length, 12);
  eocd.writeUInt32LE(offset, 16);

  fs.writeFileSync(outPath, Buffer.concat([...chunks, centralBuf, eocd]));
}

const buf = fs.readFileSync(inApk);
const entries = readEntries(buf);

const payload = fs.readFileSync(addFile);
const deflated = zlib.deflateRawSync(payload, { level: 9 });
const idx = entries.findIndex((e) => e.name === entryName);
const entry = {
  name: entryName,
  method: 8,
  crc: crc32(payload),
  csize: deflated.length,
  usize: payload.length,
  data: deflated,
};
if (idx >= 0) entries[idx] = entry;
else entries.push(entry);

writeZip(entries, outApk);
console.log(`wrote ${outApk}: ${entries.length} entries (added ${entryName}, ${payload.length} -> ${deflated.length} bytes)`);
