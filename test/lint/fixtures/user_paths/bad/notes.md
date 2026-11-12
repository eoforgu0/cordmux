# NG 例 (expect の付いた行は、ユーザー名の入ったパスを含む)

- 設定は C:\Users\alice\AppData\Roaming\sample に保存する。<!-- expect: user-path -->
- 区切りが / の形 C:/Users/alice/Videos も同じ。<!-- expect: user-path -->
- 小文字の d:\users\bob\work も検出する。<!-- expect: user-path -->
- 大文字だけの名前 C:\Users\ALICE\Desktop と、大文字と小文字が混ざった名前 C:/Users/Bob/Music も検出する。<!-- expect: user-path, user-path -->
- Git Bash の形 /c/Users/alice/projects と WSL の形 /mnt/c/Users/alice/projects。<!-- expect: user-path, user-path -->
- Cygwin の形 /cygdrive/c/Users/alice/projects と、Linux の形 /home/alice/projects と、macOS の形 /Users/alice/Movies。<!-- expect: user-path, user-path, user-path -->
- ファイルの URL file:///C:/Users/alice/report.html と、エディターの URL vscode://file/C:/Users/alice/a.ts も検出する。<!-- expect: user-path, user-path -->
- ドライブを環境変数で書いた %SystemDrive%\Users\carol\Desktop も検出する。<!-- expect: user-path -->
- 共有フォルダーの形 \\pc01\c$\Users\alice\share も検出する。<!-- expect: user-path -->
- 区切りを - に替えてつないだ名前 C--Users-alice-projects-app も検出する。<!-- expect: user-path -->
- 架空の名前でも、テストのファイルの外では検出する: C:\Users\example\notes。<!-- expect: user-path -->
