// SPDX-License-Identifier: Apache-2.0

// Annex B のバイト列から SPS と PPS を取り出し、16 進の文字列で比べられるようにする純粋関数。
// renderer.js (Electron) と vitest (test/tools/obs-run/webcodecs-nal.test.ts) の両方から読むので、classic script
// として書き、末尾で window と module.exports の両方に公開する。Electron の renderer で file:// から ES モジュールを
// 読むと、Chromium の CORS で拒否されることがあるためである。NAL の中身は解釈しない。

'use strict';

// H.264 の NAL の種別 (nal_unit_type。1 バイト目の下位 5 ビット)。
var NAL_TYPE_IDR = 5;
var NAL_TYPE_SPS = 7;
var NAL_TYPE_PPS = 8;

// Annex B の開始コード (3 バイトの 00 00 01 と 4 バイトの 00 00 00 01) で切り分ける。
// 返すのは [{ start, end, type }] で、start は NAL の 1 バイト目 (開始コードの直後)。
function scanAnnexB(bytes) {
  var units = [];
  var n = bytes.length;
  var i = 0;
  var prevStart = -1;
  var prevType = -1;
  while (i + 2 < n) {
    if (bytes[i] === 0 && bytes[i + 1] === 0 && bytes[i + 2] === 1) {
      var start = i + 3;
      if (prevStart >= 0) units.push({ start: prevStart, end: i, type: prevType });
      // 4 バイトの開始コードは「3 バイトの開始コードの前に 00 が 1 つ多い」だけなので、
      // 直前の 00 は前の NAL の末尾から外す (末尾の 00 は捨ててよい = trailing zero)。
      if (units.length > 0 && i > 0 && bytes[i - 1] === 0) {
        var last = units[units.length - 1];
        if (last.end === i) last.end = i - 1;
      }
      prevStart = start;
      prevType = start < n ? bytes[start] & 0x1f : -1;
      i = start;
      continue;
    }
    i += 1;
  }
  if (prevStart >= 0) units.push({ start: prevStart, end: n, type: prevType });
  return units;
}

function toHex(bytes) {
  var out = '';
  for (var i = 0; i < bytes.length; i += 1) {
    var v = bytes[i];
    out += (v < 16 ? '0' : '') + v.toString(16);
  }
  return out;
}

// 最初の SPS と最初の PPS を 16 進で返す (なければ null)。開始コードは含めない。同じ NAL でも、開始コードが
// 3 バイトか 4 バイトかで文字列が変わるためである。
function extractParameterSets(bytes) {
  var units = scanAnnexB(bytes);
  var sps = null;
  var pps = null;
  var hasIdr = false;
  for (var i = 0; i < units.length; i += 1) {
    var u = units[i];
    if (u.type === NAL_TYPE_SPS && sps === null) sps = toHex(bytes.slice(u.start, u.end));
    else if (u.type === NAL_TYPE_PPS && pps === null) pps = toHex(bytes.slice(u.start, u.end));
    else if (u.type === NAL_TYPE_IDR) hasIdr = true;
  }
  return { sps: sps, pps: pps, hasIdr: hasIdr, unitCount: units.length };
}

// 2 つの取り出し結果を比べる。片方でも取り出せていなければ、比べられなかったことを null で表す。false にすると、
// 違ったと読めてしまうためである。
function compareParameterSets(first, second) {
  var spsEqual = first.sps !== null && second.sps !== null ? first.sps === second.sps : null;
  var ppsEqual = first.pps !== null && second.pps !== null ? first.pps === second.pps : null;
  return {
    spsEqual: spsEqual,
    ppsEqual: ppsEqual,
    bothEqual: spsEqual === true && ppsEqual === true,
    first: first,
    second: second,
  };
}

var api = {
  NAL_TYPE_IDR: NAL_TYPE_IDR,
  NAL_TYPE_SPS: NAL_TYPE_SPS,
  NAL_TYPE_PPS: NAL_TYPE_PPS,
  scanAnnexB: scanAnnexB,
  toHex: toHex,
  extractParameterSets: extractParameterSets,
  compareParameterSets: compareParameterSets,
};

if (typeof module !== 'undefined' && module.exports) module.exports = api;
if (typeof window !== 'undefined') window.cordmuxNal = api;
