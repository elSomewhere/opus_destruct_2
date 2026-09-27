// Deterministic JSON writer for fixtures.
//
//  * Object keys are sorted (UTF-16 code-unit order) at every level.
//  * Finite numbers use ECMAScript Number->String, i.e. the SHORTEST decimal
//    that round-trips to the identical IEEE-754 double (full precision, no
//    information loss). -0 is written as 0.
//  * Non-finite numbers are not valid JSON: they are written as the strings
//    "Infinity", "-Infinity", "NaN".
//  * Typed arrays become plain arrays; undefined object members are dropped;
//    undefined array slots become null.
//  * Layout: a value whose children are all scalars (or arrays of scalars)
//    is written on one line; larger containers get one child per line.
//    This keeps files diff-friendly without bloating them.

export function toPlain(value) {
  if (value === null || value === undefined) return value ?? null;
  if (typeof value === 'number') {
    if (Number.isFinite(value)) return Object.is(value, -0) ? 0 : value;
    return Number.isNaN(value) ? 'NaN' : (value > 0 ? 'Infinity' : '-Infinity');
  }
  if (typeof value === 'bigint') return Number(value);
  if (typeof value !== 'object') return value;
  if (ArrayBuffer.isView(value)) return Array.from(value, (x) => toPlain(x));
  if (Array.isArray(value)) return value.map((x) => (x === undefined ? null : toPlain(x)));
  if (value instanceof Map) {
    const out = {};
    for (const [k, v] of value) out[String(k)] = toPlain(v);
    return out;
  }
  if (value instanceof Set) return [...value].map((x) => toPlain(x));
  const out = {};
  for (const key of Object.keys(value)) {
    const v = value[key];
    if (v === undefined || typeof v === 'function') continue;
    out[key] = toPlain(v);
  }
  return out;
}

function isScalar(v) {
  return v === null || typeof v !== 'object';
}

function isScalarArray(v) {
  return Array.isArray(v) && v.every(isScalar);
}

const MAX_INLINE_OBJECT = 400; // chars; longer flat objects get one key per line

function isFlat(v) {
  if (isScalar(v)) return true;
  if (Array.isArray(v)) return v.every(isScalar);
  if (!Object.values(v).every((x) => isScalar(x) || isScalarArray(x))) return false;
  return inline(v).length <= MAX_INLINE_OBJECT;
}

function scalar(v) {
  return JSON.stringify(v);
}

function inline(v) {
  if (isScalar(v)) return scalar(v);
  if (Array.isArray(v)) return `[${v.map(inline).join(',')}]`;
  const keys = Object.keys(v).sort();
  return `{${keys.map((k) => `${JSON.stringify(k)}:${inline(v[k])}`).join(',')}}`;
}

function write(v, indent, out) {
  if (isFlat(v)) {
    out.push(inline(v));
    return;
  }
  const pad = ' '.repeat(indent + 1);
  const end = ' '.repeat(indent);
  if (Array.isArray(v)) {
    out.push('[\n');
    v.forEach((item, i) => {
      out.push(pad);
      write(item, indent + 1, out);
      out.push(i < v.length - 1 ? ',\n' : '\n');
    });
    out.push(`${end}]`);
    return;
  }
  const keys = Object.keys(v).sort();
  out.push('{\n');
  keys.forEach((k, i) => {
    out.push(`${pad}${JSON.stringify(k)}:`);
    write(v[k], indent + 1, out);
    out.push(i < keys.length - 1 ? ',\n' : '\n');
  });
  out.push(`${end}}`);
}

export function stableStringify(value) {
  const out = [];
  write(toPlain(value), 0, out);
  out.push('\n');
  return out.join('');
}

// Inverse of the non-finite encoding, for readers (check.mjs).
export function num(v) {
  if (typeof v === 'number') return v;
  if (v === 'Infinity') return Infinity;
  if (v === '-Infinity') return -Infinity;
  if (v === 'NaN') return NaN;
  return v;
}
