# OK 例 (どの行も検出されない)

- 設定は %APPDATA%\sample (例: C:\Users\<名前>\AppData\Roaming\sample) に保存する。
- 共有の C:\Users\Public\Documents と、新しいユーザーの元になる C:\Users\Default と C:\Users\All Users。
- GitHub Actions の C:\Users\runneradmin\work と /home/runner/work。
- 環境変数 %USERPROFILE%、$env:USERPROFILE、${HOME}、C:\Users\%USERNAME%\Videos、/c/Users/$USER/bin、/home/$USER/bin。
- 例の名前 /home/<名前>/projects と C--Users-<名前>-projects。
- 省略した C:\Users\...\AppData。
- Users という文字列だけ: the Users folder、C:\Users。
- URL の中 https://example.com/c/Users/alice と https://example.com/home/alice は検出しない。
