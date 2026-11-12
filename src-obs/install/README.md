# src-obs/install

Vesktop に cordmux の Vencord を追加するための state.json の書き換え、更新したときの data/vencord のコピー、アンインストールの手順を決める。uninstall_real.cpp のほかは、ファイル、時計、OS を自分では読まない。呼び出し元 (dock/ と obs_module_load) が読んだ結果を引数で受け取って何をするかを返すか、ファイルと OBS を扱う関数を注入されて使う。実際のファイルの読み書きは platform/ の関数が行う。インストールとアンインストールの流れは docs/architecture.md の「配布、インストール、更新、アンインストール」にある。

## 主な型と関数

- `plan_write()` と `plan_restore()` (`vencord_dir.h`): state.json の vencordDir を書き換えるか、元に戻すかを決める。書き換える前の値はバックアップ (`Backup`) として vencord_dir_backup.json に保存する。
- `parse_build_mark()` と `classify_shown_bundle()` (`build_mark.h`): マーカーのファイル cordmux-build.txt を読み、data/vencord が data/vencord-src と一致しているか、前のバージョンのファイルが残っているか、ほかのツールに上書きされたかを判定する。
- `plan_reinstall()` (`reinstall_plan.h`): [再インストール] で data/vencord-src から data/vencord へコピーする手順を決める。
- `kRemovalTable` (`removal_table.h`): アンインストールで消す対象の表。test/lint/removal_table.py と test/tools/obs-run/install_snapshot.py もこのヘッダーを読むので、ヘッダーのコメントにある形を守る。
- `run_uninstall()` (`uninstall.h`): アンインストールの段階を順に実行し、残ったものを結果にまとめる。段階ごとの処理は関数として注入する。
- `remove_from_vesktop()` (`uninstall.h`): 状態タブの [Vesktopから削除]。アンインストールの段階 (3) と同じ処理 (`plan_restore` と、Vencord の設定の cordmux の項の戻し) で、Vesktop の設定を cordmux を追加する前の値に戻す。段階 (3) と違い、バックアップと cordmux の設定とファイルは消さない。そのため、後で [Vesktopに追加] を押すと `plan_write` の分岐 4 (元の値に戻っている) になり、バックアップは cordmux の場所で上書きされない。
- `cordmux_add_install(<target>)` (`install.cmake`): target に install/ のソースファイルを追加する。

Vesktop の設定 (state.json と Vencord の設定) は、Vesktop が起動していない間だけ書く。Vesktop は state.json を起動したときに 1 回だけ読んでメモリに持ち、ウィンドウを動かすなどで値が変わるたびに、メモリの値の全体で書き直すので、起動中に書いた値は消える (Vesktop v1.6.9 の src/main/settings.ts と src/main/mainWindow.ts)。どの書き込みも、書く直前に Vesktop が起動していないことを確認する関数 (`platform::vesktop_running_now`) を注入され、起動していれば (調べられなくても) 書かない。次の書き込みがこの形である。

- [Vesktopに追加] (dock/dock_poll_install.cpp)。
- [Vesktopから削除] (`remove_from_vesktop`)。
- 起動時の書き直し (pipeline/pipeline.h の `run_startup_vencord_dir_rewrite`)。起動していれば、ドックが Vesktop の終了を見たときにもう一度試す。
- アンインストールの段階 (3) (`restore_vencord_dir`)。起動していれば、書けなかったときと同じく、元に戻せなかった設定として止まる。

## 依存

uninstall_real.cpp のほかは、libobs にも Qt にも依存しない。nlohmann/json と platform/bundle_check.h (純粋関数だけのヘッダー) を使う。uninstall_real.cpp は、実際の platform/ と OBS の frontend API と sources/cordmux_removal.h をまとめるファイルで、製品のビルドにだけ入る。

テストは src-obs/tests/test_install_*.cpp にある。
