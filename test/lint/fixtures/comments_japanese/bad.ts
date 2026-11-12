// comments_japanese.py の NG 例。英語だけのコメントを検出する。

// This comment is written only in English on purpose, so the checker
// below should turn red because it has three or more English words
// and not a single Japanese character.
export function add(a: number, b: number): number {
  return a + b;
}
