"""番号を検出する lint の例 (NG 例)。番号の形の例を文字列に書くと検出される (例は NG 例と OK 例のファイルに置く)。"""

SAMPLE = "See ZZ-61."  # expect: record-id
MESSAGE = "a component word after [ ] (ZZ-13)"  # expect: record-id
