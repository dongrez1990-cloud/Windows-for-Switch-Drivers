// 生成应用图标（无第三方依赖）：先 2 倍超采样再降采样得到平滑圆角。
import fs from "node:fs";
import zlib from "node:zlib";

const SS = 2;
const SIZE = 192;
const W = SIZE * SS;
const px = new Uint8Array(W * W * 4);

function blend(x, y, r, g, b, a) {
  if (x < 0 || y < 0 || x >= W || y >= W) return;
  const i = (y * W + x) * 4;
  const sa = a / 255;
  px[i] = Math.round(px[i] * (1 - sa) + r * sa);
  px[i + 1] = Math.round(px[i + 1] * (1 - sa) + g * sa);
  px[i + 2] = Math.round(px[i + 2] * (1 - sa) + b * sa);
  px[i + 3] = Math.max(px[i + 3], a);
}

function roundRect(x0, y0, x1, y1, rad, color) {
  const [r, g, b, a] = color;
  for (let y = Math.floor(y0); y < Math.ceil(y1); y++) {
    for (let x = Math.floor(x0); x < Math.ceil(x1); x++) {
      const cx = Math.min(Math.max(x + 0.5, x0 + rad), x1 - rad);
      const cy = Math.min(Math.max(y + 0.5, y0 + rad), y1 - rad);
      const dx = x + 0.5 - cx;
      const dy = y + 0.5 - cy;
      if (dx * dx + dy * dy <= rad * rad) blend(x, y, r, g, b, a);
    }
  }
}

// 背景
roundRect(0, 0, W, W, 42 * SS, [26, 32, 44, 255]);
// 键盘外壳
roundRect(18 * SS, 52 * SS, 174 * SS, 140 * SS, 16 * SS, [43, 52, 70, 255]);

const keyW = 16;
const keyH = 15;
const gapX = 6;
const startX = 31;
const rowsY = [66, 88, 110];
for (let gy = 0; gy < 3; gy++) {
  for (let gx = 0; gx < 6; gx++) {
    if (gy === 2 && gx > 0 && gx < 5) continue;
    const x = startX + gx * (keyW + gapX);
    const y = rowsY[gy];
    const isAccent = gy === 1 && gx === 5;
    roundRect(x * SS, y * SS, (x + keyW) * SS, (y + keyH) * SS, 3 * SS,
      isAccent ? [76, 154, 255, 255] : [92, 106, 130, 255]);
  }
}
// 空格
roundRect(53 * SS, 110 * SS, 133 * SS, 125 * SS, 3 * SS, [76, 154, 255, 255]);

// 降采样
const out = new Uint8Array(SIZE * SIZE * 4);
for (let y = 0; y < SIZE; y++) {
  for (let x = 0; x < SIZE; x++) {
    let r = 0, g = 0, b = 0, a = 0;
    for (let dy = 0; dy < SS; dy++) {
      for (let dx = 0; dx < SS; dx++) {
        const i = ((y * SS + dy) * W + (x * SS + dx)) * 4;
        r += px[i]; g += px[i + 1]; b += px[i + 2]; a += px[i + 3];
      }
    }
    const n = SS * SS;
    const o = (y * SIZE + x) * 4;
    out[o] = Math.round(r / n);
    out[o + 1] = Math.round(g / n);
    out[o + 2] = Math.round(b / n);
    out[o + 3] = Math.round(a / n);
  }
}

// raw scanlines with filter byte 0
const raw = Buffer.alloc((SIZE * 4 + 1) * SIZE);
for (let y = 0; y < SIZE; y++) {
  raw[y * (SIZE * 4 + 1)] = 0;
  Buffer.from(out.buffer, y * SIZE * 4, SIZE * 4).copy(raw, y * (SIZE * 4 + 1) + 1);
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crcBuf = Buffer.alloc(4);
  crcBuf.writeUInt32BE(zlib.crc32 ? zlib.crc32(body) >>> 0 : crcFallback(body), 0);
  return Buffer.concat([len, body, crcBuf]);
}

function crcFallback(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    c ^= buf[i];
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  }
  return (c ^ 0xffffffff) >>> 0;
}

const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(SIZE, 0);
ihdr.writeUInt32BE(SIZE, 4);
ihdr[8] = 8;   // bit depth
ihdr[9] = 6;   // RGBA
ihdr[10] = 0;
ihdr[11] = 0;
ihdr[12] = 0;

const png = Buffer.concat([
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
  chunk("IHDR", ihdr),
  chunk("IDAT", zlib.deflateSync(raw, { level: 9 })),
  chunk("IEND", Buffer.alloc(0)),
]);

const outPath = process.argv[2];
fs.mkdirSync(outPath.substring(0, outPath.lastIndexOf("\\")), { recursive: true });
fs.writeFileSync(outPath, png);
console.log(`icon written: ${outPath} (${png.length} bytes)`);
