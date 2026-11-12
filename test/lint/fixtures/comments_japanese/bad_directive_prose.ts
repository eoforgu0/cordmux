// comments_japanese.py の NG 例。指示の語 (eslint など) を含むだけの英語の説明文を、指示のコメントとして
// 扱わずに検出することを確かめる。

// This comment mentions the eslint parser and explains nothing else here
export function noop(): void {
  return;
}
