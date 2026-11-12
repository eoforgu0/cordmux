// 架空の本体 (NG 例)。存在するパス app/main.cpp は検出しない。
// 設計書の 3 章と 付録 B を参照する。 expect: doc-section, doc-section
// F20 の指摘で変えた (キーの名前の形でも、キーとして書いていなければ検出する)。 expect: record-id
// C0 のセットで数える (文字の集合の名前の形でも、その説明でなければ検出する)。 expect: record-id
// 由来のない判断: 枠は 4 つ。 expect: history-label
// テストした日は 2031-02-03。 expect: date
// 設計書の 05 の 2.3 による。 expect: doc-section
// 直したのは 1 か所。 expect-candidate: history-word
// 保存先の説明は README.md の「架空のツリー (NG 例)」にある (このフォルダーの README.md にその見出しはない)。 expect: missing-heading
static const char *kMissing = "app/none.txt"; // expect: missing-path
static const char *kName = "ZZ-21 のテスト"; // expect: record-id
