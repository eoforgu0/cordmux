// comments_japanese.py の OK 例。例外 (SPDX の行、URL だけのコメント、指示のコメント) は日本語を含まなくてよい。

// SPDX-License-Identifier: MIT

// https://example.com/wire-format-v0

// 2 つの数を足す純粋関数
export function add(a: number, b: number): number {
  // eslint-disable-next-line no-unused-vars
  return a + b;
}
