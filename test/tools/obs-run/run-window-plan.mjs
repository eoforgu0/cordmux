// 実機テストの実行スクリプト run-window.mjs のうち、純粋関数の部分をまとめたファイル。
// parseArgs / planSteps / blockersFromProcessList / encoderBusy / deadlineExceeded /
// filesToPlace / judgeBackpressure / parseStatsVideoDropped / judgeConnectionHeld を持つ。
//
// 4 アカウントの通話のテスト (call-1) のためのもの:
// --step call-gate-a/-b/-c/-d と call-1 の実行計画、--call-window / --real-profile /
// --allow-state-json-write / --decoder / --call-minutes の解釈、judgeOutputSkipped
// (OBS 自身のフレームの欠落)、judgeVencordDirWritten (開始ゲートの判定)、
// judgeOperationCount (触った操作の数)。
//
// 配布・インストール・更新・アンインストールのテストのためのもの: --step install-roundtrip と
// 13 の場面 (--scenario) の実行計画、--layout / --vm / --zip / --old-zip / --obs-zip / --test-dll /
// --keep-work の解釈、roundtripPaths、obsDepsBinRelFromBuildspec、往復の判定
// (judgeRoundtrip と judgeStartupRewrite ほか)、parseBuildMarkText。
//
// 4 アカウントの通話のテストと短い項目を、1 回の実行 (2 時間 30 分) にまとめるためのもの:
// 手順の種類 "human" (人の操作待ち。通知ファイル / state.json の変化 / 秒数で進む)、call-gate-a の
// [Vesktop に追加] を人が押す形、call-1 のサブアカウントを使う構成 (--broadcaster-id)・テスト動画の A/V (av1 / av2)・
// 途中で録画 ON・使う声の置き換え・連続運転 (5 分周期の照合とわざと異常)・締め (アンインストール の判定)、
// plannedMinutes (実行計画が見込む待ちの合計)、--rehearsal (実際の Vesktop の代わりにテスト用の送信側で通す)、
// judgeVencordDirRestored / judgeAvDrift / judgeNotNamedInTexts / insertStatsAtMinutes。
// **human の message はテストを実行する人に見せる文なので、日本語の文字列リテラルで持つ** (文字列リテラルを
// ASCII にする規則の例外。内部の語 (手順の種類の名前など) は入れない)。
//
// 判定スクリプトのセルを枠の実際の位置と大きさから決める screenCellRegions / cellRegionsForJudge / judgeCellArgs、
// A/V の録画の間の音の切り分け、parseVideoRateSamples / judgeNoticeIffFramesStopped
// (手順の種類 measure-video-rate)、使う声を飛ばす手順の種類 skip (ラベルを持たないバージョンの製品の
// ためだけに残す)、OBS の録画を残す obsRecordingMoves (手順の種類 keep-obs-recordings)、
// 期限の見積もり deadlineEstimate / deadlineMinFor、参加の文 callJoinMessage。
// 締めの判定スクリプトは、人の操作を全部済ませて OBS を閉じた後に実行する (callAnalysisSteps)。
//
// この一式は Node のどの API も呼ばない (fs / path / process / child_process を import しない)。
// 実行 (fs 読み書き・プロセス起動・OBS への接続) はすべて run-window.mjs 側が担う。
// パスは文字列連結だけで組み立てる (path.join を使わない)。区切りは "/" に統一する
// (Windows でも Node の fs は "/" 区切りのパスを受け付ける)。

export const STEP_VALUES = [
  "place",
  "through",
  "integration",
  "guard",
  "second-obs",
  "discord-first",
  "dock-view",
  "mic-check",
  // 4 アカウントの通話のテスト: 開始ゲートを人の操作の前後で 4 つに割り、
  // メインの手順 (call-1) は人がゲートの成功を見てから起動する (1 つの step の中で分岐しない)。
  "call-gate-a",
  "call-gate-b",
  "call-gate-c",
  "call-gate-d",
  "call-1",
  // 録画のテスト。OBS 側の既存の record-start / record-stop / judge は OBS 自身の録画のものなので、
  // cordmux の録画は step の名前で分ける (結果を読む側が取り違えない)。
  "record",
  // webcodecs-check: 実行の最初に WebCodecs の対応を測るツール (OBS を起動しない。mic-check と同じ形)。
  "webcodecs-check",
  // 導入の往復と場面 (--scenario)。OBS を毎回 zip から展開し直し、
  // cordmux を配布用の zip から置く (--obs / --plugin を取らない唯一の step)。
  "install-roundtrip",
  // インストール・初期設定・起動・接続の後、通知ファイルが置かれるまで OBS を開いたまま待ち、閉じて後始末する。
  // 開いている間に、テストを実行する人が UI Automation のツールを手で順に打つ。
  "hold",
  // 実際の Vesktop と接続したまま OBS を開いておく手順。call-gate-a の初期設定と [Vesktop に追加] -> 開き直した Vesktop の
  // 接続 -> 通知ファイルまで待つ -> call-1 の締め (アンインストールで戻す)。
  "call-hold",
];
// 実際の %APPDATA% / %LOCALAPPDATA% を使う step。この 6 つだけが --real-profile を
// 要求し、この 6 つ以外は --real-profile も --call-window も受け付けない (テスト用の送信側だけの実行が
// 利用者の Vesktop の横で動かないようにするため)。
export const CALL_STEP_VALUES = ["call-gate-a", "call-gate-b", "call-gate-c", "call-gate-d", "call-1", "call-hold"];
export function isCallStep(step) {
  return CALL_STEP_VALUES.includes(step);
}
// --rehearsal を受ける step。
export const REHEARSAL_STEPS = ["call-1", "call-hold"];
export const HW_VALUES = ["prefer-hardware", "prefer-software"];
// マーカーの音は 1 つのストリームにだけ乗せる。harness のパラメータ syncTone に、
// 実行スクリプトがどの値を渡すかを決める純粋関数の値の一覧。
export const SYNC_TONE_VALUES = ["screen", "voice", "none"];
// 既定は "screen" (画面)。音のずれを判定しない呼び出しでも、「1 つのストリームにだけ乗せる」を
// 常に明示するための値。
export const DEFAULT_SYNC_TONE = "screen";

// --integration-config の値。"default" は --n-screen/--n-voice/--seconds を
// そのまま使う (syncTone は常に "screen" を明示する)。"one-screen" は画面 1 + 声 0 で 1 分の
// 固定値、"one-screen-one-voice" は画面 1 + 声 1 でマーカーは声に乗せる固定値
// (秒数は実測ではない見積もり)。
export const INTEGRATION_CONFIG_VALUES = ["default", "one-screen", "one-screen-one-voice"];
// 画面 1 + 声 0 の場合の 1 分。
const ONE_SCREEN_SECONDS = 60;
// 見積もり (実機の実行で測って見直す前提): 画面 1 + 声 1 でマーカーは声に乗せる。
// 画面 1 つ (60 秒) より長めの 3 分とした。
const ONE_SCREEN_ONE_VOICE_SECONDS = 180;

// --dock-scenario の値。dock-view を状態ごとに撮る形に広げたもの。8 つの状態のうち、「2 つ目の OBS」は
// 既存の --step second-obs をそのまま使う (--step second-obs が BlockedByOtherWriter の確認を持つので、
// 同じ確認を --dock-scenario の値として重ねない)。「警告が出ている間の出力の差分 0」は独立した状態ではなく
// degraded の録画に含める (下の buildDockViewSteps 参照)。この 6 つに加えて、obs-websocket を使わず、
// ドックの操作だけで載せる人を選べて 4 分割が出る確認 (自動配置・ダミー・診断情報の zip) を、人の手なしで
// 行えるように "select-four" / "dummy" / "export-bundle" を足した (下の各定数を見よ)。
export const DOCK_SCENARIO_VALUES = ["none", "connected", "no-scene", "degraded", "contact-missing", "idle-10min", "select-four", "dummy", "export-bundle"];
// 既定は「テスト用の送信側なし」: --dock-scenario を指定しなかった呼び出しは、この一覧を追加する前の
// dock-view (撮れること自体を確認する土台) とまったく同じ実行計画を返す (後方互換。この既定を変えるときは
// run-window-plan.test.ts の並びのテストも修正する)。
export const DEFAULT_DOCK_SCENARIO = "none";

// ---- 製品の画面のテキストと OBS の中の名前 ---------------------------------------
// 画面のテキストは ini だけが持ち、コードでテキストを組み直さない。ドックはタブを持たない 1 面で、設定と
// アンインストールはウィンドウに移っている。ソースは名前ではなく型 id と slot で探す。
// **この一式はテキストの値を 1 つも持たない**。押すコンポーネントとウィンドウのタイトルは ini の id で指し、run-window.mjs が
// 製品の ja-JP.ini (OBS 自身のテキストは OBS の ja-JP.ini) から引く。人への文に画面のテキストを入れるときは
// textRef のプレースホルダーで埋め、run-window.mjs が同じく引いて置き換える (表記が変わってもツールを修正せずに済む)。
// テキストの値を定数にコピーして、テストで ini と突き合わせる方式は採らない。テキストを 2 か所で持つことになり、
// ini を修正するたびにツールも修正する必要があるため。
//
// OBS 自身の ini のキー ([OK] [キャンセル] [適用]。製品は obs_frontend_get_locale_string で引く。
// メニューバーの [ツール] も) は "obs:" を先頭に付けた id で表す。
export const OBS_TEXT_PREFIX = "obs:";
export const OBS_TEXT_OK = OBS_TEXT_PREFIX + "OK";
export const OBS_TEXT_CANCEL = OBS_TEXT_PREFIX + "Cancel";
export const OBS_TEXT_APPLY = OBS_TEXT_PREFIX + "Apply";
// OBS のメニューバーの [ツール] (OBS 自身の ini のテキスト。ja「ツール(&T)」、en「&Tools」)。cordmux は [ツール] に
// 「cordmuxの設定」と「cordmuxをアンインストール...」の 2 項目を足す (plugin-main.cpp)。
export const OBS_TEXT_TOOLS_MENU = OBS_TEXT_PREFIX + "Basic.MainMenu.Tools";
// OBS 自身のテキストを引く ini の順。OBS は ja-JP.ini にないキーを en-US.ini のテキストで出す (ja-JP.ini は訳のあるキーだけを
// 持つ。31.1.1 と 32.2.2 の ja-JP.ini に OK= がなく、en-US.ini に OK="OK" がある)。製品も obs_frontend_get_locale_string
// で同じテキストを得る。ja-JP.ini だけを読むと obs:OK が引けず、結果のダイアログの [OK] を押す手順が
// 必ず失敗する。
export const OBS_LOCALE_FALLBACK_ORDER = ["ja-JP", "en-US"];

// OBS 自身のテキストを、ini の中身 (キー -> テキスト) の並びから前のものを優先して引く。どれにも無ければ null。
export function lookupObsText(textsInOrder, key) {
  for (const texts of textsInOrder || []) {
    const v = texts && texts[key];
    if (typeof v === "string" && v.length > 0) return v;
  }
  return null;
}

// Qt が読み上げの名前を作るときと同じく、テキストのアクセスキーのマーカー & を外す ("&&" は "&" 1 つ。Qt の qt_accStripAmp)。
// 例: OBS の ja「ツール(&T)」の読み上げの名前は「ツール(T)」、en「&Tools」は「Tools」。dialog-click.ps1 は読み上げの
// 名前と完全一致で探すので、ini のテキストのままでは見つからない。人への文にも画面に出る形 (& なし) を入れる。
// 製品の ini は & を 1 つも持たないので、製品のテキストには何も起きない。
// dialog-click.ps1 の側で & を無視して比べる方式は採らない。読み上げの名前に実際の & を持つコンポーネントと区別できなくなるため。
export function stripQtMnemonic(text) {
  if (typeof text !== "string") return text;
  return text.replace(/&(&|[^&])?/g, (whole, next) => next ?? "");
}

// ドック (src-obs/dock。1 面)。
export const TEXT_ID_DOCK_WINDOW_TITLE = "cordmux.dock.window.title";
// 下のバーの [...] (bottom_bar.cpp の menu_。読み上げの名前とツールチップがこのテキスト)。押すとドックのメニューが開く。
export const TEXT_ID_DOCK_MENU_BUTTON = "cordmux.dock.more_actions.a11y";
// ドックのメニューの項目 (dock_menus.cpp。5 項目のうちツールが押す 3 つ)。
export const TEXT_ID_MENU_SETTINGS = "cordmux.dock.menu_settings.menu";
export const TEXT_ID_MENU_EXPORT_DIAGNOSTICS = "cordmux.dock.menu_export_diagnostics.menu";
// メッセージの中のボタン (rules_dock.cpp)。
export const TEXT_ID_ADD_TO_VESKTOP = "cordmux.dock.add_to_vesktop.button";
export const TEXT_ID_REINSTALL = "cordmux.dock.reinstall.button";
// cordmux の録画のボタン (bottom_bar.cpp の record_。名前はボタンのテキストで、向きで変わる)。**行き先の名前で押す**:
// 行き先の名前がなく逆の名前があれば、既にその向きになっている。
export const TEXT_ID_RECORD_START = "cordmux.dock.record_start.button";
export const TEXT_ID_RECORD_STOP = "cordmux.dock.record_stop.button";
// 参加者の行 (person_row.cpp、row_menu.cpp)。行のチェックの読み上げの名前は人の見えている名前そのもの
// (person_row.cpp の check_->setAccessibleName(name))。[...] のツールチップと、メニューの「使う声」。
export const TEXT_ID_ROW_MORE_ACTIONS = "cordmux.dock.more_actions.a11y";
export const TEXT_ID_ROW_VOICE_FROM = "cordmux.dock.row_voice_from.menu";
export const TEXT_ID_ROW_OWN_VOICE = "cordmux.dock.row_own_voice.menu";
// 音声タブの人の行の「声」のつまみ (行の見出しは audio_voice_kind.label。値のテキストは row_volume_value.label の "%1%")。
// つまみを動かすために音声タブを開き、動かした後に参加者タブへ戻す。
export const TEXT_ID_TAB_AUDIO = "cordmux.dock.tab_audio.label";
export const TEXT_ID_TAB_PEOPLE = "cordmux.dock.tab_people.label";
// ドックの [状態] タブ (映像の受信の表と [映像のバッファ] がある)。メッセージの件数がないときのタブの名前で、件数があるときの名前もこれで始まる。
export const TEXT_ID_TAB_STATUS = "cordmux.dock.tab_status.label";
export const TEXT_ID_AUDIO_VOICE_KIND = "cordmux.dock.audio_voice_kind.label";
// 行のチェックの説明のテキスト (「枠に載せる」。人への文でチェックの名前として挙げる)。
export const TEXT_ID_ROW_CHECK = "cordmux.dock.row_check.a11y";
// メッセージの見出し (「出た / 消えた」をドックの文で判定する。%1 より前のテキストで当てる)。
export const TEXT_ID_MAIN_ACCOUNT_NOT_IN_CALL = "cordmux.dock.main_account_not_in_call.headline";
export const TEXT_ID_MAIN_ACCOUNT_NOT_IN_CALL_NO_NAME = "cordmux.dock.main_account_not_in_call_no_name.headline";
export const TEXT_ID_NO_VIDEO = "cordmux.dock.no_video.headline";
// ドックの 1 行目の状態 (Vesktop と接続しているが、通話に参加していない)。%1 を持たないので、テキストがそのまま文に出る。
export const TEXT_ID_SUMMARY_CONNECTED = "cordmux.dock.summary_connected.status";
export const TEXT_ID_VENCORD_OVERWRITTEN = "cordmux.dock.vencord_overwritten.headline";

// cordmux の設定ウィンドウ (settings_window.cpp。モードレスのウィンドウ。ページの一覧は QListWidget)。
export const TEXT_ID_SETTINGS_WINDOW_TITLE = "cordmux.settings.window.title";
export const TEXT_ID_SETTINGS_PAGE_ADVANCED = "cordmux.settings.advanced.page";
export const TEXT_ID_SETTINGS_PAGE_ABOUT = "cordmux.settings.about.page";
// [映像の受信] のページ (映像エンコーダとチューニングの欄)。映像のバッファの欄と、参加者ごとの届き方の表は、ドックの [状態] タブにある。
export const TEXT_ID_SETTINGS_PAGE_INCOMING_VIDEO = "cordmux.settings.incoming_video.page";
// [映像の受信] の「映像エンコーダ」と「チューニング」(どちらも QComboBox。入力欄の名前は buddy のラベルのテキスト)。
// 選ぶ項目は ComboBox の中の項目のテキスト (option のテキスト)。
export const TEXT_ID_SETTINGS_VIDEO_ENCODER = "cordmux.settings.video_encoder.label";
export const TEXT_ID_SETTINGS_VIDEO_ENCODER_SOFTWARE = "cordmux.settings.video_encoder_software.option";
export const TEXT_ID_SETTINGS_ENCODE_PRIORITY = "cordmux.settings.encode_priority.label";
export const TEXT_ID_SETTINGS_ENCODE_PRIORITY_QUALITY = "cordmux.settings.encode_priority_quality.option";
// ドックの [表示] タブ (dock_tabs.cpp の DockTabBar。タブの名前は QTabBar のタブのテキスト)。変えた値はすぐ反映される。
export const TEXT_ID_DOCK_TAB_DISPLAY = "cordmux.dock.tab_display.label";
// ドックの上部のメインの行の [変更] (押すと、通話中の人と過去の参加者のメニューが開く。項目は人の見えている名前)。
export const TEXT_ID_DOCK_ACCOUNT_CHANGE = "cordmux.dock.account_change.button";
// [表示] タブの「自分の枠に映すソース」(QComboBox。入力欄の名前は buddy のラベルのテキスト)。選ぶ項目は ComboBox の中の項目のテキスト。
export const TEXT_ID_LOOK_SELF_FRAME_SOURCE = "cordmux.look.self_frame_source.label";
// [表示] タブの「名前」のまとまりの [表示] (名前ラベルの見た目の設定を変えた後の確認に使う)。
// [表示] はほかのまとまりにもあるので、製品はアクセシブル名を見出しを添えた形 (TEXT_ID_LOOK_FIELD_IN_GROUP の
// "%1 (%2)"。look_tab.cpp の field_in_group_name) にしている。押すコンポーネントはその名前で指す (uiClickStep の group)。
export const TEXT_ID_LOOK_SHOW = "cordmux.look.show.check";
export const TEXT_ID_LOOK_HEADING_NAME = "cordmux.look.heading_name.heading";
export const TEXT_ID_LOOK_FIELD_IN_GROUP = "cordmux.look.field_in_group.a11y";
// [表示] タブの「ダミー映像を表示」(QCheckBox。押すとすぐ反映される)。
export const TEXT_ID_LOOK_SHOW_PLACEHOLDERS = "cordmux.look.show_placeholders.check";
// [バージョン情報] の [cordmux をアンインストール...]。
export const TEXT_ID_SETTINGS_UNINSTALL = "cordmux.settings.uninstall.button";
// OBS の [ツール] メニューの「cordmuxをアンインストール...」(plugin-main.cpp。上のボタンと同じテキストなので、押すコンポーネントの
// 種類 (MenuItem と Button) と探すウィンドウ (メニューと設定ウィンドウ) で見分ける)。押すと設定ウィンドウが [バージョン情報] で開き、
// 押せる状態なら確認のダイアログがその上に出る。
export const TEXT_ID_TOOLS_MENU_UNINSTALL = "cordmux.settings.tools_menu_uninstall.menu";

// 確認と結果のダイアログ (install_dialog_plan.cpp。確認と結果は同じタイトル)。
export const TEXT_ID_DIALOG_UNINSTALL_TITLE = "cordmux.dialog.uninstall.title";
export const TEXT_ID_DIALOG_UNINSTALL_BUTTON = "cordmux.dialog.uninstall.button";
export const TEXT_ID_DIALOG_OPEN_FOLDER = "cordmux.dialog.open_folder.button";
// 別の Vencord からの切り替えの確認のダイアログ。
export const TEXT_ID_DIALOG_VESKTOP_ADD_TITLE = "cordmux.dialog.vesktop_add.title";
export const TEXT_ID_DIALOG_SWITCH_BUTTON = "cordmux.dialog.switch.button";
// 診断情報のエクスポートの保存ダイアログのタイトル (dock_poll_export.cpp の QFileDialog::getSaveFileName)。
export const TEXT_ID_DIALOG_EXPORT_TITLE = "cordmux.dialog.export.title";

// cordmux の録画の完成ファイルの名前に入る種類の語 (%1 %2 %3 の %2)。録画ログに録画ファイルの作成の行がないときの探し方に使う。
export const TEXT_ID_FILE_SCREEN_SHARE = "cordmux.source.file_screen_share.label";
export const TEXT_ID_FILE_VOICE = "cordmux.source.file_voice.label";
export const TEXT_ID_FILE_NAME_FORMAT = "cordmux.source.file_name.label";

// 人への文 (human の message) や obs-websocket のリクエストの中に画面のテキストを埋めるプレースホルダー。run-window.mjs が
// expandTextRefsDeep で置き換える。
export function textRef(id) {
  return "{{" + id + "}}";
}
const TEXT_REF_PATTERN = /\{\{([^{}]+)\}\}/g;

// 文の中のプレースホルダーの id を並べる (重ねない)。
export function textRefIds(text) {
  const ids = [];
  for (const m of String(text ?? "").matchAll(TEXT_REF_PATTERN)) {
    if (!ids.includes(m[1])) ids.push(m[1]);
  }
  return ids;
}

// 文の中のプレースホルダーを lookup(id) のテキストで置き換える。引けなかった id は missing に並べ、プレースホルダーはそのまま残す
// (空にはしない。dry-run と実行のログで見える)。
export function expandTextRefs(text, lookup) {
  const missing = [];
  const out = String(text ?? "").replace(TEXT_REF_PATTERN, (whole, id) => {
    const value = lookup(id);
    if (typeof value === "string" && value.length > 0) return value;
    if (!missing.includes(id)) missing.push(id);
    return whole;
  });
  return { text: out, missing };
}

// 値 (文字列・配列・入れ子のオブジェクト) の中の文字列のプレースホルダーを全部置き換える。元の値は変えない。
export function expandTextRefsDeep(value, lookup) {
  const missing = [];
  const walk = (v) => {
    if (typeof v === "string") {
      const r = expandTextRefs(v, lookup);
      for (const id of r.missing) if (!missing.includes(id)) missing.push(id);
      return r.text;
    }
    if (Array.isArray(v)) return v.map(walk);
    if (v && typeof v === "object") {
      const out = {};
      for (const [k, x] of Object.entries(v)) out[k] = walk(x);
      return out;
    }
    return v;
  };
  return { value: walk(value), missing };
}

// 手順 1 つが引く画面のテキストの id の全部 (dry-run が ini と突き合わせる)。id を指定する欄 (textId / titleTextId /
// buttonTextId / sceneTextId)、判定のテキスト (names.textPrefix。1 つか配列)、どの欄の中のプレースホルダーも (message、requestData、
// expect ほか。欄を指定して拾うと、プレースホルダーを足した新しい欄を見落とすので、手順の全部を文字列にして拾う)。
export function stepTextIds(s) {
  const ids = [];
  const add = (id) => {
    if (typeof id === "string" && id && !ids.includes(id)) ids.push(id);
  };
  if (!s || typeof s !== "object") return ids;
  add(s.textId);
  add(s.titleTextId);
  add(s.buttonTextId);
  add(s.sceneTextId);
  add(s.selectTextId);
  // judge-rt の入力のうち、テキストを ini から引く物 (names のほか、バージョンの表の labels / reference / unavailable)。
  for (const source of Object.values((s && s.inputs) || {})) {
    const prefix = source && typeof source === "object" ? source.textPrefix : undefined;
    for (const id of Array.isArray(prefix) ? prefix : [prefix]) add(id);
  }
  for (const id of textRefIds(JSON.stringify(s))) add(id);
  return ids;
}

// cordmux のソースの型 id (src-obs/sources/scene_names.h。利用者に見えない内部の識別子で、変えない)
// と、枠の番号のキー (kSlotSettingKey)。
export const CORDMUX_SOURCE_KINDS = {
  composite: "cordmux_composite",
};
export const CORDMUX_SLOT_SETTING_KEY = "slot";
// 枠の番号を持たないソース (合成のソース。指定は種類だけ)。
const SLOTLESS_KINDS = ["composite"];

// %1 から %9 を args の値で 1 回の走査で置き換える (scene_names.h の fill_template と同じ。人の名前に "%2" が
// 入っていても 2 度置き換えない)。
export function fillTemplate(template, args) {
  return String(template ?? "").replace(/%([1-9])/g, (whole, n) => {
    const i = Number(n) - 1;
    return i < (args || []).length ? String(args[i]) : whole;
  });
}

// 人が載った枠の名前 (ini の「%1 (cordmux画面共有 %2)」) から、slot の枠に載っている人の名前を読む。
// 形に合わなければ null (空きの枠、利用者が付けた名前)。scene_names.h の slot_name_matches と同じ分け方。
export function personFromSlotName(name, slot, personForm) {
  const filled = fillTemplate(personForm, ["%1", String(slot)]);
  const at = filled.indexOf("%1");
  if (!personForm || at < 0 || filled.indexOf("%1", at + 2) >= 0) return null;
  const prefix = filled.slice(0, at);
  const suffix = filled.slice(at + 2);
  const text = String(name ?? "");
  if (text.length <= prefix.length + suffix.length || !text.startsWith(prefix) || !text.endsWith(suffix)) return null;
  return text.slice(prefix.length, text.length - suffix.length);
}

// obs-websocket の GetInputList の入力と、型 id が cordmux の物について GetInputSettings で読んだ slot から、
// cordmux のソースの一覧を作る。slots は inputName -> 設定 (inputSettings) のコピー。personForms は ini の
// 画面共有と声の枠の名前のテキスト ({ screen, voice })。
// 返す物: [{ inputName, inputUuid, kind: "composite", slot: null, person: null }] (現在の型は枠の番号を持たない)
export function classifyCordmuxInputs({ inputs, settings, personForms }) {
  const byKind = Object.fromEntries(Object.entries(CORDMUX_SOURCE_KINDS).map(([k, v]) => [v, k]));
  const out = [];
  for (const input of inputs || []) {
    const kind = byKind[input && input.inputKind];
    if (!kind) continue;
    const s = (settings || {})[input.inputName] || {};
    const raw = s[CORDMUX_SLOT_SETTING_KEY];
    const slot = Number.isInteger(raw) && raw > 0 ? raw : null;
    const form = personForms && personForms[kind];
    const person = slot !== null && form ? personFromSlotName(input.inputName, slot, form) : null;
    out.push({ inputName: String(input.inputName), inputUuid: input.inputUuid || null, kind, slot, person });
  }
  return out;
}

// 手順が名指すソース ({ kind, slot }。自分の枠は slot を持たない) を一覧から探す。見つからなければ null。
export function findCordmuxInput(list, ref) {
  if (!ref || !ref.kind) return null;
  const hits = (list || []).filter((x) => x.kind === ref.kind && (SLOTLESS_KINDS.includes(ref.kind) || x.slot === ref.slot));
  return hits.length > 0 ? hits[0] : null;
}

// 手順の説明に出す、指すソースの呼び方 (内部のテキスト。画面のテキストではない)。
export function sourceRefLabel(ref) {
  if (!ref) return "?";
  return SLOTLESS_KINDS.includes(ref.kind) ? CORDMUX_SOURCE_KINDS[ref.kind] : CORDMUX_SOURCE_KINDS[ref.kind] + " slot " + ref.slot;
}

// テスト用の送信側の参加者の、画面に見える名前 (test/tools/harness/sender.ts の合成の userId。名簿がないので userId がそのまま
// 名前になる)。参加者の行のチェックの読み上げの名前に当たる。
export function harnessPersonName(kind, n) {
  return "harness-" + (kind === "voice" ? "voice" : "screen") + "-" + n;
}

// scenario "select-four" (4 人を選んで 4 分割が出る確認、自動配置の確認)。
// 見積もり (実機の実行で測って見直す前提): 接続待ち (最大 15 秒) +
// 一覧に 4 人が並ぶまでの 5 秒 + dock-click x4 + 自動配置 (既定 ON) の周期が 4 人ぶん
// 収束するまでの 5 秒 + screenshot-window + 4 ストリームぶんの screenshot-source +
// GetSceneItemList の往復。dock-view の connected/degraded と同じ桁 (150 秒) を見込む。
export const SELECT_FOUR_N_SCREEN = 4;
export const SELECT_FOUR_N_VOICE = 0; // 声は載せない (参加者一覧の行を画面 4 つだけにする)
export const SELECT_FOUR_HARNESS_SECONDS = 150;
export const SELECT_FOUR_CONNECT_WAIT_TIMEOUT_MS = 15000;
// 5 秒待ってから撮る。
export const SELECT_FOUR_SETTLE_WAIT_MS = 5000;
// scenario "dummy" (4 分割の確認: ダミー表示)。5 秒待ってから撮る。
export const DUMMY_SETTLE_WAIT_MS = 15000; // 5 秒では色タイルが撮れなかったので 15 秒に延ばした

// scenario "export-bundle" (診断情報の zip の確認)。
// 保存ダイアログへ入力するファイル名 (見積もり。ダイアログとしか決まっていない)。
export const EXPORT_BUNDLE_FILE_NAME = "cordmux-bundle-export.zip";
// 見積もり (実機の実行で測って見直す前提): メニューの [診断情報をエクスポート...] を選んでから保存ダイアログが開くまでの見込み
// (dialog-sendkeys.ps1 がウィンドウを探す上限)。製品は中身を別のスレッドで zip にまとめ終えてから
// 保存ダイアログを開く (dock_poll_export.cpp)。zip の中身は数十 KB 程度なので
// まとめること自体は速いはずだが、余裕を見て 10 秒にした。
export const EXPORT_BUNDLE_DIALOG_TIMEOUT_MS = 10000;
// 5 秒待って zip があることを確認する。
export const EXPORT_BUNDLE_SETTLE_WAIT_MS = 5000;

// --step second-obs (2 つ目の OBS とシーンコレクションの切り替え)。
// シーンコレクション名は seed-config.py が作る 2 つの名前と揃える (このファイルは Node の API を呼ばないので、
// 名前を合わせるだけで、実際の生成は seed-config.py の役目のまま。名前が食い違うと --step second-obs が実行時に気付ける)。
export const SECOND_OBS_MAIN_SCENE_COLLECTION = "test";
export const SECOND_OBS_EMPTY_SCENE_COLLECTION = "test-empty";
// 見積もり (実機の実行で測って見直す前提): テスト用の送信側が実際に接続するまで
// 待つ上限。エンコーダの直列のラダー確保 + ウォームアップ 60 フレーム + electron の起動オーバーヘッドを
// 見込んで 15 秒とした (sender.ts はこのラダー確保が終わってから socket を開くので、
// この待ちの間は SECOND_OBS_HARNESS_SECONDS の秒数はまだ数え始めていない)。
const SECOND_OBS_CONNECT_WAIT_TIMEOUT_MS = 15000;
// 見積もり (実機の実行で測って見直す前提): 切り替えをまたいでテスト用の送信側を
// 動かしておくのに要る秒数 (この秒数は接続確立の直後から数え始めるので、上の待ちの分は
// 含めなくてよい)。5 秒の保留 + 復帰後 10 秒の録画 + obs-websocket の往復の余裕を足して
// 30 秒とした。
const SECOND_OBS_HARNESS_SECONDS = 30;
const SECOND_OBS_SWITCH_HOLD_MS = 5000;
const SECOND_OBS_POST_SWITCH_RECORD_SEC = 10;

// --step discord-first (Discord を先に起動する場合)。見積もり
// (実機の実行で測って見直す前提): OBS を起動するまでの遅れを 5 秒とした。
// テスト用の送信側の全長 (45 秒) は 5 秒の遅れ + OBS の起動と cordmux モジュールの初期化 +
// backoff.ts の再試行間隔 (0/2/4/5 秒) + 接続後に映像が乗るのを録る猶予
// (8 秒) を見込んだ見積もり。
export const DISCORD_FIRST_OBS_DELAY_MS = 5000;
const DISCORD_FIRST_SOURCES_WAIT_MS = 4000; // 枠のソースができるまでの猶予 (実測して足した値)
const DISCORD_FIRST_HARNESS_SECONDS = 45;
const DISCORD_FIRST_RECORD_WAIT_SEC = 8;

// LOCALAPPDATA の差し替え先。リポジトリ直下からの相対パス。
export const LOCAL_APPDATA_OVERRIDE_REL = "_work/localappdata";
// %LOCALAPPDATA%\cordmux\session.json を上の差し替え後で見るときの相対パス。
export const SESSION_JSON_REL = LOCAL_APPDATA_OVERRIDE_REL + "/cordmux/session.json";
export const HANDSHAKE_DIR_REL = LOCAL_APPDATA_OVERRIDE_REL + "/cordmux";

// APPDATA の差し替え先。LOCALAPPDATA と同じ形でリポジトリ直下からの相対パス。Vesktop の state.json の場所は、製品が
// 環境変数 VENCORD_USER_DATA_DIR (Vesktop 自身の上書きの手段。製品は APPDATA を読まない) で決めるので、実行スクリプトは
// VENCORD_USER_DATA_DIR をここの下の Vesktop (VESKTOP_DATA_DIR_REL) に向ける。APPDATA も同じ場所に差し替えるのは、
// テスト用の送信側 (Electron の userData) と、APPDATA の下を読む道具が、利用者の実際のフォルダーに触れないようにするため。
// 利用者の実際の %APPDATA%\Vesktop には触れない。
export const APPDATA_OVERRIDE_REL = "_work/appdata";
// 差し替えた Vesktop の設定のフォルダー (VENCORD_USER_DATA_DIR の値)。
export const VESKTOP_DATA_DIR_REL = APPDATA_OVERRIDE_REL + "/Vesktop";
export const VESKTOP_STATE_JSON_REL = APPDATA_OVERRIDE_REL + "/Vesktop/state.json";
// seed-config.py が --dock-install ours/missing で使う、Vencord のビルド成果 (dist) の保存先
// (vencord-plugin.yml の pnpm build --standalone --disable-updater が作る dist/ をここへコピーする)。
export const VENCORD_DIST_REL = "_work/vencord-dist";

// --dock-install の値。state.json の vencordDir が elsewhere (別の場所を指す) / missing (同梱の場所を指すが
// 4 ファイルがない = 上書きされて消えている) / ours (同梱の場所を指し 4 ファイルがある) / absent (vencordDir の
// キーがない) のどれかを、seed-config.py が --appdata の配下に作る。既定は elsewhere
// (Vesktop が cordmux の入る前の Vencord を読んでいる見え方。APPDATA を差し替えても、実際の state.json を 1 度も書かない形を守る)。
export const DOCK_INSTALL_VALUES = ["elsewhere", "missing", "ours", "absent"];
export const DEFAULT_DOCK_INSTALL = "elsewhere";

// --assign-mode の値。seed-config.py が
// <obs>/config/obs-studio/plugin_config/cordmux/settings.json の assign_mode に書く。
// **既定は manual = 製品の既定と同じ** (実行スクリプトが製品の既定を変えて実行すると、実行で見たものが
// 利用者の見るものと違う)。auto を渡した実行だけが「共有を始めた人を上限まで」になる。
export const DOCK_ASSIGN_MODE_VALUES = ["manual", "auto"];
export const DEFAULT_DOCK_ASSIGN_MODE = "manual";

// --dock-scenario degraded がテスト用の送信側へ渡す長辺の上限。HARNESS_FAIL_STAGE=3 は degraded という
// 状態自体の定義。HARNESS_MAX_LONG_EDGE を渡すと上限が付き、--dock-max-long-edge で変えられる。
// 640 という値は見積もり。
// 0 を渡すと launchHarness (run-window.mjs) の真偽判定で HARNESS_MAX_LONG_EDGE を送らなくなる
// (fpsDivisor 等と同じ「0 は既定 (縮小なし) の意味」という既存の扱いに揃えた)。
export const DEFAULT_DOCK_MAX_LONG_EDGE = 640;

// --verbose: cordmux の 10 秒ごとの stats と facts の行、送信側 ([cordmux/js]) の行は LOG_DEBUG で出る。
// OBS は --verbose で起動したときだけ LOG_DEBUG の行をログのファイルに書く
// (OBS 31.1.1 frontend/obs-main.cpp の do_log)。実機テストのツールはそれらの行を OBS のログから読む (parseStatsVideoDropped・
// parseVideoRateSamples・summarize-window.py・notice_count.py の timing) ので、実行では付ける。利用者の OBS には付かない。
export const OBS_LAUNCH_ARGS = ["--portable", "--multi", "--disable-shutdown-check", "--disable-missing-files-check", "--disable-updater", "--verbose"];
// obsws.mjs (run-window.mjs が OBS を操作するための obs-websocket) の待ち受けポート番号。seed-config.py が
// ポータブル OBS の設定にこの番号で認証なしの待ち受けを書く (4460 固定)。
// cordmux 自身の待ち受け (4470 から 16 個。harness が session.json 経由で使う) とは別物。
export const OBSWS_CONTROL_PORT = 4460;

// ---- 4 アカウントの通話 (call-1) の値 ---------------------------------------

// デコーダーの種別 (src-obs/config/settings.h の decoder。利用者の語で持ち、API の名前を設定に出さない)。
// software と hardware の 2 通りで測り、切り替えは OBS の再起動で行う
// (decoder は module load で 1 回読むだけ)。seed-config.py が settings.json へ書く。
export const DECODER_VALUES = ["software", "hardware"];
export const DEFAULT_DECODER = "software";

// call-1 の定常状態の長さ (分)。--call-minutes で切り替える (software で 20 分 -> OBS を再起動 -> hardware で
// 10 分)。既定は software の 20 分。
export const DEFAULT_CALL_MINUTES = 20;

// 実際の %APPDATA% 配下の Vesktop の state.json (--real-profile のときだけ触る。差し替え後を
// 見る VESKTOP_STATE_JSON_REL とは別物)。パスの解決は run-window.mjs の役目で、ここは
// %APPDATA% からの相対だけを持つ。
export const VESKTOP_STATE_JSON_REAL_REL = "Vesktop/state.json";
// call-gate-a が結果フォルダーへコピーする実際の state.json (開始ゲートの前半)。call-gate-b は別の
// プロセスとして走るので、同じ --out を渡してこの名前で読み直す (step を分割しているため)。
export const STATE_JSON_BEFORE_NAME = "state-json-before.json";
// テスト用にインストールしたファイルが Vesktop に見せる場所 (seed-config.py の bundled_vencord_dirs と同じ
// <obs>/plugins/cordmux/data/vencord)。ゲートの判定はここを指しているかを見る。
export const BUNDLED_VENCORD_REL = "plugins/cordmux/data/vencord";
// 4 ファイルだけの形で「有効」かをゲートの後半で見るために退避するファイル。
export const BUNDLED_VENCORD_PACKAGE_JSON_REL = BUNDLED_VENCORD_REL + "/package.json";
// 製品がバックアップを書く場所。ポータブル OBS では obs_module_config_path が
// <obs>/config/obs-studio/plugin_config/cordmux/ を指す (src-obs/dock/dock_bind.cpp の
// module_config_file_path)。cleanup.ps1 -CallWindow の -ObsDir から同じ場所を組み立てる。
export const PLUGIN_CONFIG_REL = "config/obs-studio/plugin_config/cordmux";
export const VENCORD_DIR_BACKUP_REL = PLUGIN_CONFIG_REL + "/vencord_dir_backup.json";
// 実行スクリプトだけが知る値 (一時の Vencord のビルド・ブラウザのプロファイル・共有の中身に使った
// 一時フォルダー) を置く 1 ファイル。リポジトリ直下からの相対パス。
export const CALL_WINDOW_ARTIFACTS_REL = "_work/call-window-artifacts.json";

// obs-stats: OBS 自身のフレームの欠落。GetStats を record-start の 15 秒後と
// record-stop の直前の 2 回取り、差分の割合で判定する。しきい値は 0.02%。
export const OUTPUT_SKIPPED_RATIO_THRESHOLD = 0.0002;
export const OBS_STATS_FIRST_WAIT_MS = 15000;

// call-1 は声 4 + 画面 3 + 自分の枠 1。
// 画面の枠は自分の枠を入れて 4 つなので、枠への出し入れも 4 で数える。
export const CALL_SCREEN_COUNT = 4;
export const CALL_VOICE_COUNT = 4;
// call-1 の枠の数と声の数は --n-screen / --n-voice で上書きできる: 4 アカウントの通話では
// 自分以外の共有は 3 つ (メイン・Chrome・Firefox のアカウント) なので、実際の実行では 3 を渡す。渡さなければ 4 (テスト用の送信側での
// リハーサルの形)。--step の既定 (nScreen 4 / nVoice 8) とは別に持つ。
export function callScreens(args) {
  return args && args.nScreenGiven ? args.nScreen : CALL_SCREEN_COUNT;
}
export function callVoices(args) {
  return args && args.nVoiceGiven ? args.nVoice : CALL_VOICE_COUNT;
}
// 自分の枠は画面共有の枠とは別のソース (型 id cordmux_mirror_source。slot を持たない) なので、
// 「自分の枠を含む」を見るには別に 1 枚撮る必要がある。

// SetInputVolume で 200% とミュートをストリームごとに往復する。
// 見積もり (実機の実行で測って見直す前提): 1 区間 3 秒。録画の波形で区間を
// 見分けられる長さとして、判定ツールが扱う最小の長さ (frames.py のフリーズ判定の時間幅) より十分長い値にした。
export const AC32_SEGMENT_MS = 3000;

// 見積もり (実機の実行で測って見直す前提): ゲートでテストを実行する人が Vesktop を開き直して
// から送信側が接続してくるまでの待ちの上限。人の操作 (開き直し) を挟むので、テスト用の送信側の
// 15 秒ではなく 3 分にした。call-1 の接続待ち (下の 20 分) より短いのは、ゲートは人が
// 目の前で開き直す場面だからである。
export const CALL_GATE_CONNECT_WAIT_TIMEOUT_MS = 180000;
// 通話を始めてから 20 分たっても接続しないなら実行を打ち切る条件。
export const CALL_CONNECT_WAIT_TIMEOUT_MS = 20 * 60 * 1000;
// 見積もり: ボタンを押してから state.json が書き終わるまでの猶予 (ドックのボタンの処理は
// 同期だが、書き込みは押した後の処理なので少し待つ)。
export const CALL_GATE_WRITE_SETTLE_MS = 3000;
// 見積もり: 接続してからドックの 1 行目が更新されるまでの猶予。
// ドックの更新の周期 (settings.h の dock_poll_ms 既定 200 ms) の何周期ぶんかに余裕を足した値。
export const CALL_GATE_SHOT_SETTLE_MS = 3000;

// ---- 録画のテストの値 -----------------------------------------------------

// cordmux の録画のボタンは TEXT_ID_RECORD_START / TEXT_ID_RECORD_STOP (向きで名前が変わる。行き先の名前で押す)。

// セッションのフォルダーの中の並び: 直下に完成したファイル、中間ファイル (録画ログ・区間・黒) は data/ の下。
// 古い並びの録画 (録画ログが直下、完成が <表示名>_screen.mov) は「中間ファイルのフォルダー = セッションのフォルダー」と読む
// (ledger_check.py の materials_dir_of と同じ読み分け)。
export const RECORD_MATERIALS_DIR_NAME = "data";
export const LEGACY_SCREEN_OUTPUT_SUFFIX = "_screen.mov";
export function recordMaterialsDirOf(sessionDir, hasMaterialsDir) {
  return hasMaterialsDir ? String(sessionDir).replace(/[\\/]+$/, "") + "/" + RECORD_MATERIALS_DIR_NAME : String(sessionDir);
}

// セッションのフォルダー直下のファイル名のうち、画面共有の完成ファイルに当たる物 (録画ログに録画ファイルの作成の行がないときの探し方)。
// 現在のバージョンの名前は ini の書式 (TEXT_ID_FILE_NAME_FORMAT の「%1 %2 %3」= 名前・種類の語・録画の日時) に種類の語
// (TEXT_ID_FILE_SCREEN_SHARE) を入れた形、古い並びは <表示名>_screen.mov。format か screenWord が無ければ古い並びの形だけ。
export function screenOutputFileNames(fileNames, format, screenWord) {
  const out = [];
  for (const raw of fileNames || []) {
    const name = String(raw);
    if (!name.toLowerCase().endsWith(".mov")) continue;
    if (name.endsWith(LEGACY_SCREEN_OUTPUT_SUFFIX)) {
      out.push(name);
      continue;
    }
    if (!format || !screenWord) continue;
    const base = name.slice(0, -4);
    const parts = fillTemplate(format, ["\u0001", screenWord, "\u0001"]).split("\u0001");
    let pos = 0;
    let ok = base.startsWith(parts[0]);
    pos = parts[0].length;
    for (let i = 1; ok && i < parts.length; i += 1) {
      const at = i === parts.length - 1 ? (parts[i] === "" ? base.length : base.lastIndexOf(parts[i])) : base.indexOf(parts[i], pos + 1);
      if (at < pos + 1) ok = false;
      else pos = at + parts[i].length;
    }
    if (ok && pos === base.length) out.push(name);
  }
  return out;
}

// cordmux の録画の保存先 (seed-config.py が settings.json の record_dir へ必ず書く先)。
// **実行の出力フォルダーからの相対**で、run-window.mjs が絶対パスにする。
export const RECORD_DIR_REL = "recordings";

// 録画ごとのフォルダー (<開始日時>) を並べるフォルダー。製品は、設定の record_dir に入っている場所そのものにこのフォルダーを作る
// (src-obs/record/record_location.h の choose_record_base_dir)。
// seed-config.py が record_dir に書く先 (RECORD_DIR_REL) と、ツールが録画を探す先は、この関数で 1 つにそろえる。
export function recordSessionsRoot(recordDirAbs) {
  return recordDirAbs;
}

// 録画ごとのフォルダーの名前か。保存先の直下には、録画のほかのフォルダー (call-1 が OBS の録画を移す obs-<tag> など) も並ぶので、
// 日時の名前だけを録画のフォルダーとして数える (src-obs/record/record_segment.h の is_session_dir_name と同じ形。古い並びの
// 「年月日-時分秒」の形と、同じ秒の 2 つ目からの「 (2)」付きの名前も含む)。
export function isRecordSessionDirName(name) {
  return /^(\d{4}-\d{2}-\d{2} \d{2}-\d{2}-\d{2}|\d{8}-\d{6})( \(\d+\))?$/.test(name);
}

// テスト用の送信側を 3 回起動する (2 回の再接続)。最初の 1 回だけが長く、
// 実行中に解像度を 2 回変える (解像度が 2 回変わり再接続が数回ある録画を作るため)。
// 見積もり: 秒数とスケジュールは実測ではない。最初の 1 回は
// 「接続 + 安定 + 途中 ON + 解像度 2 回 + それぞれの後の定常」が収まる長さにし、
// 2 回目以降は「新しいエンコーダで数秒ぶんの区間ができる」だけの長さにした。
export const RECORD_HARNESS_FIRST_SECONDS = 90;
export const RECORD_HARNESS_RECONNECT_SECONDS = 30;
export const RECORD_RECONNECT_COUNT = 2;
// 再起動した後にもう 1 回接続する長さ。**これがないと「再起動後は録画が OFF に戻っている」ことの判定が空になる**:
// 区間は最初のキーフレームが来るまで 1 バイトも開かないので、テスト用の送信側が
// 1 つも接続していなければ、録画が ON でもフォルダーは 1 つも増えない。接続したうえで増えない
// ことを見て初めて「OFF に戻っている」と言える。
export const RECORD_AFTER_RESTART_SECONDS = 25;
// 同じ OBS の起動の中での 2 回目の録画の長さ。1 回目のセッションにいた人の区間がまた開くまでが要るだけなので、
// 繋ぎ直しより短くした (見積もり)。
export const RECORD_SECOND_SESSION_SECONDS = 20;

// 実行中の解像度の変更のスケジュール。テスト用の送信側の起動からの秒数。
// 見積もり: 解像度は 1280x720 と 640x360 (どちらも 16:9 で、既定の 1280x720 から
// 長辺の上限 640 まで下げる道筋。長辺の上限の既定と揃えた)。
export const RECORD_RESOLUTION_SCHEDULE = [
  { atSec: 35, width: 1280, height: 720 },
  { atSec: 60, width: 640, height: 360 },
];

// テスト用の送信側が接続するのを待つ上限 (dock-view と同じ 15 秒)。
export const RECORD_CONNECT_WAIT_TIMEOUT_MS = 15000;
// 見積もり: 接続してから録画を ON にするまでの猶予。**ここで実際の映像を貯めることが
// 「途中 ON」の形そのもの**なので、0 にはできない。ドックの更新の周期
// (500 ms) の何周期ぶんかでは足りず、区間が 1 つ閉じる程度の長さが要るので 10 秒にした。
export const RECORD_SETTLE_BEFORE_ON_MS = 10000;
// 見積もり: ON にしてからの猶予 (最初のキーフレームが届き、
// 区間のファイルが開くまで)。
export const RECORD_SETTLE_AFTER_ON_MS = 8000;
// 見積もり: 再接続の合間の猶予 (前のプロセスが完全に終了してから次を起動する)。
export const RECORD_RECONNECT_GAP_MS = 4000;

// 空き容量の警告を短い録画でも出すための値。settings の
// record_space_check_interval_s と record_space_warn_minutes をここで上書きする。
// 既定 (300 秒 / 30 分) のままだと、数分の録画では一度も確認せず、警告が出ない。
// 見積もり: 周期は 5 秒 (ドックの更新より十分遅く、録画の間に何度も確認する)。
// しきい値は settings の上限 1440 分 = 24 時間ぶん (config/settings.h の
// kRecordSpaceWarnMaxMinutes)。24 時間ぶんの空きがあるディスクは事実上ないので必ず出る。
export const RECORD_SPACE_CHECK_INTERVAL_S = 5;
export const RECORD_SPACE_WARN_MINUTES = 1440;

// 録画ファイルの作成 (録画ログの finalize の行) を待つ上限。ProRes のエンコードは実時間の
// 1 倍から 4.2 倍かかった実測に、この録画の長さ (約 3 分) を掛けた見積もり。
// 4.2 倍の 3 分 = 約 13 分に余裕を足して 15 分にした。
export const RECORD_FINALIZE_WAIT_TIMEOUT_MS = 15 * 60 * 1000;
// 録画ファイルの作成の待ちが確認する周期 (見積もり: 録画ログはファイルなので、秒の単位で十分)。
export const RECORD_FINALIZE_POLL_MS = 2000;

// 枠は 2x2 (画面 4 つ)。声も 4 つ (4 画面 + 声 4)。
export const RECORD_N_SCREEN = 4;
export const RECORD_N_VOICE = 4;

// 2 つの候補。同じ録画から両方を作って両方を開く。
export const RECORD_PCM_VALUES = ["fl32", "in24"];

// テスト用の送信側へ渡す解像度のスケジュールの書き方 (時刻のスケジュールを引数で渡す)。
// "35:1280x720,60:640x360" の形。**この書き方を決めるのは実行スクリプトの側**で、読むのは
// テスト用の送信側 (test/tools/harness) である。
export function resolutionScheduleArg(schedule) {
  return (schedule || [])
    .map((s) => String(s.atSec) + ":" + String(s.width) + "x" + String(s.height))
    .join(",");
}

// 上の書き方を読み戻す (実行スクリプトの中だけで使う。往復をテストで確認するため)。
export function parseResolutionSchedule(text) {
  const out = [];
  for (const part of String(text || "").split(",")) {
    const s = part.trim();
    if (!s) continue;
    const m = /^(\d+):(\d+)x(\d+)$/.exec(s);
    if (!m) return null;
    out.push({ atSec: Number(m[1]), width: Number(m[2]), height: Number(m[3]) });
  }
  return out;
}

// ---- 配布・インストール・更新・アンインストールのテストの値 -------------------------------------
// step・場面・置き方・install_snapshot.py・_work/rt-<tag>/ の名前は dist/README.md に書かれた名前と揃える。
// **新しい文字列リテラルは ASCII にした** (文字列リテラルに日本語を書かない規則)。ドックとダイアログの
// 押せるコンポーネントは ini の id で持ち、run-window.mjs が製品の ja-JP.ini から文言を引く (同じ文言を
// 2 か所で決めない)。

export const INSTALL_ROUNDTRIP_STEP = "install-roundtrip";
// 場面 (名前は dist/README.md の表)。失敗する場合は "-" の後ろに別の名前を付ける。
export const ROUNDTRIP_SCENARIO_VALUES = [
  "roundtrip",
  "move",
  "move-foreign",
  "force-update-missing",
  "force-update-tamper",
  "update",
  "update-while-running",
  "other-vencord",
  "other-vencord-cancel",
  "motw",
  "dll-clash",
  "old-obs",
  "newer-libobs",
];
export const DEFAULT_ROUNDTRIP_SCENARIO = "roundtrip";
// plugins は手順書のポータブル版の OBS の置き方 (OBS のフォルダーの plugins\cordmux。--obs-zip に 33 以上の zip を渡す)、
// programdata はインストール版の置き方 (ProgramData の obs-studio\plugins\cordmux。VM だけ)。
export const ROUNDTRIP_LAYOUT_VALUES = ["plugins", "programdata"];
export const DEFAULT_ROUNDTRIP_LAYOUT = "plugins";
// 前のバージョンの zip (--old-zip) が要る場面と、テスト用の DLL (--test-dll) が要る場面。
export const ROUNDTRIP_SCENARIOS_WITH_OLD_ZIP = ["update", "update-while-running"];
export const ROUNDTRIP_SCENARIOS_WITH_TEST_DLL = ["newer-libobs"];
// --layout programdata を受け付ける場面 (VM の往復だけ。他の場面はポータブルの OBS が前提:
// move はポータブルの OBS をフォルダーごと移す、old-obs / newer-libobs はバージョンの違う zip を展開し直す)。
export const ROUNDTRIP_PROGRAMDATA_SCENARIOS = ["roundtrip"];
// 手順書の「ドックが表示されない」の項に、OBS を 33 以上へ更新する案内があるかを見る (old-obs の確認)。
// 日本語と英語の手順書 (Markdown) の場所と、項の ### の見出しと、案内の文 (空白と改行は比べない)。
export const GUIDE_UPDATE_HINTS = [
  { rel: "guide/ja.md", title: "ドックが表示されない", needle: "33より古いときは、OBSを更新します" },
  { rel: "guide/en.md", title: "The dock doesn't appear", needle: "older than 33, update OBS" },
];
// 較正の 2 回と、差分を取る場面。
export const ROUNDTRIP_SCENARIOS_WITH_SNAPSHOTS = ["roundtrip", "other-vencord"];
export const ROUNDTRIP_WORK_PREFIX_REL = "_work/rt-";
const ROUNDTRIP_TAG_RE = /^[A-Za-z0-9._-]{1,64}$/;
// ポータブルでない OBS の起動の仕方 (--layout programdata。ProgramData の plugins はポータブルの OBS では
// 読まれない = src-obs/.deps/obs-studio-31.1.1/frontend/widgets/OBSBasic.cpp:132-133 の portable_mode)。
export const OBS_LAUNCH_ARGS_INSTALLED = OBS_LAUNCH_ARGS.filter((a) => a !== "--portable");
// seed-config.py の --shape user のコレクションの名前 (seed-config.py の USER_COLLECTION_* と揃える。
// run-window-plan.test.ts が seed-config.py の文字列と突き合わせる)。
export const ROUNDTRIP_COLLECTION_MAIN = "Stream";
export const ROUNDTRIP_COLLECTION_SECOND = "Chat";
// src-obs/platform/bundle_check.h の kVencordBundleFiles と同じ 4 つ (seed-config.py と同じ並び)。
export const VENCORD_BUNDLE_FILE_NAMES = ["vencordDesktopMain.js", "vencordDesktopPreload.js", "vencordDesktopRenderer.js", "vencordDesktopRenderer.css"];
// ビルドの目印のファイル (src-obs/install/build_mark.h の kBuildMarkFileName)。
export const BUILD_MARK_FILE_NAME = "cordmux-build.txt";
// install_snapshot.py が文字列として読む製品のヘッダー。
export const REMOVAL_TABLE_REL = "src-obs/install/removal_table.h";
export const SCENE_NAMES_HEADER_REL = "src-obs/sources/scene_names.h";
export const JA_LOCALE_INI_REL = "src-obs/data/locale/ja-JP.ini";
// インストールの往復 (install-roundtrip) と call-1 が指す画面のテキストの id (dry-run とテストが ini と突き合わせる)。
export const ROUNDTRIP_TEXT_IDS = [
  TEXT_ID_DOCK_WINDOW_TITLE,
  TEXT_ID_DOCK_MENU_BUTTON,
  TEXT_ID_MENU_SETTINGS,
  TEXT_ID_ADD_TO_VESKTOP,
  TEXT_ID_REINSTALL,
  TEXT_ID_SETTINGS_WINDOW_TITLE,
  TEXT_ID_SETTINGS_PAGE_ABOUT,
  TEXT_ID_SETTINGS_UNINSTALL,
  TEXT_ID_TOOLS_MENU_UNINSTALL,
  TEXT_ID_DIALOG_UNINSTALL_TITLE,
  TEXT_ID_DIALOG_UNINSTALL_BUTTON,
  TEXT_ID_DIALOG_VESKTOP_ADD_TITLE,
  TEXT_ID_DIALOG_SWITCH_BUTTON,
];
// 待ちの秒数 (見積もり。実機の実行で測って見直す)。
// 較正の起動で、閉じる前に OBS が落ち着くまでの猶予 (ドックのウィンドウの DOCK_VIEW_SETTLE_WAIT_MS より長め)。
export const ROUNDTRIP_SETTLE_MS = 8000;
// ドックがメッセージのラベルを描くまで (call-gate-a と同じ 5 秒)。
export const ROUNDTRIP_DOCK_SETTLE_MS = 5000;
// モーダルのダイアログを開くボタンの Invoke を打ち切る時限 (export-bundle と同じ 10 秒)。
export const ROUNDTRIP_INVOKE_TIMEOUT_MS = 10000;
// ダイアログが開くのを待つ上限と、「出ないこと」を見るときの短い待ち。
export const ROUNDTRIP_DIALOG_TIMEOUT_MS = 15000;
export const ROUNDTRIP_DIALOG_ABSENT_TIMEOUT_MS = 3000;
// アンインストールの結果のダイアログが出るまでの上限 (他のコレクションを順に開いて戻すため)。
export const ROUNDTRIP_UNINSTALL_DONE_TIMEOUT_MS = 60000;
// state.json が書き終わるまで (call-gate-a と同じ 3 秒)。
export const ROUNDTRIP_WRITE_SETTLE_MS = 3000;
// コレクションを切り替えてからドックがラベルを描き直すまで。
export const ROUNDTRIP_COLLECTION_SWITCH_SETTLE_MS = 5000;
// 起動してから、起動時の書き直しと最初の照合が済むまで。
export const ROUNDTRIP_STARTUP_CHECK_WAIT_MS = 10000;
// Force Update の場面で短くする照合の周期 (秒) と、その 4 周期ぶんの待ち。
export const ROUNDTRIP_CHECK_PERIOD_S = 5;
export const ROUNDTRIP_TAMPER_WAIT_MS = 4 * ROUNDTRIP_CHECK_PERIOD_S * 1000;
// [再インストール] の後、コピーを終えて問い合わせ直すまで。
export const ROUNDTRIP_REINSTALL_SETTLE_MS = 5000;
// force-update-tamper で接続しておくテスト用の送信側の長さ (接続している間の周期を通すため)。
export const ROUNDTRIP_TAMPER_HARNESS_SECONDS = 120;
export const ROUNDTRIP_CONNECT_WAIT_TIMEOUT_MS = 15000;
// newer-libobs で、OBS 33.x の「ツール」のプラグインの管理を人が開いて、スクリーンショットが撮られるまで待つ長さ
// (この 1 手だけは人の手で行い、実行の手順に書く)。
export const ROUNDTRIP_PLUGIN_MANAGER_HOLD_MS = 60000;
// Force Update の上書きを模して 1 バイト足すファイル。
export const ROUNDTRIP_TAMPER_FILE = "vencordDesktopRenderer.js";

// ---- call-1 の実行計画 (4 アカウントの通話のテストと短い項目を 1 回の実行 (2 時間 30 分) にまとめたもの) の値 -----------------------
//
// **人の操作は手順 "human" で待つ**: 実行スクリプトは人の操作を自分では行わず、ログに「HUMAN: <文>」を書いて
// 条件が満たされるまで見る。条件 (until) は 5 つ:
//   go-file                      <出力>/window3b/human-go.txt があれば消して進む (どの until でも有効)
//   state-json-points-to-placed  実際の state.json の vencordDir が配置物の data/vencord を指したら進む
//   state-json-restored          state.json が製品のバックアップ (vencord_dir_backup.json) の previous に戻ったら進む
//   call-ready                   OBS の記録の最後の Facts の行で、通話の人数が roster 以上、画面共有の人数が sharing 以上になったら進む
//                                (callReadyFromLog。人が返事をしなくても、全員が入って共有を始めたことを実行スクリプトが見て進む)
//   seconds                      waitSeconds だけ待って進む (--rehearsal がすべての human をこれに置き換える)
// waitSeconds は**人の操作の見込み** (plannedMinutes が合計に入れる値)。until が seconds のときだけ実際の待ち。
export const HUMAN_UNTIL_VALUES = ["go-file", "state-json-points-to-placed", "state-json-restored", "call-ready", "seconds"];
// call-ready の読み: 受信側が facts を受け取るたびに書く行 (src-obs/pipeline/pipeline.cpp の handle_facts の
// 「Facts: roster=<人数か「-」> sharing=<人数> ...」) の最後の 1 行。roster が「-」(通話の参加者が分からない = Vesktop のアカウントが
// 通話にいない) なら人数は null。
const FACTS_COUNTS_RE = /\[cordmux\] Facts: roster=(\d+|-) sharing=(\d+)/;
export function callReadyFromLog(lines) {
  let last = null;
  for (const raw of lines || []) {
    const m = FACTS_COUNTS_RE.exec(String(raw));
    if (m) last = { roster: m[1] === "-" ? null : Number(m[1]), sharing: Number(m[2]) };
  }
  return last;
}
// 最後の Facts の行が、通話の人数 roster 以上・画面共有の人数 sharing 以上か。
export function callReady(lines, { roster, sharing }) {
  const facts = callReadyFromLog(lines);
  return Boolean(facts && facts.roster !== null && facts.roster >= roster && facts.sharing >= sharing);
}
export const HUMAN_GO_FILE_NAME = "human-go.txt";
export const HUMAN_POLL_MS = 2000;
// 人の操作の時限。操作する人がすぐ PC の前に戻れるとは限らないので、操作 1 つにつき 30 分、通話に参加して共有を
// 始めるまでは 40 分。待ちの長さは判定に使わない
// (waitedMs はログに残すだけ)。
export const HUMAN_TIMEOUT_MS = 30 * 60 * 1000;
export const HUMAN_UNINSTALL_TIMEOUT_MS = 30 * 60 * 1000;
export const HUMAN_JOIN_TIMEOUT_MS = 40 * 60 * 1000;
// 人の操作の見込み (秒)。見積もり: 実行の長さ 2 時間 30 分の内訳 (準備 20 / サブアカウントを使う構成 10 /
// 締め 10) を人の操作の数で割った値。合計は plannedMinutes が 120 から 140 分に入ることをテストで確認する。
export const HUMAN_ALLOWANCE_REOPEN_S = 180;
export const HUMAN_ALLOWANCE_JOIN_S = 720;
export const HUMAN_ALLOWANCE_SHORT_S = 120;
export const HUMAN_ALLOWANCE_SHARE_S = 180;
export const HUMAN_ALLOWANCE_UNINSTALL_S = 300;
// 実行の期限 (分)。既定 120 はテスト用の送信側だけの実行の上限。call-1 は実行計画の見込みが 120 分を超える
// (2 時間 30 分) ので、--deadline-min が無ければ CALL_ONE_DEADLINE_MIN を使う (見込み + 接続待ち + 人の操作の遅れ)。
// 期限に達すると run-window.mjs は残りの手順を打ち切る (締めの アンインストール の判定が付かないまま終わる) ので、
// --dry-run は見込みが期限に足りない分 (deadlineShortfallMinutes) を出す。
export const DEFAULT_DEADLINE_MIN = 120;
export const CALL_ONE_DEADLINE_MIN = 180;
// サブアカウントを使う構成: メインアカウントを選んでから案内が引き直されるまで 15 秒、人の操作の後は 30 秒。
export const CALL_ACCOUNT_CHANGE_SETTLE_MS = 15000;
export const CALL_NOTICE_SETTLE_MS = 30000;
// テスト動画の A/V: 共有が始まってから 20 秒待ち、5 分録る。2 回目は**メインの録画 (main) の開始**から
// 90 分以上あと (「開始直後と 90 分後」の起点はメインの録画の開始。メインの録画にも sync.py を実行するので
// 開始直後の値はそこから取れ、av1 は中間の点になる)。
export const CALL_AV_SHARE_SETTLE_MS = 20000;
export const CALL_AV_RECORD_MS = 5 * 60 * 1000;
export const CALL_AV_SECOND_GAP_MINUTES = 90;
// 途中で録画 ON: 10 分。
export const CALL_MID_RECORD_MS = 10 * 60 * 1000;
// 使う声の置き換え: 20 秒。
export const CALL_VOICE_SOURCE_SETTLE_MS = 20000;
// 連続運転: 接続している間の照合の周期は製品の既定 300 秒 (settings.h の vencord_check_connected_s)。
// 正常に 6 周 + わざと異常 1 回 (足してから 6 分、戻してから 6 分 = 周期 1 つ + 余裕)。
export const CALL_CHECK_PERIOD_MS = 5 * 60 * 1000;
export const CALL_CHECK_CYCLES = 6;
export const CALL_TAMPER_WAIT_MS = 6 * 60 * 1000;
export const CALL_TAMPER_FILE = ROUNDTRIP_TAMPER_FILE;
// OBS のメモリと欠落を 30 分ごとに取る位置 (見込みの分の累計で置く。insertStatsAtMinutes)。
export const CALL_STATS_MARK_MINUTES = [30, 60, 90];
// --rehearsal: 待ちの短縮の倍率は**この 1 か所** (1 分以上の wait だけを縮める。数秒の待ちは縮めない)。
export const REHEARSAL_TIME_FACTOR = 0.1;
export const REHEARSAL_SCALE_MIN_MS = 60000;
export const REHEARSAL_HUMAN_WAIT_SECONDS = 5;
// テスト用の送信側の寿命 = 実行計画の見込み x 倍率 + 余裕 (秒)。見込み (plannedSeconds) は wait / human / tailSeconds しか
// 数えず、wait-finalize (最大 15 分)・dialog-click・PNG・sync.py などの判定ツールの実行時間は入らないので、見込みの 2 倍に
// 15 分を足す。送信側が先に居なくなると av2 の sync.py とわざと異常の戻しが「実際の実行にはない理由」で失敗になる。
// 寿命が長すぎても締めの harness-stop が止めるので害はない。
export const REHEARSAL_HARNESS_LIFETIME_FACTOR = 2;
export const REHEARSAL_HARNESS_MARGIN_S = 15 * 60;
export const REHEARSAL_CONNECT_WAIT_TIMEOUT_MS = 60000;
// リハーサルでアンインストールの結果のダイアログを待つ上限 (roundtrip と同じ)。
export const REHEARSAL_UNINSTALL_DONE_TIMEOUT_MS = ROUNDTRIP_UNINSTALL_DONE_TIMEOUT_MS;
// call-1 が指す画面のテキストの id (dry-run とテストが ini と突き合わせる)。メッセージはログには出ない (ドックに
// 描かれるだけ) ので、dialog-click.ps1 -ListOnly でドックのウィンドウの文を読み、見出しの %1 より前のテキストで当てる。
export const CALL_TEXT_IDS = [
  TEXT_ID_DOCK_WINDOW_TITLE,
  TEXT_ID_DOCK_MENU_BUTTON,
  TEXT_ID_MENU_SETTINGS,
  TEXT_ID_ADD_TO_VESKTOP,
  TEXT_ID_REINSTALL,
  TEXT_ID_RECORD_START,
  TEXT_ID_RECORD_STOP,
  TEXT_ID_ROW_MORE_ACTIONS,
  TEXT_ID_ROW_VOICE_FROM,
  TEXT_ID_ROW_OWN_VOICE,
  TEXT_ID_TAB_AUDIO,
  TEXT_ID_TAB_PEOPLE,
  TEXT_ID_TAB_STATUS,
  TEXT_ID_AUDIO_VOICE_KIND,
  TEXT_ID_MAIN_ACCOUNT_NOT_IN_CALL,
  TEXT_ID_MAIN_ACCOUNT_NOT_IN_CALL_NO_NAME,
  TEXT_ID_NO_VIDEO,
  TEXT_ID_SUMMARY_CONNECTED,
  TEXT_ID_VENCORD_OVERWRITTEN,
  TEXT_ID_SETTINGS_WINDOW_TITLE,
  TEXT_ID_SETTINGS_PAGE_ABOUT,
  TEXT_ID_SETTINGS_PAGE_INCOMING_VIDEO,
  TEXT_ID_DOCK_TAB_DISPLAY,
  TEXT_ID_DOCK_ACCOUNT_CHANGE,
  TEXT_ID_LOOK_SELF_FRAME_SOURCE,
  TEXT_ID_SETTINGS_VIDEO_ENCODER,
  TEXT_ID_SETTINGS_VIDEO_ENCODER_SOFTWARE,
  TEXT_ID_SETTINGS_ENCODE_PRIORITY,
  TEXT_ID_SETTINGS_ENCODE_PRIORITY_QUALITY,
  TEXT_ID_ROW_CHECK,
  TEXT_ID_SETTINGS_UNINSTALL,
  TEXT_ID_DIALOG_UNINSTALL_TITLE,
  TEXT_ID_DIALOG_UNINSTALL_BUTTON,
  TEXT_ID_DIALOG_VESKTOP_ADD_TITLE,
  TEXT_ID_DIALOG_SWITCH_BUTTON,
];

// ---- 判定スクリプトのセルとアカウントの名前 ---------------------------------------------
//
// wire-composite が合成のソースを置く、録画するシーン。
export const WIRE_SCENE_NAME = "Main";
// 判定スクリプトのセルは、録画の中で枠が実際に占める四角形 (OBS の scene item の位置と大きさ) から決める。1280x720 の
// 共有を 0.5 倍で置くと枠は 640x360 になるが、判定スクリプトが 1920x1080 の 4 象限 (960x540) を見ていると、白のマーカーの平均が
// 200 に届かず 1 つも白と見なせない (white_events 0)。セルの一辺はテスト用の送信側の古い形の帯で 16 px (sender.ts) なので、
// 録画の上では 16 x 枠の倍率 (0.5 なら 8)。**この値で読めるのは、古い形の帯が枠の左上に等倍で載っているときだけ** (テスト用の
// 送信側)。実際の共有の中身 (pattern.html / sync-video.mp4) は「時計つきの帯」(bit_strip.py) で、
// frames.py がセル (--x --y --w --h) の中で探してセルの大きさも測る (共有の帯がブラウザのタブの列の下に縮められて写り、
// 枠の左上にはないことがあるため。bit_strip.py の冒頭の実測を参照)。
// smooth.py も同じくセル (--regions) の中で時計つきの帯を探し、見つからなければ古い形の帯をセルの左上で読む。
export const BIT_CELL_SOURCE_PX = 16;
// 実際の通話のテストに使うアカウントの名前 (Discord の画面に見えている表示名)。役割ごとに、--call-accounts <JSON ファイル> で受ける。
// 名前をコードに書かないため、ファイルを渡さないとき (ドライランとテスト) はこの例の名前を使う。実際の通話のテストではファイルが要る
// (callAccountsProblem)。役割:
//   main     デスクトップ版の Discord のメインアカウント (自分の枠の行になる)
//   receiver Vesktop のサブアカウント (受信側。声の行も画面共有の行もない)
//   chrome   Chrome のアカウント (テスト用のマイクの 1 kHz を送り、テスト動画のタブを共有する)
//   firefox  Firefox のアカウント (画面を共有している)
export const CALL_ACCOUNT_ROLES = ["main", "receiver", "chrome", "firefox"];
export const DEFAULT_CALL_ACCOUNTS = Object.freeze({ main: "main", receiver: "sub1", chrome: "sub2", firefox: "sub3" });
export function callAccounts(args) {
  return args && args.callAccounts ? args.callAccounts : DEFAULT_CALL_ACCOUNTS;
}
// --call-accounts のファイルの中身 (JSON の文字列) を読む。キーは CALL_ACCOUNT_ROLES の 4 つ。どれも 1 から 64 文字の文字列
// (制御文字を含まない)、4 つとも別の名前。足りないキー・知らないキー・同じ名前の重なりは、理由つきで断る。
export function parseCallAccounts(text) {
  const errors = [];
  let data = null;
  try {
    data = JSON.parse(text);
  } catch (e) {
    return { ok: false, errors: ["--call-accounts のファイルが JSON として読めない (" + e.message + ")"] };
  }
  if (data === null || typeof data !== "object" || Array.isArray(data)) {
    return { ok: false, errors: ["--call-accounts のファイルは、キーが " + CALL_ACCOUNT_ROLES.join("・") + " のオブジェクト"] };
  }
  for (const k of Object.keys(data)) {
    if (!CALL_ACCOUNT_ROLES.includes(k)) errors.push("--call-accounts のファイルに知らないキー " + k + " がある (使えるキーは " + CALL_ACCOUNT_ROLES.join("・") + ")");
  }
  const accounts = {};
  for (const role of CALL_ACCOUNT_ROLES) {
    const v = data[role];
    if (typeof v !== "string" || !v.trim() || v.trim().length > 64 || /[\u0000-\u001f\u007f]/.test(v)) {
      errors.push("--call-accounts のファイルの " + role + " は 1 から 64 文字の名前 (文字列。制御文字を含まない)");
    } else {
      accounts[role] = v.trim();
    }
  }
  if (errors.length === 0) {
    const names = CALL_ACCOUNT_ROLES.map((r) => accounts[r]);
    if (new Set(names).size !== names.length) errors.push("--call-accounts のファイルの 4 つの名前は、どれも別の名前にする");
  }
  return errors.length > 0 ? { ok: false, errors } : { ok: true, accounts: Object.freeze(accounts) };
}
// 実際の通話のテスト (call-1 でリハーサルでもドライランでもないもの) は --call-accounts が要る (call-hold の実行計画は名前を使わない)。問題があれば文を返す。
export function callAccountsProblem(args) {
  if (!args || args.dryRun || args.rehearsal || args.step !== "call-1") return null;
  if (args.callAccounts) return null;
  return "--call-accounts <JSON ファイル> が要る (実際の通話の実行に使うアカウントの名前。キーは " + CALL_ACCOUNT_ROLES.join("・") + ")";
}
// テスト動画の A/V の録画 (av1 / av2) の間、実行スクリプトがテスト動画を共有している人の画面の枠のソース以外の cordmux の音を
// 全部ミュートする (録画のミックスに Chrome のテスト用のマイクの 1 kHz が混ざると、マーカーの 2 kHz が埋もれるため)。
// 誰の枠かは、人が載った枠の名前 (「%1 (cordmux画面共有 %2)」) の %1 で見分ける
// (classifyCordmuxInputs。表示名は依頼文にも出ているテキスト)。
// リハーサル (テスト用の送信側) はマーカーの音を最初の画面ストリームに乗せる (DEFAULT_SYNC_TONE)。その枠の表示名。
export const REHEARSAL_AV_SCREEN_ACCOUNT = "harness-screen-1";
// call-1 では Vesktop の共有を依頼しない (--vesktop-share を渡したときだけ依頼する)。true にすると、参加の文に Vesktop の共有を入れる
// (callJoinMessage)。
export const VESKTOP_SHARE_IN_CALL_ONE = false;
// 誰の声を誰から取るか: 画面共有している人 = Firefox のアカウント (画面を共有している)、声を貸す人 = Chrome のアカウント (テスト用のマイクの 1 kHz を
// 送る)。「声がリンク先から来る」ことは、声を貸す人の声が実際に鳴っていないと見えない。画面共有している人を Chrome、声を貸す人を
// Firefox にすると、Firefox のマイクがミュートなので、置き換えても「Chrome の 1 kHz が消える」ことしか見えない。
// 入れ替えると、1 kHz が Chrome のアカウントの声枠から Firefox のアカウントの声枠へ移ることと、Firefox のアカウントの画面の音が変わらないことの 2 つが見える
// (OBS のミキサーの行のメーター。録画で機械に確認する手順はまだない)。
// 4 分割の付け外しで枠から外して戻す人 (実際の実行)。画面を共有している 3 人のうち、使う声の画面共有している人と同じ Firefox のアカウント (誰でもよい)。
// リハーサルはテスト用の画面ストリームの最初の 1 つ。行は見えている名前で名指すので、
// Discord の表示名がこのテキストと違えば押せずに失敗する (黙って別の人を押さない)。
// 映像が届かないときのメッセージ (AllVideoStoppedHint) は、映像が実際に止まったときだけ出るのが正しい。
// Vesktop でスピーカーミュートにしても映像は届き続ける (画面 3 ストリームで 72-75 フレーム/秒) ので、そのときメッセージは出ない。そこで
// 「案内が出ている」と「映像が止まっている」が一致することだけを判定し、どちらだったかは記録に残す。映像のレートは
// プラグインのログの `[cordmux] stats video received_hz=` の直近 VIDEO_RATE_WINDOW_LINES 行 (10 秒ごとに出る) で見て、
// すべて VIDEO_STOPPED_HZ 未満なら止まっているとみなす (1 フレーム/秒 未満は実質 0 とみなす)。
export const VIDEO_RATE_WINDOW_LINES = 2;
export const VIDEO_STOPPED_HZ = 1;
// call-1 の OBS の録画 (main / ac32 / av1 / av2 / ac35) は後で測り直せるように残す (finally の cleanup.ps1 が
// 消すと、OBS の出口の A/V を測り直せない)。cleanup.ps1 -KeepRecordings が残すのは名前が recordings のフォルダの下
// だけなので、締めの手順と finally で `<out>/window3b/recordings/obs-<tag>/` へ移す。消すのは後で cleanup.ps1 を
// 素で 1 回実行したとき。配信の受け口の flv (実行の全体、約 2 時間半) は測る予定がないので残さない。
// call-hold も残す: 話している人の光は、テストを実行する人が obs-websocket の StartRecord / StopRecord で
// 手で録り、border_timing.py が実行の後に読む。実行スクリプトが始めた録画ではないので label がなく、下の
// STEPS_KEEPING_UNLABELED_OBS_RECORDINGS で拾う。
// record も残す: 途中で解像度が 2 回変わる録画の、名前ラベルの背景の高さの並びを、実行の後に plate_measure.py で読む。背景は
// OBS の出力にだけ描かれ、cordmux の参加者ごとの録画にはないので、OBS の録画を残さないと読めない。
export const STEPS_KEEPING_OBS_RECORDINGS = ["call-1", "call-hold", "record"];
export const OBS_RECORDINGS_KEEP_DIR_PREFIX = "recordings/obs-";
// 実行スクリプトが始めていない OBS の録画 (テストを実行する人が開いている間に obs-websocket で録ったもの) も残す step。OBS の録画の保存先
// (seed-config.py の RecFilePath = <out>/window3b) の直下にある、実行の開始より後に書かれた動画を "extra-<n>" として移す
// (unlabeledObsRecordings)。テストを実行する人が StopRecord の後に自分で移す方式にしないのは、締めの手順の cleanup が先に消しうるため。
export const STEPS_KEEPING_UNLABELED_OBS_RECORDINGS = ["call-hold"];
// OBS の録画の拡張子 (OBS の [録画フォーマット] の物。seed-config.py は mkv を書く)。
export const OBS_RECORDING_EXTENSIONS = [".mkv", ".mp4", ".mov", ".flv", ".ts", ".m3u8"];
export const UNLABELED_RECORDING_LABEL_PREFIX = "extra-";

// 実行スクリプトが始めていない OBS の録画を選ぶ (純関数)。entries は保存先の直下の [{ name, path, isFile, mtimeMs }]、known は
// label 付きで既に覚えている録画の場所、sinceMs は実行の開始時刻。選んだものを名前の順に { label: "extra-<n>", from } で返す。
export function unlabeledObsRecordings({ entries, known, sinceMs }) {
  const knownSet = new Set((known || []).map((p) => String(p).replace(/\\/g, "/").toLowerCase()));
  const picked = (entries || [])
    .filter((e) => e && e.isFile && typeof e.name === "string" && typeof e.path === "string")
    .filter((e) => OBS_RECORDING_EXTENSIONS.some((ext) => e.name.toLowerCase().endsWith(ext)))
    .filter((e) => Number(e.mtimeMs) >= Number(sinceMs || 0))
    .filter((e) => !knownSet.has(e.path.replace(/\\/g, "/").toLowerCase()))
    .sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  return picked.map((e, i) => ({ label: UNLABELED_RECORDING_LABEL_PREFIX + (i + 1), from: e.path }));
}
// 実行の期限の見積もり (deadlineEstimate) の数。実測した手順の所要時間に基づく。
//   - 判定ツール: 1 つの区間を 1 回読むのに、録画 1 秒あたりの秒数。全面の灰色をパイプへ流す読み方では、sync.py が
//     0.20、frames.py が 0.18、smooth.py が 0.27 (枠 3 つ、20 分の録画)。区間を ffmpeg の側で切り出した後の手元の
//     実測 (1080p60 の 60 秒、通話なし) は sync.py 0.049 / frames.py 0.043 / smooth.py 0.13 で、通話中は
//     同じ読み方が手元より約 1.6 倍遅かった (0.20 / 0.13) ので、2 倍して丸めた値を置く。
//   - JUDGE_PASS_OVERHEAD_S: ffprobe と ffmpeg の起動、音の読み込み (1 回ごと)。
//   - judge-record (ledger_check / black_span / au_hash): 3 つで 1.6 分。1 つ 60 秒。
//   - 条件待ち (wait-connection / wait-finalize): どちらも数秒。1 つ 60 秒。
//   - 他の手順: 待ち・人・判定ツール以外の約 140 手順で約 1.5 分。1 手順 2 秒 (余裕込み)。
//   - CALL_ONE_HUMAN_OVERRUN_MIN: 人の操作の所要時間は見込みを 17 分超えた (タイムアウトの 10 分に達した手順が 2 つ)。
//     人がすぐ操作できるとは限らないので 90 分にした。期限は打ち切りの上限なので、早く操作すれば実行は延びない。
//     短すぎると締めのアンインストールの前に打ち切られる。
//   - DEADLINE_MARGIN_FACTOR: 上の合計に掛ける余裕 (15%)。
export const JUDGE_SECONDS_PER_RECORDED_SECOND = { "frames.py": 0.1, "sync.py": 0.1, "smooth.py": 0.3 };
export const JUDGE_PASS_OVERHEAD_S = 15;
export const JUDGE_RECORD_ALLOWANCE_S = 60;
export const CONDITION_WAIT_ALLOWANCE_S = 60;
export const STEP_OVERHEAD_S = 2;
export const CALL_ONE_HUMAN_OVERRUN_MIN = 90;
export const DEADLINE_MARGIN_FACTOR = 1.15;

// ---- 実際の通話の再確認で call-1 に足したフラグ ------------------------------------------------
// 1 アカウント方式で Vesktop が自分も共有しながらほかの 3 ストリームを受ける形、OBS の出口の
// 映像と音のずれ、途中で始めた cordmux の録画の中のずれ、Firefox のようにフレームの間隔が不規則な共有での時刻の溜まり、使う声の
// ラベル、別アカウント方式のメッセージ、バージョンとアイコンの表示を、人の操作を少なく 1 回の通話で再確認するためのフラグである。
// **どれも既定はなし**で、フラグが無ければ開始ゲート 4 つの後に call-1 を実行する形のままになる (テストがこの形を確認し続ける)。
//   --start-with-add    開始ゲートを実行せず、call-1 の先頭で Vesktop の終了と [Vesktopに追加] -> Vesktop の起動 -> 接続 (call-hold の先頭と同じ並び。
//                       addToVesktopSteps)。開始ゲートは 4 つとも答えが出た (a・b・c は合格、d は「package.json も要る」) ので、
//                       Vesktop の終了と起動を 4 回から 1 回に減らす。ゲートを残すと、同じ答えを人の操作 3 回で
//                       取り直すだけで新しく分かることがない。リハーサルは前から実行スクリプトが [Vesktopに追加] を押すので変わらない。
//   --vesktop-share     参加の文で Vesktop の共有も依頼する (1 アカウント方式の既定の使い方。固定の Vencord が古いと、共有の選択の
//                       画面で落ちる)。
//   --skip-continuous   連続実行 (5 分 x 6 とわざと異常にした 12 分) と、av2 をメインの手順から 90 分あけるための待ちを省く。
//   --skip-av2          2 回目のテスト動画の A/V (av2) と、av1 との差を省く (時間が足りないときに最初に省く項目の 1 つ)。
//   --skip-speaker-mute 別アカウント方式の段のスピーカーミュートと解除を省く (ミュートしても映像が止まらず、メッセージが出ない形が
//                       正しい)。--broadcaster-id と一緒にだけ受ける。
//   --extra-checks      人の操作が要らない確認を足す (どれも記録して判定もするが、gate にしない = 実行を止めない):
//                       (a) 参加と共有の前に送信部の接続の数を保存し、共有を始めた後とメインの手順の後に切れていないか (Vesktop が共有で落ちて
//                           起動し直すと close と hello が増える)。
//                       (b) cordmux の設定ウィンドウの [バージョン情報] の「バージョン」の Vesktop の行が「-」でないか。
//                       (c) 使う声の置き換えの後と戻した後に、画面共有している人の声の枠だけ・声を貸す人の声の枠だけを鳴らして短く録り、1 kHz
//                           (Chrome のテスト用のマイクの音) が画面共有している人の枠へ移って戻るかを tone_scan.py で数える。
//                       (d) 途中で始めた cordmux の録画のうち、テスト動画を共有している人の画面の録画に sync.py を実行する
//                           (音が 142 ms 先から 177 ms 先へ広がったのを測った形)。
//                       (e) OBS のログの stats 行から、ストリームごとの整列のずれ (offset_us) の時間に対する傾き、送信側の降格の段、
//                           アイコンを送らなかった行、接続と切断の数を数える (Firefox で 1% の溜まりを見つけた形)。
//                       (f) 声の音量: 使う声の確認の次に、[音声] タブの画面共有している人の声のつまみを 200% にしてもらい、同じ条件で
//                           もう 1 つ録って、置き換えの確認の 1 つ目 (100%) との音量の比を見る (voiceVolumeSteps、judgeVoiceVolume)。
//                       (g) 実際のアバターの並び (--extra-checks の実際の実行だけ): 締めの文を「共有を止める」と「抜けて Vesktop を終了」の
//                           2 つに分け、共有を止めた後に合成のソースの画像と並びの名前ラベルの行を残す (rosterCheckSteps)。
//                       (h) 映像のバッファの統計: メインの手順の録画の間に、ドックの [状態] タブを開いて撮り、[参加者] タブへ戻す
//                           (incomingVideoSteps。押す手順は readOnlyOp で、「その他の操作」に数えない)。
//                       (a)-(h) を call-1 の既定にしないのは、テストの多くが手順の数と並びで書いてあり、フラグで分けて
//                       おけば既定の形と新しい形の両方をテストで確認し続けられるためである。
// 実行前の点検で足したフラグ (どれも既定はなし。フラグが無ければ上の形のまま):
//   --skip-export-bundle  締めの診断情報の書き出し (ドックのメニュー -> 保存ダイアログへ dialog-sendkeys がファイルの場所と Enter を
//                         SendKeys で打つ) を省く。SendKeys は「いま前面にあるウィンドウ」へ送るので、人がキーボードとマウスを使って
//                         いる間に打つと、残りのテキストと Enter が別のウィンドウ (Discord の入力欄など) へ入りうる。同じものは
//                         診断情報の書き出し (dock-view の export-bundle。人が触らない区間) で確認している。実行計画の中でキーを送る手順は
//                         KEY_SENDING_KINDS (keySendingSteps) で数え、--dry-run の出力に出す。保存ダイアログのファイル名の欄を
//                         UI Automation の ValuePattern で入力して [保存] を Invoke する方式にしないのは、ツールの作り直しが要り、
//                         ここで確認したいことは診断情報の書き出しで足りているためである。
//   --latency-person <名前>  cordmux が足す遅れ (Discord 自身の再生に対して OBS の出力がどれだけ遅れるか) を測る区間を
//                         足す (callLatencySteps の説明を参照)。名前は遅れを読む画面共有している人 (配信者) の見えている名前で、
//                         個人のアカウントの名前なのでテキストとして持たずに引数で受ける。
//   --voice-channel <名前>  参加の文 (`callJoinMessage`) で入ってもらうボイスチャンネルの名前。テストに使うチャンネルは、テストをする人の
//                         Discord のサーバーにあるので、名前をコードに書かずに引数で受ける (`--latency-person` と同じ扱い)。渡さなければ、
//                         文はチャンネルの名前を挙げない。
//   --recheck             実際の通話でしか確認できない項目だけを実行する短い実行 (callRecheckBodySteps)。初期設定・[Vesktopに追加]・Vesktop の起動と接続の後に、
//                         送信側が自分のアカウントを届けたか (wait-self-account)、参加の後の画像、バージョンの表示、画面共有の音、映像の遅れ、共有の途中の
//                         解像度の変化、エンコードの設定、別アカウント方式のメッセージ、自分の枠、スピーカーミュートと最小化、受け手の Vesktop の抜け戻り、
//                         実際のアバターの並び、締めだけを並べる。メインの手順の録画と配信・使う声と音量・av1/av2・連続実行・診断情報の書き出しは
//                         実行しない。--start-with-add と一緒にだけ受け、メインの手順の長さを決めるフラグ (--vesktop-share --extra-checks --skip-*) とは両立しない
//                         (どれも recheck の実行計画には適用されないので、渡されたら誤りにして呼び出し行の食い違いを先に見つける)。--rehearsal とは
//                         両立させる: 手順の並びと実行スクリプトの受け渡しを、実際の実行の前に通せる。テスト用の送信側では確かめられない項目
//                         (REHEARSAL_NOT_JUDGED) は、リハーサルでは判定しない (手順 skip に理由を残し、judge-lines の行は判定なし)。
//                         実際のアバターの並びと共有の途中の解像度の変化はテスト用の送信側では起こせないので、リハーサルでは入らない。
//   --av-check            --recheck に、テスト動画の映像と音のずれの測定を足す (recheckMeasureSteps)。
//   --av-only             --recheck --av-check の手順表を、映像と音のずれ (と --latency-person の遅れ) を測るところと締めだけにする
//                         (callAvOnlyBodySteps)。バージョンの表示・画面共有の音・解像度の変化・エンコードの設定・別アカウント方式のメッセージ・
//                         自分の枠・スピーカーミュートと抜け戻り・実際のアバターの並びは実行しない。締めのアンインストールは実行スクリプトが押す
//                         (手順書の道を人が押す確認は --recheck の締めにある)。
// 引数の値を取るフラグは CALL_ONE_VALUE_FLAGS。
export const CALL_ONE_OPTION_FLAGS = ["--start-with-add", "--vesktop-share", "--skip-continuous", "--skip-av2", "--skip-speaker-mute", "--extra-checks", "--skip-export-bundle", "--recheck", "--av-check", "--av-only"];
export const CALL_ONE_VALUE_FLAGS = ["--latency-person", "--voice-channel"];
// (b) [バージョン情報] の「バージョン」の Vesktop の行の名前 (settings_window.cpp のバージョンの表)。
export const TEXT_ID_SETTINGS_VERSION_VESKTOP = "cordmux.settings.version_vesktop.label";
// バージョンの表のラベルの全部 (ja-JP.ini の version_*。ラベルと値を並びに頼らず分けるため = judgeVersionRowShown)、いつもバージョンが出る行
// (cordmux (OBS))、値が読めない行の読み上げの名前 (settings_window.cpp の update_about が「-」の値に付ける)。
export const TEXT_ID_SETTINGS_VERSION_OBS_PLUGIN = "cordmux.settings.version_obs_plugin.label";
export const TEXT_ID_SETTINGS_VERSION_LABELS = [
  TEXT_ID_SETTINGS_VERSION_OBS_PLUGIN,
  "cordmux.settings.version_vesktop_plugin.label",
  TEXT_ID_SETTINGS_VERSION_VESKTOP,
  "cordmux.settings.version_bundled_vencord.label",
  "cordmux.settings.version_obs.label",
  "cordmux.settings.version_windows.label",
  "cordmux.settings.version_gpu.label",
];
export const TEXT_ID_SETTINGS_UNAVAILABLE_A11Y = "cordmux.settings.unavailable.a11y";
// (c) 使う声の確認。録画 1 つの長さ (tone_scan.py の 20 ms のブロックで 400 個。置き換わりの数秒は前の 20 秒の
// 待ち = CALL_VOICE_SOURCE_SETTLE_MS が吸う) と、数える音の高さ (Chrome のテスト用のマイクの 1 kHz)。
export const VOICE_SOURCE_CHECK_RECORD_MS = 8000;
export const VOICE_SOURCE_TONE_HZ = 1000;
// 置き換えた後と戻した後に、どちらの声の枠だけを鳴らして録るかと、そこで 1 kHz が聞こえるべきか。
// 置き換えている間は声を貸す人の声が画面共有している人の枠に入り、声を貸す人が前から持っていた枠は割り当てを残したまま無音になる
// (src-obs/pipeline/stream_router.h の AssignPolicy の (1)-(3))。戻すと逆になる。
export const VOICE_SOURCE_CHECKS = [
  { label: "vs-switched-owner", phase: "switched", keep: "owner", expectTone: true },
  { label: "vs-switched-from", phase: "switched", keep: "from", expectTone: false },
  { label: "vs-restored-owner", phase: "restored", keep: "owner", expectTone: false },
  { label: "vs-restored-from", phase: "restored", keep: "from", expectTone: true },
];
// 「鳴っていない」とみなす 1 kHz のブロックの割合の上限 (鳴っている側のしきい値 0.5 と、無音・部屋の音の 0 との間で、
// 1 kHz の残響や前後の切り替わりの数ブロックを鳴っていないに数える幅)。音量のしきい値は下の TONE_AUDIBLE_RMS / TONE_SILENT_RMS。
// 音のしきい値 (二乗平均平方根。0-1 の f32)。テスト用の送信側の 1 kHz は振幅 0.3 = 約 0.21。鳴っている = 0.02 (-34 dBFS) 以上で
// 1 kHz のブロックが半分以上、鳴っていない = 0.002 (-54 dBFS) 未満 (AAC のエンコードの揺れより上)。どちらでもない値は「判定できない」。
export const TONE_AUDIBLE_RMS = 0.02;
export const TONE_SILENT_RMS = 0.002;
export const TONE_HZ = 1000;
export const VOICE_SOURCE_QUIET_TONE_SHARE = 0.1;
export const VOICE_SOURCE_HEARD_TONE_SHARE = 0.5;
// (e) 整列のずれの傾きの上限 (時間に対する割合 %)。Firefox のストリームで約 1.1% (45 分で 24 秒) の傾きが出た例があり、
// 整列の修正後の合成の列の最悪は 0.0105%。0.05% はその最悪の約 5 倍で、1.1% の約 20 分の 1 (1 時間で 1.8 秒)。
// 傾きを読むのは、stats 行が ALIGN_DRIFT_MIN_SPAN_S 以上続いたストリームだけ (短いストリームは揺れで傾きが立つ)。
export const ALIGN_DRIFT_MAX_PERCENT = 0.05;
export const ALIGN_DRIFT_MIN_SPAN_S = 600;

// ---- キーを送る手順の種類 ----------------------------------------------------------------------
// 実行計画の中でキーを送る手順の種類 (ツールの中を読んで数えた。keySendingSteps を参照)。
//   dialog-sendkeys  保存ダイアログへファイルの場所と Enter を SendKeys で打つ (dialog-sendkeys.ps1。前面を確認するのは打ち始める前の 1 回だけ)
//   send-keys        名指したウィンドウへキーを送る (send-keys.ps1)
// 送らないもの: dock-click (UI Automation の Invoke / Toggle)、dialog-click (UI Automation のパターン。メニューを閉じる Esc は
// PostMessage でそのメニューのウィンドウへ直に送り、前面のキーの流れには入らない)、screenshot-window (PrintWindow)。
export const KEY_SENDING_KINDS = ["dialog-sendkeys", "send-keys"];

// cordmux が足す遅れの測り方: 同じ 1 つの録画に
// cordmux のソースと Discord (Vesktop) のウィンドウのキャプチャを並べて撮り、同じ変化 (共有の「時計つきの帯」の番号) が現れるフレームの差を読む。
// テスト用の OBS の Main の空いた象限 (画面の枠 3 つの 2x2 の右下) に Vesktop のウィンドウのウィンドウキャプチャを置き、Vesktop で
// 配信者の共有を大きく表示している間だけ録る (callLatencySteps)。読むのは latency.py (呼び出しの形は
// latencyToolArgs)。
// ウィンドウキャプチャの設定 (OBS 31.1.1 の plugins/win-capture/window-capture.c と libobs/util/windows/window-helpers.c を読んだ):
//   window    "<タイトル>:<クラス>:<実行ファイル>"。**クラスは空にできない** (decode_str は空のテキストを NULL にし、ms_find_window_top_level は
//             クラスが NULL ならウィンドウを探さない)。Electron のメインウィンドウのクラス Chrome_WidgetWin_1 を書き、タイトルは空にする (通話の名前や
//             サーバー名をテキストとして持たない)。
//   priority  2 = WINDOW_PRIORITY_EXE。実行ファイルが一致するウィンドウだけを候補にする (一致しないウィンドウの点は最大 = 選ばれない) ので、
//             vesktop.exe 以外のウィンドウは取り込まない。
//   method    2 = WGC (Chromium のウィンドウは BitBlt では黒くなる。自動でもクラス名で WGC を選ぶが、明示する)。
//   cursor / capture_audio  false (カーソルと Vesktop の音を録画に入れない)。
// メインの手順の 20 分の間ずっと置く方式にしないのは、Vesktop のウィンドウがメインの手順・av の録画と PNG の全部に入り、負荷の測定にも混ざるためである。
// 専用のシーンへ切り替えて大きく撮る方式にしないのは、ほかの枠が見えなくなり、製品の見えている枠の扱いがメインの手順と変わるためである。
export const LATENCY_RECORDING_LABEL = "latency";
export const LATENCY_INPUT_NAME = "cordmux-latency-reference";
export const LATENCY_WINDOW_CAPTURE_KIND = "window_capture";
export const LATENCY_WINDOW_CAPTURE_SETTINGS = {
  window: ":Chrome_WidgetWin_1:vesktop.exe",
  priority: 2,
  method: 2,
  cursor: false,
  capture_audio: false,
  client_area: true,
  force_sdr: false,
};
// ウィンドウキャプチャを置いてから録り始めるまでの猶予 (置くのは Vesktop を大きく表示して通知した後 = callLatencySteps。
// ウィンドウキャプチャがウィンドウを探し直す周期 WC_CHECK_TIMER の 1 秒 + WGC の立ち上がり + 余裕)。録る長さは 2 分 (60 fps で 7200 フレーム。メインの手順の後の定常の遅れの中央値と揺れを読むのに
// 十分で、人の待ちを 2 分しか増やさない)。
export const LATENCY_SETTLE_MS = 5000;
export const LATENCY_RECORD_MS = 2 * 60 * 1000;
// 目標 (ITU-R BT.1359-1 の検知限 45 ms と許容限 90 ms、音が先行する側)。超えても実行の中では止めず、
// 設計上の問題として記録する。
export const LATENCY_TARGET_DETECT_MS = 45;
export const LATENCY_TARGET_ACCEPT_MS = 90;
// ウィンドウキャプチャ自身の遅れ (WGC で 1 フレーム前後)。基準 (Vesktop のウィンドウ) の側も遅れて録られるので、測った差は本当の遅れより
// この分だけ小さく出る (偏り)。judgeLatency は測った値と、偏りを足した見積もりの両方を出し、しきい値は見積もりに当てる。
export const LATENCY_CAPTURE_BIAS_FRAMES = 1;
export const LATENCY_TOOL = "latency.py";
// ウィンドウキャプチャがウィンドウを捉えられなかったときの切り替え先 (画面キャプチャ)。method 1 = DXGI (Chromium のウィンドウの画像が取れる)、
// カーソルは入れない。画面は monitor_id の一覧から選ぶ (値にバックスラッシュが入るので、引数では渡さない)。切り替えの間だけ別の名前で置き、
// 画面が決まってからウィンドウキャプチャを外して LATENCY_INPUT_NAME に付け替える (選べなければ元のまま)。
export const LATENCY_MONITOR_CAPTURE_KIND = "monitor_capture";
export const LATENCY_MONITOR_CAPTURE_SETTINGS = { method: 1, capture_cursor: false };
export const LATENCY_MONITOR_TRIAL_NAME = "cordmux-latency-reference-monitor";

// 合否の基準。どれも「記録するだけのもの」(実行の中で止めない。gate にもしない)。実行の中で止めるのは、後の
// 確認が 1 つも取れなくなる前提 2 つ (--start-with-add の [Vesktopに追加] が state.json に届かない・起動し直した Vesktop が
// 接続しない。stopOnFail) だけ (JUDGE_LINES)。
//   OBS の出口と録画の中の映像と音のずれ: sync.py の audio_minus_video_ms (音の時刻 - 映像の時刻。負 = 音が先) の平均が
//   -AV_SYNC_AUDIO_LEAD_LIMIT_MS より大きく AV_SYNC_AUDIO_LAG_LIMIT_MS より小さい (ITU-R BT.1359-1 の検知限: 音が先 45 ms 未満・
//   音が遅れ 125 ms 未満)。
//   傾き: |傾き| が AV_TREND_DEFECT_MS_PER_MIN (1 ms/分) を超え、95% の幅が 0 を含まない (0 と区別できる) なら欠陥。超えなければ
//   「長い通話で確認するもの」として残す。ペアが AV_TREND_MIN_PAIRS 未満か、ペアの並びが AV_TREND_MIN_SPAN_S 秒に届かない録画では
//   傾きを判定しない (リハーサルの 30 秒の av の録画を失敗にしない)。
//   av2 - av1: 区間ごとの平均の差の絶対値が AV_DRIFT_DELTA_LIMIT_MS (20 ms) 未満。
export const AV_SYNC_AUDIO_LEAD_LIMIT_MS = 45;
export const AV_SYNC_AUDIO_LAG_LIMIT_MS = 125;
export const AV_TREND_DEFECT_MS_PER_MIN = 1.0;
export const AV_MIN_PAIRS = 10;
export const AV_TREND_MIN_PAIRS = AV_MIN_PAIRS;
export const AV_TREND_MIN_SPAN_S = 120;
export const AV_DRIFT_DELTA_LIMIT_MS = 20;
// 4 分割の付け外し: 3 つの時点の配置の行の label (callAc30Steps)。
export const AC30_ITEM_LABELS = { before: "ac30-before", afterRemove: "ac30-after-remove", afterRestore: "ac30-after-restore" };
// 声のミュートの往復: 録る間を声 1 人だけ鳴らし、区間ごとの音量を tone_scan.py --windows で読む。基準 (ミュートを外した区間)
// が鳴っていて、ミュートの区間は無音、外し直した区間は基準に戻る。戻りの許容幅 AC32_GAIN_TOLERANCE_DB は、Discord の音声の
// エンコードと OBS のミックスの揺れを見込んだ幅である。区間の両端は AC32_WINDOW_MARGIN_S ずつ削る (ミュートを変えた所の切り替わりと、録画の時刻の
// 原点のずれを数えない)。**音の出る声は 1 人だけの前提** (Chrome のテスト用のマイクの 1 kHz。ほかの人はマイクをミュート)。
// 基準の区間が鳴っていない人は「判定できない」。
export const AC32_GAIN_TOLERANCE_DB = 1.5;
export const AC32_WINDOW_MARGIN_S = 0.5;
export const AC32_TONE_HZ = 1000;
export const AC32_SEGMENT_NAMES = ["base", "muted", "restored"];
// 使う声の確認 (--extra-checks の (c)) の tone_scan.py の区間 (8 秒の録画の先頭と末尾を 0.5 秒ずつ削る)。tone_scan.py は
// --windows のときだけ tone_share を出す (ストリームごとの rms は Hann ウィンドウ関数を掛けた後の値で、区間の rms とは違う)。
export const VOICE_SOURCE_SCAN_MARGIN_S = 0.5;
// 声の音量 (--extra-checks の (c) の続き): 使う声を置き換えた後の画面共有している人の枠 (声を貸す人の 1 kHz が鳴る枠。VOICE_SOURCE_CHECKS の
// 最初の項目と同じ、画面共有している人の声だけを鳴らす条件) の音量を、[音声] タブの声のつまみで 200% にしてもらい、録画の音量の比を
// 置き換えの確認の録画 (100%) と比べる。ゲインは percent / 100 の直線 (src-obs/config/person_audio.h の gain_of_person_volume) なので、
// 200% は +20 log10(2) = +6.02 dB。許容幅は声のミュートの往復 (AC32_GAIN_TOLERANCE_DB) と同じ 1.5 dB: 同じ 8 秒の区間を同じ経路
// (Discord のエンコード -> cordmux -> OBS のミックス -> 録画) で 2 回読んだ比で、幅の根拠もその経路の揺れだからである。実際の録画で揺れが
// この幅に収まるかは、最初の実際の実行で初めて分かる (収まらなければここを修正する)。基準の音が大きく、200% で 1.0 を超えて切れる
// おそれがあるとき (正弦波の実効値 x 2 x sqrt(2) >= 1) は、比が縮んで読めるので判定しない。
export const VOICE_VOLUME_PCT = 200;
export const VOICE_VOLUME_GAIN_TOLERANCE_DB = AC32_GAIN_TOLERANCE_DB;
export const VOICE_VOLUME_BEFORE_LABEL = VOICE_SOURCE_CHECKS[0].label;
export const VOICE_VOLUME_AFTER_LABEL = "vs-volume-200";
export const VOICE_VOLUME_CLIP_RMS = 1 / (2 * Math.SQRT2);
// 音量を変えてから録るまでの猶予 (行の送りの間隔と、OBS の混ぜに利得が届くまで)。
export const VOICE_VOLUME_SETTLE_MS = 3000;

// 利用者の Vesktop の中の Vencord の設定ファイル (Vesktop の DATA_DIR/settings/settings.json。seed-config.py の
// user_vesktop_files が偽の保存先に作るのと同じ形)。--start-with-add の実際の実行は [Vesktopに追加] の前に結果フォルダ (_work の
// 下) へコピーし (読むだけ・コピーするだけ)、締めで Vesktop を終了してもらった後に現在の設定と鍵の単位で比べて違いを記録する。**戻さない**
// (戻すのは利用者の同意を得てから、テストを実行する人がコピーで行う)。v1.15.11 の Vencord は起動のときに設定の鍵を移す
// (migratePluginSettings) ので、通常の Vencord へ戻った後に設定が変わって見えることがある。
// 結果には鍵のパスと変わり方だけを書き、値は書かない (プラグインの設定に人の ID が入りうる)。コピー自体は ID の入りうるものとして、
// 結果の保存先へコピーしない (ID_BEARING_FILE_PATTERNS)。
export const VENCORD_SETTINGS_REAL_REL = "Vesktop/settings/settings.json";
export const VENCORD_SETTINGS_BEFORE_NAME = "vencord-settings-before.json";
export const VENCORD_SETTINGS_AFTER_NAME = "vencord-settings-after.json";
// アンインストールの後に取る比較用のコピー。アンインストールは Vencord の設定から cordmux の項目を消すので、実行の開始時のコピーと変わらなければ
// 利用者の設定を壊していない。違いがあれば失敗にする (Vesktop を終了した直後の比較は記録するだけで、こちらは判定する)。
export const VENCORD_SETTINGS_AFTER_UNINSTALL_NAME = "vencord-settings-after-uninstall.json";

// 実行スクリプトの外から実行を穏やかに止める通知ファイル (<出力>/window3b/stop-run.txt)。置くと次の手順の境目か待ちの 1 秒の
// 刻みで手順を抜け、finally (OBS を閉じる・後始末・vencordDir を戻す) を通る。node のプロセスツリーを
// taskkill /F で止めると finally を通らず、vencordDir を手で戻す必要があるため、taskkill は通知ファイルで止まらないときの最後の手段に残す。
export const STOP_FILE_NAME = "stop-run.txt";
// finally が書く vencordDir の最後の確認 (<出力>/window3b/vencord-dir-final-<tag>.json)。手順書の「元に戻りました」は
// これと実際の state.json を読んでから書く。
export const VENCORD_DIR_FINAL_PREFIX = "vencord-dir-final-";

// 配信者の Discord のユーザー ID は結果 JSON (args) とログに書かない (REDACTED_BROADCASTER_ID に
// 置き換える)。消せないもの (テスト用の OBS の cordmux の settings.json、Vencord の設定のコピー、
// OBS のログ) は名前で分かるようにし、結果の保存先へコピーしない (id-scan.mjs がコピーした後に 17-20 桁の数を数えて 0 を確認する)。
export const REDACTED_BROADCASTER_ID = "<配信者の ID>";
export const ID_BEARING_FILE_PATTERNS = [
  VENCORD_SETTINGS_BEFORE_NAME,
  VENCORD_SETTINGS_AFTER_NAME,
  VENCORD_SETTINGS_AFTER_UNINSTALL_NAME,
  "latency-reference",
  "obs-logs-",
  "plugin_config",
];

// rt-<tag> の下の保存先 (repo 直下からの相対)。**この一式は Node の API を呼ばない**ので、パスは文字列で組む。
export function roundtripPaths(tag) {
  const work = ROUNDTRIP_WORK_PREFIX_REL + tag;
  return {
    workRel: work,
    obsRel: work + "/obs",
    obsMovedRel: work + "/obs-moved",
    appDataRel: work + "/appdata",
    localAppDataRel: work + "/localappdata",
    handshakeRel: work + "/localappdata/cordmux",
    zipDirRel: work + "/zip",
    oldZipDirRel: work + "/zip-old",
    customVencordRel: work + "/custom-vencord/dist",
  };
}

// src-obs/buildspec.json から obs-deps の bin の場所を導く ("obs-studio-31.1.1" などの
// 文字列を buildspec の 1 か所から導く)。読めなければ null。
export function obsDepsBinRelFromBuildspec(buildspec) {
  const deps = buildspec && buildspec.dependencies;
  const obsVersion = deps && deps["obs-studio"] && deps["obs-studio"].version;
  const prebuilt = deps && deps.prebuilt && deps.prebuilt.version;
  if (typeof obsVersion !== "string" || typeof prebuilt !== "string" || !obsVersion || !prebuilt) return null;
  return "src-obs/.deps/obs-studio-" + obsVersion + "/.deps/obs-deps-" + prebuilt + "-x64/bin";
}

// ---- parseArgs -------------------------------------------------------

// 表示のバッファ (ms) の上限。製品の settings.h の display_buffer_ms の clamp と同じ値。
export const DISPLAY_BUFFER_MS_MAX = 1000;

function toIntArg(value, flag, errors) {
  const n = Number(value);
  if (!Number.isFinite(n) || !Number.isInteger(n) || n < 0) {
    errors.push(flag + " は 0 以上の整数 (" + value + " は不正)");
    return 0;
  }
  return n;
}

// ---- --step hold の値 ----------------------------------------
// 開いたまま待つ長さ (分)。既定は 30 で、1 つの区間の所要時間 (10-12 分) に余裕を足した見込みである。
// 上限は実行の期限 (--deadline-min、既定 120) から閉じる・後始末の分を引いた値までで、
// parseArgs が確認する。待ちは通知ファイル (human の go-file) でいつでも終えられるので、長めでよい。
export const HOLD_DEFAULT_MINUTES = 30;
export const HOLD_MAX_MINUTES = 110;
// 期限の中に残す、閉じる・ログの回収・後始末の分 (見込み。close-obs の 30 秒 + cleanup + summarize)。
export const HOLD_CLOSE_MARGIN_MIN = 10;
// テスト用の送信側を接続する実行 (--n-screen / --n-voice を渡したとき) の寿命の余裕 (秒)。送信側が待ちの途中で自分で止まると、
// 撮る画像から人が消える。待ちは通知で早く終わるので、終わった後は harness-stop が止める。
export const HOLD_HARNESS_MARGIN_S = 10 * 60;
// hold の実行が開いている間の目印のファイル (OBS の pid・obs-websocket のポート・通知ファイル。テストを実行する人が -Pid に渡す)。
export const HOLD_INFO_FILE_PREFIX = "hold-";
// hold の --self-mic (自分の枠の音声ソースのしきい値を測る短い実行。録画しない): OBS が起動したら obs-websocket で音声入力キャプチャ
// (既定のデバイス) を録画するシーン (WIRE_SCENE_NAME) に作り、cordmux の settings.json の self_speaking_source_name を
// その名前にして始める (seed-config.py の --self-speaking-source-name。しきい値は書かず、製品の既定の値で測る)。シーンに置くのは、
// 配信に出ている扱いにするため (製品は画面に出ていない入力を鳴っていないと見る)。OBS は音を鳴らさない: 入力の監視は「監視なし」に
// し、プロファイルの MonitoringDeviceId は seed-config.py が disabled で書く。名前は OBS の既定の入力 (マイク/補助音声) と重ならない文字列にする。
export const SELF_MIC_INPUT_NAME = "マイク";
export const SELF_MIC_INPUT_KIND = "wasapi_input_capture";
export const SELF_MIC_DEVICE_ID = "default";
export const SELF_MIC_MONITOR_TYPE = "OBS_MONITORING_TYPE_NONE";
// 入力を作ってから製品がそれを見つけるまでの猶予 (ドックの周期 500 ms ごとに探し直す。余裕を見て 5 秒)。
export const SELF_MIC_FOUND_WAIT_MS = 5000;
// テスト用の送信側のフレームだけを止める通知ファイル。実行スクリプトは hold の実行の送信側に
// <out>/window3b/frames-pause-<tag>.flag の場所を環境変数 HARNESS_FRAMES_PAUSE_ENV で渡し、hold-info の framesPauseFile に
// 書く。ファイルがある間、送信側はエンコードしたフレームを送らない (接続・ハートビート・声は保つ。読むのは test/tools/harness の側)。
// 実行スクリプトは送信側を起動する前と実行の終わり (finally) にファイルを消す (前の実行の通知ファイルで止まったまま始めない)。
// obs-websocket や送信側への別の経路で止める方式にしないのは、送信側が待ち受けを持たないためである。
// ファイルなら、テストを実行する人が New-Item / Remove-Item の 1 行で出し入れできる。
export const HARNESS_FRAMES_PAUSE_ENV = "HARNESS_FRAMES_PAUSE_FILE";
export const FRAMES_PAUSE_FILE_PREFIX = "frames-pause-";
export const FRAMES_PAUSE_FILE_SUFFIX = ".flag";

// ---- --step call-hold の値 ------------------------------------------
// 期限の見積もりに足す人の遅れ (分)。call-hold の人の操作は [Vesktopに追加]・開き直す・閉じる・アンインストール・
// 開き直すの 5 回で、call-1 (約 10 回) の半分なので CALL_ONE_HUMAN_OVERRUN_MIN の半分とする。
export const CALL_HOLD_HUMAN_OVERRUN_MIN = 10;
// リハーサル (--rehearsal) のテスト用の送信側の既定のストリーム数 (--n-screen / --n-voice を渡さないとき)。画面 1 ストリームで、
// 共有・止める・閉じるの手順の代わりにツールを実行できる。
export const CALL_HOLD_REHEARSAL_N_SCREEN = 1;
export const CALL_HOLD_REHEARSAL_N_VOICE = 0;

// ---- Vesktop の終了と起動を依頼する文 ---------------------------------
// Vesktop の設定「Minimize to tray」は既定で有効 (settings.json も minimizeToTray: true) で、ウィンドウの
// 閉じるボタン (右上の X) ではトレイに残り Vesktop.exe が動き続ける。そのため「閉じて、開き直して」では (1) call-hold の
// wait-connection が待つ新しい hello が来ず (読み直した Vencord が接続しない) 3 分で失敗になり、(2) 締めのアンインストールは製品が
// Vesktop.exe の名前で動いていると見て断る (uninstall_vesktop_running。platform_win.cpp の find_vesktop_process)。終了の方法は
// 通知領域のアイコンのメニューの [Quit] (メニューは Open・About・Repair Vencord・Reset Vesktop・Restart・Quit。英語だけで訳がないので
// 文字列はそのまま)。[Restart] で 1 手にしないのは、締めは起動し直さないためである。
// 「Vesktopを終了してください」だけにしないのは、どうすれば終了かを言わないと、X で閉じた人は待ちのタイムアウトまで気づかないためである。
// Windows は既定で多くの通知領域のアイコンを隠すので、[Quit] を依頼する文に、手順書 (guide/ja.md
// の「インストール」の手順3) と同じテキストの 1 文 VESKTOP_HIDDEN_ICON_HINT を添える (テキストを 1 か所に持ち、手順書のテキストとの一致は run-window-call-hold.test.ts が
// 確認する)。追加する位置は [Quit] の文の直後 (アイコンを探す所で読める)。括弧の注記がある文は注記の後 (注記を前の文に付けたまま読ませる)。
// 「時計の左にある「^」の印 (隠れているインジケーターを表示します)」の言い方にしないのは、手順書とテキストが違い、覚える形が 2 つになるためである。
// 括弧に入れて [Quit] の文の中へ入れないのは、文が長くなり、何を選ぶかの読みが遅れるためである。
//
// [Vesktopに追加] は Vesktop の起動中は押せない (製品の ledger/actions.cpp の vesktop_busy_reason。起動中の Vesktop は state.json を
// メモリの値で書き直し、書いた値が消える)。そのため、追加の依頼は「Vesktop を終了 -> [Vesktopに追加] -> (確認のウィンドウ)」を 1 つの
// 依頼にし (vesktopQuitAndAddMessage)、起動は追加の判定の後に別の依頼にする (VESKTOP_START_AFTER_ADD_MESSAGE)。手順書の
// インストールの 3 と同じ並び (終了 -> 追加 -> 起動) である。終了と追加を 1 つの依頼にするのは、追加の依頼の待ち (state.json が
// 配置物を指すまで) が、終了を済ませたことも含めて見ているためで、人に依頼する回数を増やさない。
export const VESKTOP_HIDDEN_ICON_HINT = "アイコンが見当たらないときは、通知領域の [^] を押すと表示されます";
export function vesktopQuitAndAddMessage() {
  return (
    "タスクバーの右端の通知領域にあるVesktopのアイコンを右クリックし、[Quit] を選んで、Vesktopを終了してください。" +
    VESKTOP_HIDDEN_ICON_HINT +
    "。Vesktopが終了したら、テスト用のOBSのドックにある [" + textRef(TEXT_ID_ADD_TO_VESKTOP) + "] を押してください" +
    " (Vesktopの起動中は押せず、ボタンの下に理由が出ます)。確認のウィンドウが出たら [" + textRef(TEXT_ID_DIALOG_SWITCH_BUTTON) + "] を押してください"
  );
}
export const VESKTOP_START_AFTER_ADD_MESSAGE = "Vesktopを起動してください (cordmuxを読み込んで起動します)";
export const VESKTOP_HANG_UP_AND_QUIT_MESSAGE =
  "画面共有を止めて通話を切り、通知領域のVesktopのアイコンを右クリックして [Quit] を選んでください (テスト用のOBSのウィンドウはまだ閉じないでください)。" +
  VESKTOP_HIDDEN_ICON_HINT;
// 実際のアバターの並びを撮る実行 (rosterCheckPlanned) は、締めの「共有を止めて通話を切り、Vesktop を [Quit]」を 2 つに分ける:
// 先に共有を止めてもらい (通話にはまだ居る)、並びを撮ってから、通話を切って Vesktop を終了してもらう。
export const VESKTOP_STOP_SHARING_MESSAGE = "画面共有をしているアカウントは、画面共有を止めてください (ボイスチャンネルからは、まだ抜けないでください)";
export const VESKTOP_LEAVE_AND_QUIT_MESSAGE =
  "4つのアカウントすべてでボイスチャンネルから抜け、通知領域のVesktopのアイコンを右クリックして [Quit] を選んでください (テスト用のOBSのウィンドウはまだ閉じないでください)。" +
  VESKTOP_HIDDEN_ICON_HINT;
export const VESKTOP_START_AFTER_UNINSTALL_MESSAGE = "Vesktopを起動してください (元の設定で動きます)";
// --av-only の締め: 測り終えたら、共有を止めて全員で抜け、Vesktop を終了してもらう。アンインストールは実行スクリプトがドックから押す
// (ウィンドウを押すので、その間はテスト用の OBS に触らないでもらう)。
// 終了の返事からアンインストールを押すまでの猶予 (callUninstallSteps の --av-only)。
export const AV_ONLY_QUIT_SETTLE_MS = 10000;
export const AV_ONLY_LEAVE_AND_QUIT_MESSAGE =
  "測り終えました。画面共有を止めて、4つのアカウントすべてでボイスチャンネルから抜け、通知領域のVesktopのアイコンを右クリックして [Quit] を選んでください。" +
  VESKTOP_HIDDEN_ICON_HINT +
  "。終わったら「OK」と書いてください。その後、こちらがテスト用のOBSでcordmuxのアンインストールを行います (1分ほど)。テスト用のOBSのウィンドウには触らないでください";
// call-1 の最初の依頼: 実行を始める前に Vesktop を終了している形が前提で、まだ動いていれば同じ [Quit] の方法で終了してから起動する
// (「まだ開いていれば閉じてから」では X で閉じる読みになるため)。
// ---- hold と call-hold の配信の受け口 ------------------------------------------
// 「Vesktopとの接続が%1に切れました」のメッセージは、OBS が配信中で枠に人がいるときだけ出る (rules.cpp の (9)、
// observation.h の streaming_with_slotted_person)。配信中と配信していないときの両方の形を見るには、テスト用の OBS を
// 配信中にできる受け口が要る。--stream-sink を渡すと、hold-info の前に実行スクリプトと同じ受け口 (ffmpeg -listen 1 で STREAM_SINK_URL を受け、
// -c copy で <out>/window3b/stream-<tag>.flv へ) を起動し、実行の終わり (OBS を閉じた後) と finally で止める。配信の開始と停止は
// テストを実行する人が obs-websocket の StartStream / StopStream で行う。接続先は seed-config.py の test の形の service.json (rtmp://127.0.0.1:1935/live と
// キー test) と同じ 127.0.0.1 だけで、インターネットへは出ない。
// ffmpeg -listen 1 は送信側が切ると終了コード 0 で終わる (同梱の ffmpeg n7.1.1 同士で確認した。OBS の切り方で 0 になるかは
// 実機で初めて分かる) ので、hold と call-hold の受け口は、0 で終わるか flv に書いて終わるたびに次のファイル (stream-<tag>-<n>.flv) で
// 開き直す (STREAM_SINK_MAX_REARMS まで。判定は stream-sink.mjs の shouldRearm)。
// 1 回きりの受け口にしないのは、最初の StopStream の後の 2 回目の StartStream に受け手がなく、テスト用の OBS が接続失敗のダイアログを出すためである。
// 受け口を手で起動する方式にしないのは、cleanup.ps1 が _work の外の ffmpeg を止めないので閉じ損ねが残りうるためである。実行スクリプトの受け口は
// 実行スクリプト (node) と一緒に終わる (node を外から打ち切っても libuv のジョブで 5-17 ms に終わることを測った)。手で起動した受け口は、
// 同梱の ffmpeg で起動したシェルが閉じた後なら stream-sink-stale.ps1 が「親がいない受け口」として見つけて伝えるが、
// cleanup.ps1 も実行スクリプトも止めない (止めるのはテストを実行する人が -Stop を手で実行するときだけ)。PATH の ffmpeg なら場所が違うので見つけもしない。
export const STREAM_SINK_PORT = 1935;
export const STREAM_SINK_URL = "rtmp://127.0.0.1:" + STREAM_SINK_PORT + "/live/test";
export const STREAM_SINK_STEPS = ["hold", "call-hold"];
// 開き直しの上限 (配信の開始と停止は数回の見込みで、異常に早く終わり続けるときにループを止める)。
export const STREAM_SINK_MAX_REARMS = 20;

export const VESKTOP_START_FOR_CALL_ONE_MESSAGE =
  "Vesktopを起動してください (まだ起動したままなら、通知領域のVesktopのアイコンを右クリックして [Quit] を選び、終了してから起動してください。" +
  VESKTOP_HIDDEN_ICON_HINT +
  ")";

// ---- --recheck の人への文 (Vesktop の再起動と、足した 6 つの確認) ----------------------------------------------------
// どの文も、それだけ読んで分かる形 (現在の状態・操作・気をつけること。前の文や手順を指さない)。待つタイムアウトは HUMAN_TIMEOUT_MS
// (30 分) で、ほかの文と同じ。
// 次の案内が長く出ないときの案内 (文の最後に付ける)。通話から抜けて Vesktop を終了し、テスト用の OBS のウィンドウが消えてから
// 1 時間たって Vesktop を起動する (実行スクリプトは OBS を閉じた後に判定を実行し、その後に vencordDir を戻す。消えた直後に起動すると
// 戻す前の設定で Vesktop が動き、戻した値を上書きしうる)。期限の約 3 時間 (CALL_ONE_DEADLINE_MIN) を過ぎてもウィンドウが消えないのは
// 実行スクリプトが止まった場合で、その分岐の案内 (cleanup.ps1 で Vesktop の設定を戻す。test/tools/obs-run/README.md の「後片付け (`cleanup.ps1`)」) も添える。
export const CALL_CONTACT_LOST_GUIDANCE =
  "40分たっても次の案内が出ないときは、画面共有を止めて通話から抜け、" +
  "通知領域のVesktopのアイコンを右クリックして [Quit] で終了してください。テストのために起動したブラウザーも閉じて構いません。" +
  "テスト用のOBSは、通話の確認を始めてから長くても約3時間で自動的に閉じます。ウィンドウが消えてから1時間たったら、Vesktopを起動してください " +
  "(Vesktopの設定は、ウィンドウが消えた後に元に戻ります)。ただし、通話の確認を始めてから3時間半たってもウィンドウが消えないときは、" +
  "Vesktopを起動せずに、テスト用のOBSのウィンドウを閉じてください。そのうえで、cleanup.ps1 を -CallWindow -RealProfile を付けて実行し、" +
  "Vesktopの設定を元に戻してください (test/tools/obs-run/README.md の「後片付け (`cleanup.ps1`)」)。";
// 文の中のボイスチャンネルの名前 (呼び出し行の --voice-channel。渡されていなければ名前を挙げない)。
function channelQuoted(voiceChannel) {
  return voiceChannel ? "ボイスチャンネル「" + voiceChannel + "」" : "指定のボイスチャンネル";
}
// --recheck で、[Vesktopに追加] を押した後に Vesktop を起動させる文 (追加の依頼で Vesktop を終了している)。[Vesktopに追加] は
// Vesktop の起動中は押せないので、前の形 (起動したまま追加して、通知領域の [Restart] で再起動する) はできない
// (VESKTOP_HIDDEN_ICON_HINT の前のコメント)。Vesktop はログインを覚えているので、起動するとログインした状態の画面に戻る。
export function vesktopStartAfterAddMessage(accounts = DEFAULT_CALL_ACCOUNTS) {
  return (
    "Vesktop (" + accounts.receiver + ") を起動して、cordmuxを読み込ませます ([" + textRef(TEXT_ID_ADD_TO_VESKTOP) + "] を押す前に終了したままです)。\n" +
    "Vesktopが起動して、ログインした状態の画面になったら「OK」と書いてください。その後、次のお願いまで3分ほどお待ちください。\n" +
    CALL_CONTACT_LOST_GUIDANCE
  );
}
// エンコードの設定を替えた後に、Firefox のアカウントの画面共有を始め直してもらう文。
export function encodeRestartShareMessage(voiceChannel, accounts = DEFAULT_CALL_ACCOUNTS) {
  return (
    "映像の符号化の設定を、テスト用のOBSから切り替えました (" + textRef(TEXT_ID_SETTINGS_VIDEO_ENCODER) + ": " + textRef(TEXT_ID_SETTINGS_VIDEO_ENCODER_SOFTWARE) +
    "、" + textRef(TEXT_ID_SETTINGS_ENCODE_PRIORITY) + ": " + textRef(TEXT_ID_SETTINGS_ENCODE_PRIORITY_QUALITY) + ")。" +
    "確認するために、Firefox (" + accounts.firefox + ") の画面共有を、一度止めてから始め直してください。\n" +
    channelQuoted(voiceChannel) + "からは抜けず、Chrome (" + accounts.chrome + ") とデスクトップ版のDiscord (" + accounts.main + ") の画面共有と、Vesktopはそのままにしてください。\n" +
    "始め直したら「OK」と書いてください。その後、次のお願いまで1分ほどお待ちください。テスト用のOBSのウィンドウには触らないでください。\n" +
    CALL_CONTACT_LOST_GUIDANCE
  );
}
// 自分の枠: デスクトップ版の Discord (メインのアカウント) に画面共有を始めてもらう文。
export function selfFrameShareStartMessage(voiceChannel, accounts = DEFAULT_CALL_ACCOUNTS) {
  return (
    "自分の枠を確認します。いま、4つのアカウントが" + channelQuoted(voiceChannel) + "に入っていて、Chrome (" + accounts.chrome + ") とFirefox (" + accounts.firefox + ") が画面共有をしています。\n" +
    "デスクトップ版のDiscord (" + accounts.main + ") で、画面共有を始めてください (" + channelQuoted(voiceChannel) + "には入ったままで、マイクはミュートのままです)。\n" +
    "始めたら「OK」と書いてください。その後、次のお願いまで1分ほどお待ちください。ほかのアカウントは、そのままにしてください。テスト用のOBSのウィンドウには触らないでください。\n" +
    CALL_CONTACT_LOST_GUIDANCE
  );
}
// 自分の枠: メインのアカウントの Discord に画面共有を止めてもらう文。
export function selfFrameShareStopMessage(voiceChannel, accounts = DEFAULT_CALL_ACCOUNTS) {
  return (
    "いま、デスクトップ版のDiscord (" + accounts.main + ") が画面共有をしています。\n" +
    "デスクトップ版のDiscord (" + accounts.main + ") の画面共有を止めてください。" + channelQuoted(voiceChannel) + "からは抜けないでください。\n" +
    "止めたら「OK」と書いてください。その後、次のお願いまで1分ほどお待ちください。ほかのアカウントは、そのままにしてください。テスト用のOBSのウィンドウには触らないでください。\n" +
    CALL_CONTACT_LOST_GUIDANCE
  );
}
// スピーカーミュートと Vesktop の最小化: 受け取りが続くかを見る文。
export function speakerMuteMinimizeMessage(voiceChannel, accounts = DEFAULT_CALL_ACCOUNTS) {
  return (
    "いま、4つのアカウントが" + channelQuoted(voiceChannel) + "に入っています。\n" +
    "デスクトップ版のDiscord (" + accounts.main + ") で、スピーカーをミュートにしてください (スピーカーのアイコンを押します。マイクではありません)。" +
    "続けて、Vesktop (" + accounts.receiver + ") のウィンドウを最小化してください。\n" +
    "ほかのアカウントは、そのままにしてください。両方できたら「OK」と書いてください。" +
    "その後、約1分間は、どちらもそのままにしてください (私がテスト用のOBSで、映像が届き続けているかを確認します)。テスト用のOBSのウィンドウには触らないでください。\n" +
    CALL_CONTACT_LOST_GUIDANCE
  );
}
// 3 つ元に戻し、Vesktop だけ通話から抜けてもらう文。
export function speakerUnmuteLeaveMessage(voiceChannel, accounts = DEFAULT_CALL_ACCOUNTS) {
  return (
    "いま、デスクトップ版のDiscord (" + accounts.main + ") のスピーカーはミュートで、Vesktop (" + accounts.receiver + ") のウィンドウは最小化されています。\n" +
    "3つ元に戻します。(1) デスクトップ版のDiscord (" + accounts.main + ") のスピーカーのミュートを解除してください。(2) Vesktopのウィンドウを元の大きさに戻してください。" +
    "(3) Vesktop (" + accounts.receiver + ") だけ、" + channelQuoted(voiceChannel) + "から抜けてください (Vesktopは終了しません。通知領域のアイコンには触れません)。\n" +
    "できたら「OK」と書いてください。その後、次のお願いまで1分ほどお待ちください。ほかのアカウントは、そのままにしてください。テスト用のOBSのウィンドウには触らないでください。\n" +
    CALL_CONTACT_LOST_GUIDANCE
  );
}
// Vesktop に通話へ入り直してもらう文。
export function vesktopRejoinMessage(voiceChannel, accounts = DEFAULT_CALL_ACCOUNTS) {
  return (
    "いま、Vesktop (" + accounts.receiver + ") は" + channelQuoted(voiceChannel) + "から抜けた状態で、ほかのアカウントは通話に参加したままです。\n" +
    "Vesktop (" + accounts.receiver + ") で、もう一度" + channelQuoted(voiceChannel) + "に入ってください (マイクはミュートのままです)。画面共有は始めなくて構いません。\n" +
    "入ったら「OK」と書いてください。その後、次のお願いまで1分ほどお待ちください。テスト用のOBSのウィンドウには触らないでください。\n" +
    CALL_CONTACT_LOST_GUIDANCE
  );
}
// テスト用の送信側の画面ストリームごとの最初のサイズ (HARNESS_SCREEN_SIZES。書式を読むのは test/tools/harness/sender.ts の
// parseScreenSizes)。"<幅>x<高さ>" をカンマで並べる。
export const SCREEN_SIZES_RE = /^[1-9][0-9]{1,4}x[1-9][0-9]{1,4}(,[1-9][0-9]{1,4}x[1-9][0-9]{1,4})*$/;

// ---- 実際の OBS の画面を撮るための hold の旗 ---------------------------
// README の画像のように、実際の OBS の画面に名前ラベル付きの 4 分割を出して撮るときに使う。どれも --step hold だけで受ける。
//   --screen-names    テスト用の送信側の画面ごとの表示名 (HARNESS_SCREEN_NAMES。読むのは test/tools/harness/sender_pure.ts の
//                     parseScreenNames)
//   --screen-images   画面ごとに描く画像のファイル (HARNESS_SCREEN_IMAGES。parseScreenImages。ファイルを読むのは
//                     test/tools/harness/screen_image_file.js)
//   --voice-users     声のストリームごとの持ち主の userId (HARNESS_VOICE_USERS。parseVoiceUsers)。渡すと、声に音を乗せる通知ファイルの
//                     場所も決めて送信側に渡し、hold-<tag>.json の voiceToneFile に書く (置くと声が鳴り、その人の枠が光る)
//   --label-em-ratio-pct  cordmux の settings.json の名前の大きさ (seed-config.py が書く)
//   --wire-composite  接続の後に、合成のソースを Main に置く (置かないと OBS のプレビューに何も映らない)
// 送信側の側でも同じ規則で読むが、読めない項目を黙って既定に戻すので、OBS を起動する前にここで断る (撮り直しで OBS を
// 起動し直さないため)。
export const HOLD_SCREEN_NAME_MAX = 64;
export const VOICE_USER_ID_RE = /^[A-Za-z0-9_-]{1,64}$/;
// テスト用の送信側が既定で付ける声の userId。--voice-users に書くと、ほかの声の既定と重なりうるので断る。
const DEFAULT_VOICE_USER_RE = /^harness-voice-[0-9]+$/;
const HARNESS_SCREEN_USER_RE = /^harness-screen-([0-9]+)$/;
// 画像の形式は Chromium の createImageBitmap が読めるもののうち、README の画像の材料に使うものだけを受ける。
export const SCREEN_IMAGE_EXTENSIONS = [".png", ".jpg", ".jpeg"];
// 名前の大きさの範囲と刻み (src-obs/config/settings.h の kLabelEmRatioMinPct と kLabelEmRatioMaxPct。0.1 刻み)。
export const LABEL_EM_RATIO_PCT_MIN = 1.0;
export const LABEL_EM_RATIO_PCT_MAX = 8.5;
// 声に音を乗せる通知ファイル (test/tools/harness の CORDMUX_HARNESS_VOICE_TONE_FILE)。置き方と消し方はフレームを止める
// 通知ファイルと同じで、場所は <out>/window3b/voice-tone-<tag>.flag。中身が空なら全部の声、"5" や "5,6" ならその番号の声に音を乗せる。
export const HARNESS_VOICE_TONE_ENV = "CORDMUX_HARNESS_VOICE_TONE_FILE";
export const VOICE_TONE_FILE_PREFIX = "voice-tone-";
export const VOICE_TONE_FILE_SUFFIX = ".flag";

function commaItems(value) {
  return String(value ?? "").split(",").map((item) => item.trim());
}

/** --screen-names の値の誤り (なければ null)。どの項目も空でなく、制御文字を含まず、HOLD_SCREEN_NAME_MAX 文字まで。 */
export function screenNamesProblem(value) {
  for (const item of commaItems(value)) {
    if (item.length === 0) return "--screen-names は表示名をカンマで並べる (空の項目がある: " + value + ")";
    if (Array.from(item).length > HOLD_SCREEN_NAME_MAX) return "--screen-names の表示名は " + HOLD_SCREEN_NAME_MAX + " 文字まで (" + item + ")";
    if (Array.from(item).some((ch) => ch.codePointAt(0) < 0x20 || ch.codePointAt(0) === 0x7f)) return "--screen-names の表示名に制御文字がある";
  }
  return null;
}

/** --screen-images の値の誤り (なければ null)。どの項目も空でなく、拡張子が SCREEN_IMAGE_EXTENSIONS のどれか。 */
export function screenImagesProblem(value) {
  for (const item of commaItems(value)) {
    if (item.length === 0) return "--screen-images は画像のファイルをカンマで並べる (空の項目がある: " + value + ")";
    const lower = item.toLowerCase();
    if (!SCREEN_IMAGE_EXTENSIONS.some((ext) => lower.endsWith(ext))) {
      return "--screen-images の画像は " + SCREEN_IMAGE_EXTENSIONS.join(" / ") + " のファイル (" + item + ")";
    }
  }
  return null;
}

/** --voice-users の値の誤り (なければ null)。どの項目も VOICE_USER_ID_RE に合い、重ならず、既定の harness-voice-<番号> でない。 */
export function voiceUsersProblem(value) {
  const seen = new Set();
  for (const item of commaItems(value)) {
    if (!VOICE_USER_ID_RE.test(item)) return "--voice-users の userId は英数字と _ と - の 64 文字まで (" + (item || "空") + ")";
    if (DEFAULT_VOICE_USER_RE.test(item)) return "--voice-users に既定の userId (" + item + ") は書かない (ほかの声の既定と重なる)";
    if (seen.has(item)) return "--voice-users に同じ userId が 2 回ある (" + item + ")";
    seen.add(item);
  }
  return null;
}

/**
 * --voice-users のうち、テスト用の送信側の画面の人を指すのに、その番号の画面がないもの (nScreen を超える harness-screen-<n>)。
 * 書き損じると、だれの枠も光らない声だけの人になる。
 */
export function voiceUsersBeyondScreens(value, nScreen) {
  return commaItems(value).filter((item) => {
    const m = HARNESS_SCREEN_USER_RE.exec(item);
    return m !== null && (Number(m[1]) < 1 || Number(m[1]) > nScreen);
  });
}

/** --label-em-ratio-pct の値を読む。{ value, error }。範囲は LABEL_EM_RATIO_PCT_MIN から MAX、0.1 刻み (製品は刻みに合わない値を読まない)。 */
export function parseLabelEmRatioPct(text) {
  const raw = String(text ?? "").trim();
  if (!/^[0-9]+(\.[0-9])?$/.test(raw)) return { value: null, error: "--label-em-ratio-pct は 0.1 刻みの数 (" + raw + " は不正)" };
  const value = Number(raw);
  if (value < LABEL_EM_RATIO_PCT_MIN || value > LABEL_EM_RATIO_PCT_MAX) {
    return { value: null, error: "--label-em-ratio-pct は " + LABEL_EM_RATIO_PCT_MIN.toFixed(1) + " から " + LABEL_EM_RATIO_PCT_MAX.toFixed(1) + " (" + raw + " は不正)" };
  }
  return { value, error: null };
}

/**
 * --screen-images の画像がそろっているか (OBS を起動する前に run-window.mjs が呼ぶ)。toAbs で絶対の場所にし、exists で確かめる。
 * 戻り値は { files: [絶対の場所], problems: [文] }。ないファイルが 1 つでもあれば problems に載る。
 */
export function screenImageFiles(value, toAbs, exists) {
  const files = commaItems(value).filter((item) => item.length > 0).map((item) => toAbs(item));
  const problems = files.filter((file) => !exists(file)).map((file) => "--screen-images の画像がない: " + file);
  return { files, problems };
}
// OBS の見た目と言語 (seed-config.py の --language / --theme / --font-scale / --density / --hdr へそのまま渡す。
// 値を確認するのは seed-config.py の側 = その OBS の themes と locale の保存先を見る)。
// --obs-language は --step hold だけ: 実行スクリプトは押すコンポーネントを製品と OBS の ja-JP.ini のテキストで探す (makeTextLookup) ので、
// 英語の OBS では実行計画の dialog-click が名前で見つけられない。hold は手順の中でコンポーネントを押さない (押すのはテストを実行する人が
// 手で実行するツールで、名前はそのとき渡す)。見た目の 4 つは名前に依存しないので dock-view でも受ける (HDR は
// --dock-scenario dummy に --obs-hdr を付けて使う)。
// --obs-full-height-docks on|off は OBS の [ドック] の [フルハイト ドック] (seed-config.py の --full-height-docks)。OBS 33 の既定は on で、
// 右と左の領域が主ウィンドウの上端から下端までの列になる。off は下の領域が主ウィンドウの幅いっぱいになり、右の領域のドックは
// プレビューの右でコントロールのドックの上に入る (README の画像を撮るとき。readme-shot.ps1 の「ドッキングの仕方」)。
export const OBS_LOOK_STEPS = ["hold", "dock-view"];
export const FULL_HEIGHT_DOCKS_VALUES = ["on", "off"];
export const OBS_LANGUAGE_STEPS = ["hold"];

// ---- --obs と --out の保存先の制限 ------------------------------------------
// 実行スクリプトは --obs の OBS のフォルダーへ cordmux を置いて実行の終わりに消し (removePlaced)、設定 (seed-config.py) を書く。--out の下へは
// 結果・ログ・録画 (record_dir) を書き、通知ファイルを消す。保存先を確認しないと、書き違え 1 つ (例: --obs に普段使っている OBS の
// フォルダー) で、その OBS へ DLL を置いて消す・設定を書き換えることになる。そこで実行スクリプトが始める前 (dry-run も含む。実行計画を
// 組む前) に、--obs は repo の _work の下、--out は repo の _work か build_x64 の下だけを受ける。
// 利用者のファイルに触らないこと、実行手順書の呼び出し (--obs は _work/obs-33.0.1・obs-32.2.2、--out は
// _work/out/live-a・live-b・live-c。往復の --obs は parseArgs が _work/rt-<tag>/obs を入れる)、ゲートの dry-run のスクリプト
// (--out は build_x64 の下。build_x64 を --out に受けるのはこのため) が前提である。
// 下 (直下でなく下) だけを受け、ルートそのもの (_work や build_x64) は断る。ルートの隣で名前の先頭が同じフォルダー (_work2 など) も断る。
// テキストの上で比べ、ジャンクションやシンボリックリンクは解かない (_work の下 2 段に外を指すジャンクションがないことを確認済み)。
// 外で実行する必要があるとき (VM の中で repo の外に OBS を置く場合など) は、呼び出し行に --allow-outside-work を書く (結果 JSON の args に残り、
// seed-config.py にも同じフラグを渡す)。
// parseArgs の中で確認しないのは、parseArgs が Node の API を呼ばない純関数で、パスを絶対パスにする path.resolve が要るためである。テストの
// 呼び出しは C:/obs のような架空のパスを渡す。パスの中に _work という名前があるかだけを見る方式にしないのは、どこにでも
// _work は作れ、利用者のフォルダーの下の _work も通ってしまうためである。フラグなしで外を必ず断る方式にしないのは、VM や別の保存先で実行する正当な形を
// ツールの書き換えなしに実行できなくなるためである。
export const ALLOW_OUTSIDE_WORK_FLAG = "--allow-outside-work";
export const OBS_DIR_ROOTS_REL = ["_work"];
export const OUT_DIR_ROOTS_REL = ["_work", "build_x64"];

function normalizePathText(p, caseInsensitive) {
  const s = String(p).replace(/\\/g, "/").replace(/\/+$/, "");
  return caseInsensitive ? s.toLowerCase() : s;
}

// absPath が absRoot の下 (ルートそのものは含まない) か。どちらも呼び出し側が絶対パスにした場所 (path.resolve)。区切りは両方受ける。
// "." と ".." の段を含む場所は下と見ない (path.resolve を通っていない文字列を受けない)。
export function isPathUnderRoot(absPath, absRoot, caseInsensitive) {
  if (typeof absPath !== "string" || typeof absRoot !== "string" || !absPath || !absRoot) return false;
  const p = normalizePathText(absPath, caseInsensitive);
  const r = normalizePathText(absRoot, caseInsensitive);
  if (!r || p.split("/").some((seg) => seg === "." || seg === "..")) return false;
  return p.startsWith(r + "/") && p.length > r.length + 1;
}

// 実行スクリプトの --obs と --out の保存先の問題を並べる (空なら制限を通る)。obsAbs / outAbs は絶対パスか null (null は確認しない =
// 要るかどうかは parseArgs が見る)。repoRoot は repo のルートの絶対パス。caseInsensitive は Windows で true。
export function workPathProblems({ obsAbs, outAbs, repoRoot, caseInsensitive }) {
  const problems = [];
  const join = (rel) => String(repoRoot).replace(/[\\/]+$/, "") + "/" + rel;
  const check = (flag, abs, roots) => {
    if (abs === null || abs === undefined) return;
    if (roots.some((rel) => isPathUnderRoot(abs, join(rel), caseInsensitive))) return;
    problems.push(
      flag + " は repo の " + roots.join(" か ") + " の下だけ (" + abs + " は外。外で実行するときは呼び出し行に " + ALLOW_OUTSIDE_WORK_FLAG + " を書く)",
    );
  };
  check("--obs", obsAbs, OBS_DIR_ROOTS_REL);
  check("--out", outAbs, OUT_DIR_ROOTS_REL);
  return problems;
}

// argv (process.argv.slice(2) 相当の配列) を解釈する。呼び出し側が process.argv を渡すところが
// 唯一の Node API 接点で、この関数自体は配列を受け取るだけなので Node API を呼ばない。
export function parseArgs(argv) {
  const errors = [];
  const args = {
    obs: null,
    out: null,
    plugin: null,
    step: null,
    nScreen: 4,
    nVoice: 8,
    seconds: 180,
    // エンコーダーのウォームアップは既定でオンにする
    // (起動時の欠落が 35 から 5 に減る)。--warmup 0 でオフにする。
    warmup: true,
    hw: "prefer-hardware",
    // --step integration だけが見る (A/V を測り直すための 2 つの名前付き構成)。
    integrationConfig: "default",
    // --step dock-view だけが見る (状態ごとに撮る)。
    dockScenario: DEFAULT_DOCK_SCENARIO,
    // --dock-scenario degraded だけが見る。
    dockMaxLongEdge: DEFAULT_DOCK_MAX_LONG_EDGE,
    // seed-config の段が全 --step で見る (APPDATA を差し替えた
    // 配下に偽の state.json を作る。値の意味は DOCK_INSTALL_VALUES の説明を参照)。
    dockInstall: DEFAULT_DOCK_INSTALL,
    // seed-config の段が全 --step で見る (誰を枠に入れるかを settings.json へ
    // 書く。値の意味は DOCK_ASSIGN_MODE_VALUES の説明を参照)。
    dockAssignMode: DEFAULT_DOCK_ASSIGN_MODE,
    stream: false,
    // --deadline-min が無ければ step で決める (call-1 は CALL_ONE_DEADLINE_MIN)。
    deadlineMin: DEFAULT_DEADLINE_MIN,
    deadlineMinGiven: false,
    tag: null,
    dryRun: false,
    // 先頭のチェックから vesktop.exe だけを外す (利用者の OBS と、利用者が
    // CORDMUX_PROTECTED_PROCESSES で指定したプロセスは確認したまま)。call-* だけが受け付ける。
    callWindow: false,
    // call-* では APPDATA / LOCALAPPDATA を差し替えない。
    realProfile: false,
    // seed-config.py が settings.json へ書く 3 つのキーのうちの 2 つ
    // (残る 1 つは assign_mode)。call-* は必ず立てる (ゲートが実際の state.json を書くため)。
    allowStateJsonWrite: false,
    decoder: DEFAULT_DECODER,
    // call-1 の定常状態の長さ (分)。hardware の実行は 10 で実行し直す。
    callMinutes: DEFAULT_CALL_MINUTES,
    // 実行スクリプトが用意したものの保存先を
    // call-window-artifacts.json へ書き、cleanup.ps1 -CallWindow がそれを消す。
    // 起動するツールがまだないので、現在は呼び出し行で場所を渡す (下の 3 つ)。
    browserProfiles: [],
    sharedContentDirs: [],
    tempVencordBuild: null,
    // nvidia-smi が読めない/壊れているときに GPU の使用率を確認したことにして素通りしない
    // ための安全側の既定。どうしても進めたいときだけ明示で立てる。
    allowUnknownEncoder: false,
    // --step install-roundtrip だけが見る。
    scenario: DEFAULT_ROUNDTRIP_SCENARIO,
    layout: DEFAULT_ROUNDTRIP_LAYOUT,
    // --layout programdata の明示のフラグ (VM の中で実行している)。無ければ programdata は組めない。
    vm: false,
    zip: null,
    oldZip: null,
    obsZip: null,
    testDll: null,
    // 往復の作業場所 _work/rt-<tag>/ を最後に消さずに残す (実行中の失敗を調べるとき)。
    keepWork: false,
    // call-1 の別アカウント方式: ドックの「配信者のアカウント」欄に入れる値。call-1 だけが受ける
    // 任意の引数。**文字列のまま持つ** (数として読むと桁が落ちる)。値は結果 JSON には書いてよいが、文書や
    // テストには具体値を書かない (実行時の引数でだけ渡す)。無ければ別アカウント方式の段はない。
    broadcasterId: null,
    // call-1 のリハーサル: 実機の Vesktop の代わりにテスト用の送信側を使い、human は秒数で自動で進み、
    // 待ちは REHEARSAL_TIME_FACTOR で縮める。call-1 だけ。--real-profile / --call-window とは両立しない
    // (実際の state.json に触れない = 初期設定は record と同じ差し替えた APPDATA)。
    rehearsal: false,
    // --step hold: 開いたまま待つ分、初期設定をやり直さない (更新の確認で、2 回目の起動が前の実行の設定を使うとき)、テスト用の送信側のストリームごとのサイズ。
    holdMinutes: HOLD_DEFAULT_MINUTES,
    noSeed: false,
    // hold の後始末で cordmux の録画のフォルダー (名前が recordings のフォルダーの下) を残す。
    // 判断は keepsRecordings の 1 か所 (手順の cleanup と実行スクリプトの finally の両方がそれを読む)。
    keepRecordings: false,
    // テスト用の OBS のプロファイルにリプレイバッファを書く (seed-config.py の --replay-buffer)。
    replayBuffer: false,
    // hold の --self-mic: 自分の枠の音声ソースにする音声入力キャプチャを作って始める (SELF_MIC_INPUT_NAME の説明を参照)。
    selfMic: false,
    // hold と call-hold で配信の受け口を開いたまま持つ (STREAM_SINK_STEPS の説明を参照)。
    streamSink: false,
    screenSizes: null,
    // 実際の OBS の画面を撮るための hold の旗 (screenNamesProblem などの説明を参照)。
    screenNames: null,
    screenImages: null,
    voiceUsers: null,
    labelEmRatioPct: null,
    wireComposite: false,
    // OBS の見た目と言語 (渡さなければ null = seed-config.py は何も書かない = OBS の既定)。
    obsLook: { language: null, theme: null, fontScale: null, density: null, hdr: false, fullHeightDocks: null },
    // --obs と --out を repo の作業の保存先の外でも受ける (workPathProblems の説明を参照)。
    allowOutsideWork: false,
    // 実際の通話の再確認の call-1 のフラグ (CALL_ONE_OPTION_FLAGS の説明を参照)。どれも既定はなし。
    startWithAdd: false,
    vesktopShare: false,
    skipContinuous: false,
    skipAv2: false,
    skipSpeakerMute: false,
    extraChecks: false,
    // 実行前の点検で足したフラグ (CALL_ONE_OPTION_FLAGS の説明を参照)。どれも既定はなし。
    skipExportBundle: false,
    recheck: false,
    // --recheck に、テスト動画の A/V の測定 (配信の画面と cordmux の録画) を足す (recheckMeasureSteps)。既定はなし。
    avCheck: false,
    // --recheck --av-check を、映像と音のずれ (と --latency-person の遅れ) だけを測って終わる形にする (callAvOnlyBodySteps)。既定はなし。
    avOnly: false,
    latencyPerson: null,
    // 参加の文で入ってもらうボイスチャンネルの名前 (`--voice-channel`)。渡さなければ null で、文は名前を挙げない。
    voiceChannel: null,
    // 実際の通話の実行に使うアカウントの名前のファイル (`--call-accounts`)。run-window.mjs が読んで callAccounts に入れる
    // (parseCallAccounts)。渡さなければ null で、ドライランとテストは例の名前 (DEFAULT_CALL_ACCOUNTS) を使う。
    callAccountsPath: null,
    callAccounts: null,
  };

  const list = argv || [];
  // --step hold だけ / 見た目の step だけが受けるフラグのうち、呼び出し行に実際に書かれたもの。
  const holdOnlyGiven = new Set();
  const lookGiven = new Set();
  // --step install-roundtrip だけが受け付けるフラグのうち、呼び出し行に実際に書かれたもの (既定と同じ値でも
  // 書かれていれば他の step では誤りにする)。
  const roundtripOnlyGiven = new Set();
  // call-1 だけが受けるフラグ (CALL_ONE_OPTION_FLAGS) のうち、呼び出し行に実際に書かれたもの。
  const callOneOnlyGiven = new Set();
  const callOneFlagKeys = {
    "--start-with-add": "startWithAdd",
    "--vesktop-share": "vesktopShare",
    "--skip-continuous": "skipContinuous",
    "--skip-av2": "skipAv2",
    "--skip-speaker-mute": "skipSpeakerMute",
    "--extra-checks": "extraChecks",
    "--skip-export-bundle": "skipExportBundle",
    "--recheck": "recheck",
    "--av-check": "avCheck",
    "--av-only": "avOnly",
  };
  let i = 0;
  while (i < list.length) {
    const token = list[i];
    if (typeof token !== "string" || !token.startsWith("--")) {
      errors.push("認識できない引数: " + token);
      i += 1;
      continue;
    }
    if (Object.prototype.hasOwnProperty.call(callOneFlagKeys, token)) {
      callOneOnlyGiven.add(token);
      args[callOneFlagKeys[token]] = true;
      i += 1;
      continue;
    }
    if (token === "--stream") {
      args.stream = true;
      i += 1;
      continue;
    }
    if (token === "--dry-run") {
      args.dryRun = true;
      i += 1;
      continue;
    }
    if (token === "--allow-unknown-encoder") {
      args.allowUnknownEncoder = true;
      i += 1;
      continue;
    }
    if (token === "--call-window") {
      args.callWindow = true;
      i += 1;
      continue;
    }
    if (token === "--real-profile") {
      args.realProfile = true;
      i += 1;
      continue;
    }
    if (token === "--allow-state-json-write") {
      args.allowStateJsonWrite = true;
      i += 1;
      continue;
    }
    if (token === "--vm") {
      roundtripOnlyGiven.add(token);
      args.vm = true;
      i += 1;
      continue;
    }
    if (token === "--keep-work") {
      roundtripOnlyGiven.add(token);
      args.keepWork = true;
      i += 1;
      continue;
    }
    if (token === "--rehearsal") {
      args.rehearsal = true;
      i += 1;
      continue;
    }
    if (token === "--no-seed") {
      holdOnlyGiven.add(token);
      args.noSeed = true;
      i += 1;
      continue;
    }
    if (token === "--keep-recordings") {
      holdOnlyGiven.add(token);
      args.keepRecordings = true;
      i += 1;
      continue;
    }
    if (token === "--replay-buffer") {
      holdOnlyGiven.add(token);
      args.replayBuffer = true;
      i += 1;
      continue;
    }
    if (token === "--self-mic") {
      holdOnlyGiven.add(token);
      args.selfMic = true;
      i += 1;
      continue;
    }
    if (token === "--stream-sink") {
      holdOnlyGiven.add(token);
      args.streamSink = true;
      i += 1;
      continue;
    }
    if (token === "--wire-composite") {
      holdOnlyGiven.add(token);
      args.wireComposite = true;
      i += 1;
      continue;
    }
    if (token === "--obs-hdr") {
      lookGiven.add(token);
      args.obsLook.hdr = true;
      i += 1;
      continue;
    }
    if (token === ALLOW_OUTSIDE_WORK_FLAG) {
      args.allowOutsideWork = true;
      i += 1;
      continue;
    }
    const name = token.slice(2);
    const value = list[i + 1];
    const hasValue = value !== undefined && !(typeof value === "string" && value.startsWith("--"));
    if (!hasValue) {
      errors.push("--" + name + " には値が要る");
      i += 1;
      continue;
    }
    if (["scenario", "layout", "zip", "old-zip", "obs-zip", "test-dll"].includes(name)) roundtripOnlyGiven.add("--" + name);
    switch (name) {
      case "obs":
        args.obs = value;
        break;
      case "out":
        args.out = value;
        break;
      case "plugin":
        args.plugin = value;
        break;
      case "step":
        if (!STEP_VALUES.includes(value)) errors.push("--step は " + STEP_VALUES.join("|") + " のどれか (" + value + " は不正)");
        else args.step = value;
        break;
      case "n-screen":
        args.nScreen = toIntArg(value, "--n-screen", errors);
        args.nScreenGiven = true;
        break;
      case "n-voice":
        args.nVoice = toIntArg(value, "--n-voice", errors);
        args.nVoiceGiven = true;
        break;
      case "seconds":
        args.seconds = toIntArg(value, "--seconds", errors);
        break;
      case "deadline-min":
        args.deadlineMin = toIntArg(value, "--deadline-min", errors);
        args.deadlineMinGiven = true;
        break;
      case "warmup":
        // "0" が JS の真値の文字列であることの落とし穴がある。
        // "0" と "1" だけを受け付け、それ以外は誤りとして報告する。
        if (value === "1") args.warmup = true;
        else if (value === "0") args.warmup = false;
        else errors.push("--warmup は 0 か 1 (" + value + " は不正)");
        break;
      case "hw":
        if (!HW_VALUES.includes(value)) errors.push("--hw は " + HW_VALUES.join("|") + " のどれか (" + value + " は不正)");
        else args.hw = value;
        break;
      case "integration-config":
        if (!INTEGRATION_CONFIG_VALUES.includes(value)) errors.push("--integration-config は " + INTEGRATION_CONFIG_VALUES.join("|") + " のどれか (" + value + " は不正)");
        else args.integrationConfig = value;
        break;
      case "dock-scenario":
        if (!DOCK_SCENARIO_VALUES.includes(value)) errors.push("--dock-scenario は " + DOCK_SCENARIO_VALUES.join("|") + " のどれか (" + value + " は不正)");
        else args.dockScenario = value;
        break;
      case "dock-max-long-edge":
        args.dockMaxLongEdge = toIntArg(value, "--dock-max-long-edge", errors);
        break;
      case "dock-install":
        if (!DOCK_INSTALL_VALUES.includes(value)) errors.push("--dock-install は " + DOCK_INSTALL_VALUES.join("|") + " のどれか (" + value + " は不正)");
        else args.dockInstall = value;
        break;
      case "assign-mode":
        if (!DOCK_ASSIGN_MODE_VALUES.includes(value)) errors.push("--assign-mode は " + DOCK_ASSIGN_MODE_VALUES.join("|") + " のどれか (" + value + " は不正)");
        else args.dockAssignMode = value;
        break;
      case "decoder":
        if (!DECODER_VALUES.includes(value)) errors.push("--decoder は " + DECODER_VALUES.join("|") + " のどれか (" + value + " は不正)");
        else args.decoder = value;
        break;
      case "call-minutes":
        args.callMinutes = toIntArg(value, "--call-minutes", errors);
        break;
      case "display-buffer-ms": {
        // 製品の設定 display_buffer_ms (src-obs/config/settings.h。表示のバッファ)。seed-config.py が settings.json へ書く。
        // 渡さなければ書かず、製品の既定 (100) で動く。範囲は製品の clamp と同じ。動作するのは合成のソースがある実行 (--composite) だけで、
        // 枠ごとの公開のソースの形の実行 (合成のソースがない実行) は値を見ずにフレームをすぐ渡す (OBS のバッファのまま)。
        const ms = toIntArg(value, "--display-buffer-ms", errors);
        if (ms > DISPLAY_BUFFER_MS_MAX) errors.push("--display-buffer-ms は 0 から " + DISPLAY_BUFFER_MS_MAX + " (" + value + " は不正)");
        else args.displayBufferMs = ms;
        break;
      }
      // 実行スクリプトだけが知る値。**繰り返して渡せる** (ブラウザは 3 つ、共有の
      // 中身の一時フォルダもストリーム数ぶんありうる)。一時の Vencord のビルドは 1 つなので上書き。
      case "browser-profile":
        args.browserProfiles = args.browserProfiles.concat([value]);
        break;
      case "shared-content-dir":
        args.sharedContentDirs = args.sharedContentDirs.concat([value]);
        break;
      case "temp-vencord-build":
        args.tempVencordBuild = value;
        break;
      case "broadcaster-id":
        // 文字列のまま (数として読まない)。空は「渡していない」と同じ。
        args.broadcasterId = value.trim().length > 0 ? value.trim() : null;
        break;
      case "latency-person":
        // 遅れを読む画面共有している人の見えている名前 (CALL_ONE_OPTION_FLAGS の説明を参照)。個人のアカウントの名前なのでテキストとして
        // 持たず、呼び出し行で受ける。前後の空白は落とし、空・制御文字・64 文字を超える値は断る。
        callOneOnlyGiven.add("--latency-person");
        if (!value.trim() || value.trim().length > 64 || /[\u0000-\u001f\u007f]/.test(value)) errors.push("--latency-person は 1 から 64 文字の名前 (制御文字を含まない)");
        else args.latencyPerson = value.trim();
        break;
      case "voice-channel":
        // 参加の文で入ってもらうボイスチャンネルの名前 (`CALL_ONE_OPTION_FLAGS` の説明)。`--latency-person` と同じく、前後の空白を除き、
        // 空の値・制御文字を含む値・64 文字を超える値は受け付けない。
        callOneOnlyGiven.add("--voice-channel");
        if (!value.trim() || value.trim().length > 64 || /[\u0000-\u001f\u007f]/.test(value)) errors.push("--voice-channel は 1 から 64 文字の名前 (制御文字を含まない)");
        else args.voiceChannel = value.trim();
        break;
      case "call-accounts":
        if (!value.trim()) errors.push("--call-accounts にはファイルのパスが要る");
        else args.callAccountsPath = value;
        break;
      case "tag":
        args.tag = value;
        break;
      case "scenario":
        if (!ROUNDTRIP_SCENARIO_VALUES.includes(value)) errors.push("--scenario must be one of " + ROUNDTRIP_SCENARIO_VALUES.join("|") + " (got " + value + ")");
        else args.scenario = value;
        break;
      case "layout":
        if (!ROUNDTRIP_LAYOUT_VALUES.includes(value)) errors.push("--layout must be one of " + ROUNDTRIP_LAYOUT_VALUES.join("|") + " (got " + value + ")");
        else args.layout = value;
        break;
      case "zip":
        args.zip = value;
        break;
      case "old-zip":
        args.oldZip = value;
        break;
      case "obs-zip":
        args.obsZip = value;
        break;
      case "test-dll":
        args.testDll = value;
        break;
      case "hold-minutes":
        holdOnlyGiven.add("--hold-minutes");
        args.holdMinutes = toIntArg(value, "--hold-minutes", errors);
        if (args.holdMinutes < 1 || args.holdMinutes > HOLD_MAX_MINUTES) errors.push("--hold-minutes は 1 から " + HOLD_MAX_MINUTES + " (" + value + " は不正)");
        break;
      case "screen-sizes":
        holdOnlyGiven.add("--screen-sizes");
        if (!SCREEN_SIZES_RE.test(value)) errors.push("--screen-sizes は <幅>x<高さ> をカンマで並べる (例 1280x720,3840x2160。" + value + " は不正)");
        else args.screenSizes = value;
        break;
      case "screen-names": {
        holdOnlyGiven.add("--screen-names");
        const problem = screenNamesProblem(value);
        if (problem) errors.push(problem);
        else args.screenNames = commaItems(value).join(",");
        break;
      }
      case "screen-images": {
        holdOnlyGiven.add("--screen-images");
        const problem = screenImagesProblem(value);
        if (problem) errors.push(problem);
        else args.screenImages = commaItems(value).join(",");
        break;
      }
      case "voice-users": {
        holdOnlyGiven.add("--voice-users");
        const problem = voiceUsersProblem(value);
        if (problem) errors.push(problem);
        else args.voiceUsers = commaItems(value).join(",");
        break;
      }
      case "label-em-ratio-pct": {
        holdOnlyGiven.add("--label-em-ratio-pct");
        const parsed = parseLabelEmRatioPct(value);
        if (parsed.error) errors.push(parsed.error);
        else args.labelEmRatioPct = parsed.value;
        break;
      }
      case "obs-language":
        lookGiven.add("--obs-language");
        args.obsLook.language = value;
        break;
      case "obs-theme":
        lookGiven.add("--obs-theme");
        args.obsLook.theme = value;
        break;
      case "obs-font-scale":
        lookGiven.add("--obs-font-scale");
        args.obsLook.fontScale = toIntArg(value, "--obs-font-scale", errors);
        break;
      case "obs-density":
        // 密度の id は負の数 (-2 から -5) を含むので、整数として読む (toIntArg は 0 以上だけを受ける)。
        lookGiven.add("--obs-density");
        if (!/^-?[0-9]+$/.test(value)) errors.push("--obs-density は整数 (" + value + " は不正)");
        else args.obsLook.density = Number(value);
        break;
      case "obs-full-height-docks":
        lookGiven.add("--obs-full-height-docks");
        if (!FULL_HEIGHT_DOCKS_VALUES.includes(value)) errors.push("--obs-full-height-docks は " + FULL_HEIGHT_DOCKS_VALUES.join(" か ") + " (" + value + " は不正)");
        else args.obsLook.fullHeightDocks = value === "on";
        break;
      default:
        errors.push("知らない引数: --" + name);
        break;
    }
    i += 2;
  }

  const roundtrip = args.step === INSTALL_ROUNDTRIP_STEP;
  if (roundtrip) {
    // OBS は rt-<tag> の下へ毎回展開し直し、cordmux は zip から置く。--obs と
    // --plugin は受け付けない (作業場所の外の OBS や手で作った保存先を往復に混ぜない)。
    if (args.obs) errors.push("--obs is not accepted with --step install-roundtrip (the OBS folder is " + ROUNDTRIP_WORK_PREFIX_REL + "<tag>/obs, extracted from --obs-zip)");
    if (args.plugin) errors.push("--plugin is not accepted with --step install-roundtrip (cordmux comes from --zip)");
    if (!args.tag || !ROUNDTRIP_TAG_RE.test(args.tag)) errors.push("--tag is required with --step install-roundtrip ([A-Za-z0-9._-], up to 64 characters)");
    if (!args.zip) errors.push("--zip is required with --step install-roundtrip (the zip made by test/tools/dist-zip/make_dist_zip.py)");
    if (!args.obsZip) errors.push("--obs-zip is required with --step install-roundtrip (the portable OBS zip)");
    if (ROUNDTRIP_SCENARIOS_WITH_OLD_ZIP.includes(args.scenario) && !args.oldZip) errors.push("--old-zip is required with --scenario " + args.scenario);
    if (!ROUNDTRIP_SCENARIOS_WITH_OLD_ZIP.includes(args.scenario) && args.oldZip) errors.push("--old-zip is accepted only with --scenario " + ROUNDTRIP_SCENARIOS_WITH_OLD_ZIP.join("|"));
    if (ROUNDTRIP_SCENARIOS_WITH_TEST_DLL.includes(args.scenario) && !args.testDll) errors.push("--test-dll is required with --scenario " + args.scenario);
    if (!ROUNDTRIP_SCENARIOS_WITH_TEST_DLL.includes(args.scenario) && args.testDll) errors.push("--test-dll is accepted only with --scenario " + ROUNDTRIP_SCENARIOS_WITH_TEST_DLL.join("|"));
    // programdata は VM だけで、明示のフラグが無ければ実行しない。
    if (args.layout === "programdata" && !args.vm) errors.push("--layout programdata runs only inside the VM and needs --vm");
    if (args.layout === "programdata" && !ROUNDTRIP_PROGRAMDATA_SCENARIOS.includes(args.scenario)) errors.push("--layout programdata is accepted only with --scenario " + ROUNDTRIP_PROGRAMDATA_SCENARIOS.join("|"));
    if (args.vm && args.layout !== "programdata") errors.push("--vm is accepted only with --layout programdata");
    // 後段 (結果 JSON の args、起動、ログの場所) は args.obs を読むので、展開先を入れておく。
    if (args.tag && ROUNDTRIP_TAG_RE.test(args.tag) && !args.obs) args.obs = roundtripPaths(args.tag).obsRel;
  } else {
    for (const flag of [...roundtripOnlyGiven].sort()) {
      if (args.step) errors.push(flag + " is accepted only with --step " + INSTALL_ROUNDTRIP_STEP);
    }
    if (!args.obs) errors.push("--obs が要る");
    if (!args.plugin) errors.push("--plugin が要る");
  }
  if (!args.out) errors.push("--out が要る");
  if (!args.step) errors.push("--step が要る");
  if (args.callAccountsPath !== null && args.step && args.step !== "call-1") {
    errors.push("--call-accounts は --step call-1 だけ (通話のアカウントの名前を使う段はそこにしかない)");
  }
  // --allow-state-json-write を受け付けるのは call-* だけ (seed-config.py の側でも
  // 組めない形にしてある。ここで先に弾くのは、OBS を起動する前に呼び出し行の誤りとして返すため)。
  if (args.step && !isCallStep(args.step) && args.allowStateJsonWrite) {
    errors.push("--allow-state-json-write is accepted only for " + CALL_STEP_VALUES.join("|"));
  }

  // call-* は実際の %APPDATA% / %LOCALAPPDATA% を使う (実機の Vesktop の送信部は
  // 実際の %LOCALAPPDATA%\cordmux\session.json を読むので、OBS 側だけ差し替えるとトークンが
  // 一生合わない)。**フラグを必須にして、差し替えの有無を呼び出し行に明示させる。**
  // 逆に、テスト用の送信側だけの step が実際の場所へ届かないよう、call-* 以外では受け付けない。
  // リハーサル (--rehearsal) は call-1 と call-hold だけ (REHEARSAL_STEPS)。実際の保存先も利用者の Vesktop も使わないので、
  // --real-profile / --call-window とは両立しない (差し替えた APPDATA に偽の state.json を作る = record と同じ)。
  // call-hold のリハーサルは、実際の実行の前に同じ手順をテスト用の送信側と偽の state.json で 1 度通すため。
  if (args.rehearsal && args.step && !REHEARSAL_STEPS.includes(args.step)) {
    errors.push("--rehearsal は --step " + REHEARSAL_STEPS.join(" / --step ") + " だけ (実際の Vesktop の代わりにテスト用の送信側で通すリハーサル)");
  }
  if (args.rehearsal && args.realProfile) {
    errors.push("--rehearsal と --real-profile は両立しない (リハーサルは実際の state.json に触れない)");
  }
  if (args.rehearsal && args.callWindow) {
    errors.push("--rehearsal と --call-window は両立しない (リハーサルは利用者の Vesktop を使わない)");
  }
  if (args.broadcasterId !== null && args.step && args.step !== "call-1") {
    errors.push("--broadcaster-id は --step call-1 だけ (別アカウント方式の段はそこにしかない)");
  }
  // メインアカウントの ID は Discord の数字 (1 から 20 桁)。別の文字を渡すと、記録から ID を伏せる処理が働かない。
  // 呼び出し行で先に断る (リハーサルも数字を渡す)。
  if (args.broadcasterId !== null && !/^[0-9]{1,20}$/.test(args.broadcasterId)) {
    errors.push("--broadcaster-id は数字 1 から 20 桁 (製品の欄が数字だけを受ける)");
  }
  // 実際の通話の再確認のフラグは call-1 だけ (CALL_ONE_OPTION_FLAGS の説明を参照)。スピーカーミュートを省くフラグは、
  // 省く段 (別アカウント方式) がある実行だけ意味を持つ。
  if (args.step) {
    for (const flag of [...callOneOnlyGiven].sort()) {
      if (args.step !== "call-1") errors.push(flag + " は --step call-1 だけ");
    }
  }
  if (args.skipSpeakerMute && args.step === "call-1" && args.broadcasterId === null) {
    errors.push("--skip-speaker-mute は --broadcaster-id と一緒に (省くスピーカーミュートは別アカウント方式の段にしかない)");
  }
  // --recheck (CALL_ONE_OPTION_FLAGS の説明を参照): 開始ゲートを実行しない形 (--start-with-add) が前提で、実行計画に適用されないフラグとは一緒に渡せない。
  if (args.recheck) {
    if (!args.startWithAdd) errors.push("--recheck は --start-with-add と一緒に (開始ゲートを実行しない形だけ)");
    // --recheck は別アカウント方式の手順 (メインアカウントのメッセージと自分の枠) を確かめる。並びの人数の期待 (rosterExpectedPeople) も、
    // Vesktop のアカウントがサブアカウントとして並びから外れる別アカウント方式が前提。
    if (args.broadcasterId === null) errors.push("--recheck は --broadcaster-id と一緒に (別アカウント方式の手順と、並びの人数の期待の前提)");
    for (const [flag, key] of [["--vesktop-share", "vesktopShare"], ["--extra-checks", "extraChecks"], ["--skip-continuous", "skipContinuous"], ["--skip-av2", "skipAv2"], ["--skip-speaker-mute", "skipSpeakerMute"], ["--skip-export-bundle", "skipExportBundle"]]) {
      if (args[key]) errors.push(flag + " は --recheck と一緒に渡せない (--recheck の手順表には適用されない)");
    }
  }
  // --av-check は --recheck の手順表に A/V の測定を足すフラグ。--recheck でない call-1 は、もともと av1 と av2 で測る。
  if (args.avCheck && !args.recheck) errors.push("--av-check は --recheck と一緒に (--recheck でない call-1 は、もともと av1 と av2 で A/V を測る)");
  // --av-only は --recheck --av-check の手順表を、測るところと締めだけにする (callAvOnlyBodySteps)。
  if (args.avOnly && !(args.recheck && args.avCheck)) errors.push("--av-only は --recheck --av-check と一緒に (その手順表を、映像と音のずれを測るところと締めだけにする)");
  // Vesktop のウィンドウは Main の 2x2 の空いた象限に置くので、画面の枠は 2 つか 3 つ (1 つなら枠が全面、4 つなら空きがない)。
  if (args.latencyPerson !== null && args.step === "call-1" && latencyFreeCell(callScreens(args)) === null) {
    errors.push("--latency-person は --n-screen 2 か 3 と一緒に (Vesktop のウィンドウを Main の 2x2 の空いた象限に置くため)");
  }
  if (args.step && isCallStep(args.step) && !args.realProfile && !args.rehearsal) {
    errors.push("--real-profile が要る (" + CALL_STEP_VALUES.join("|") + " は実際の %APPDATA% / %LOCALAPPDATA% を使う)");
  }
  if (args.step && !isCallStep(args.step) && args.realProfile) {
    errors.push("--real-profile は " + CALL_STEP_VALUES.join("|") + " だけ (テスト用の送信側だけの実行は実際の保存先を読まない)");
  }
  if (args.step && !isCallStep(args.step) && args.callWindow) {
    errors.push("--call-window は " + CALL_STEP_VALUES.join("|") + " だけ (テスト用の送信側だけの実行は vesktop.exe が動いていないことを確認したまま)");
  }
  // 実行スクリプトだけが知る値は 4 アカウント通話の実行のものである (テスト用の送信側だけの実行は
  // ブラウザも一時の Vencord のビルドも使わない)。call-1 だけが書く (ゲートの 4 つは
  // ブラウザより前の段で、消す対象がまだない)。
  if (args.step && args.step !== "call-1" && callWindowArtifactsFromArgs(args) !== null) {
    errors.push("--browser-profile / --shared-content-dir / --temp-vencord-build は --step call-1 だけ (実行スクリプトが用意したものを消すのは後始末の手順)");
  }
  // --step hold だけのフラグ。--no-seed は「前の実行の設定とシーンコレクションをそのまま使う」なので、ほかの step では
  // 初期設定が前提の判定 (シーンの名前・枠のソースの数) と食い違う。
  // --hold-minutes は call-hold も受ける (開いたまま待つ長さ)。--no-seed と --screen-sizes は hold だけ (call-hold は
  // call-gate-a と同じ初期設定が前提で、テスト用の送信側はリハーサルだけ)。--keep-recordings も hold だけ (call-hold は一覧で常に残す。
  // --no-seed とは両立する = 起動し直しの 2 回目は前の実行の設定のままで録画も残したい)。
  if (args.step) {
    for (const flag of [...holdOnlyGiven].sort()) {
      const steps = flag === "--hold-minutes" || flag === "--stream-sink" ? ["hold", "call-hold"] : ["hold"];
      if (!steps.includes(args.step)) errors.push(flag + " は --step " + steps.join(" / --step ") + " だけ");
    }
  }
  // --replay-buffer は初期設定 (seed-config.py) が書くので、初期設定をしない実行では動作しない (見た目のフラグと同じ扱い)。
  if (args.replayBuffer && args.noSeed) errors.push("--replay-buffer は --no-seed と両立しない (初期設定をしない実行は OBS の設定を書かない)");
  // --self-mic は自分の枠の音声ソースの名前を初期設定 (seed-config.py) が settings.json に書くので、初期設定をしない実行では動作しない。
  if (args.selfMic && args.noSeed) errors.push("--self-mic は --no-seed と両立しない (初期設定をしない実行は cordmux の settings.json を書かない)");
  // --label-em-ratio-pct も初期設定が settings.json に書く。
  if (args.labelEmRatioPct !== null && args.noSeed) errors.push("--label-em-ratio-pct は --no-seed と両立しない (初期設定をしない実行は cordmux の settings.json を書かない)");
  if (args.step === "hold") {
    if (args.holdMinutes + HOLD_CLOSE_MARGIN_MIN > args.deadlineMin) {
      errors.push("--hold-minutes " + args.holdMinutes + " は実行の期限 (--deadline-min " + args.deadlineMin + ") から閉じる分 " + HOLD_CLOSE_MARGIN_MIN + " 分を引いた長さまで");
    }
    // テスト用の送信側は --n-screen / --n-voice を渡したときだけ接続する (既定の 4 / 8 は他の step の値で、hold では接続しない)。
    if (args.screenSizes && !args.nScreenGiven) errors.push("--screen-sizes は --n-screen と一緒に (テスト用の送信側の画面ストリームごとのサイズ)");
    if (args.screenSizes && args.nScreenGiven && args.screenSizes.split(",").length > args.nScreen) {
      errors.push("--screen-sizes の数 (" + args.screenSizes.split(",").length + ") が --n-screen (" + args.nScreen + ") より多い");
    }
    // 表示名と画像は画面ごと、声の持ち主は声ごと。どれも送信側を繋ぐときだけ意味を持つ。
    for (const [flag, key] of [["--screen-names", "screenNames"], ["--screen-images", "screenImages"]]) {
      if (!args[key]) continue;
      if (!args.nScreenGiven) errors.push(flag + " は --n-screen と一緒に (テスト用の送信側の画面ごとの値)");
      else if (args[key].split(",").length > args.nScreen) errors.push(flag + " の数 (" + args[key].split(",").length + ") が --n-screen (" + args.nScreen + ") より多い");
    }
    if (args.voiceUsers) {
      if (!args.nVoiceGiven) errors.push("--voice-users は --n-voice と一緒に (テスト用の送信側の声ごとの持ち主)");
      else if (args.voiceUsers.split(",").length > args.nVoice) errors.push("--voice-users の数 (" + args.voiceUsers.split(",").length + ") が --n-voice (" + args.nVoice + ") より多い");
      const beyond = voiceUsersBeyondScreens(args.voiceUsers, args.nScreenGiven ? args.nScreen : 0);
      if (beyond.length > 0) errors.push("--voice-users の " + beyond.join(", ") + " は送信側の画面にいない (--n-screen " + (args.nScreenGiven ? args.nScreen : 0) + ")");
    }
  }
  // call-hold の実際の実行は、送信側が利用者の Vesktop だけ (テスト用の送信側を横で接続しない。利用者の Vesktop の
  // プロセスは止めない・操作しないことと、枠に誰が載ったかの読みを混ぜないため)。テスト用の送信側のストリーム数はリハーサルだけ受ける。
  if (args.step === "call-hold" && !args.rehearsal && (args.nScreenGiven || args.nVoiceGiven)) {
    errors.push("--n-screen / --n-voice は --step call-hold では --rehearsal のときだけ (実際の実行の送信側は利用者の Vesktop)");
  }
  // OBS の見た目のフラグ: 見た目は hold と dock-view、言語は hold だけ (OBS_LOOK_STEPS の説明を参照)。--no-seed では書く機会がない。
  if (args.step) {
    for (const flag of [...lookGiven].sort()) {
      const steps = flag === "--obs-language" ? OBS_LANGUAGE_STEPS : OBS_LOOK_STEPS;
      if (!steps.includes(args.step)) errors.push(flag + " は --step " + steps.join(" / --step ") + " だけ");
      else if (args.noSeed) errors.push(flag + " は --no-seed と両立しない (初期設定をしない実行は OBS の設定を書かない)");
    }
  }
  // 実際の call-1 の期限は実行計画の見込み (2 時間超) より長く取る。リハーサルは待ちを縮めるので既定のまま。
  if (!args.deadlineMinGiven && args.step === "call-1" && !args.rehearsal) args.deadlineMin = CALL_ONE_DEADLINE_MIN;

  return { ok: errors.length === 0, args, errors };
}
// ---- 手順の処理が使う小さな判定 ------------------------------------
// run-window.mjs の中に書くとテストで実行できないので、純関数にして run-window-plan.test.ts で確認する。

// obsws-connect の直後の GetVersion が OBS の NotReady (obs-websocket の要求の状態 207) で断られたか。obsws.mjs の
// 失敗のメッセージは "<要求>: <状態の数> <説明>" (例 "GetVersion: 207 ...")。数の一部 (1207・2070) には一致させない。
export function isObsNotReadyError(message) {
  return /\b207\b/.test(String(message ?? ""));
}

// dock-click の手順が cordmux の録画の印 (recordOnMs / recordOffMs) を打つか。"on" / "off" / null。
// desired を持つ dock-click でも、行のチェック (rowCheckStep) は録画のボタンではないので印を打たない。
// 印を読むのは --step record の OBS 側の黒の判定 (run-window.mjs の judge-record) で、そこが要るのは録画のボタンを
// 押した時刻だけ。手順に印の旗を足すと、押したコンポーネントが録画のボタンかという同じ事実を 2 か所に持つことになるため、
// ここでコンポーネントの id から判定する。
export function recordMarkOf(s) {
  if (!s || s.kind !== "dock-click") return null;
  if (s.textId === TEXT_ID_RECORD_START && s.desired === "on") return "on";
  if (s.textId === TEXT_ID_RECORD_STOP && s.desired === "off") return "off";
  return null;
}

// ---- 開始前の確認 ------------------------------------------------------

// list: [{ name: string, path: string|null }] (実際の取得方法は run-window.mjs が決める。
// 名前は大文字小文字を区別しない)。obs64.exe だけは path で利用者の Program Files 配下かを見る
// (それ以外はいつ動いていても止める)。path が分からない obs64.exe は安全側に倒して止める。
//
// opts.callWindow (4 アカウント通話の実行): **vesktop.exe だけを確認の対象から
// 外す** (利用者が Vesktop を使うのが前提の実行のため)。利用者の OBS と、opts.protectedNames の
// プロセスは確認したままにする。opts を渡さない呼び出しは vesktop.exe も止める。
//
// opts.protectedNames: 利用者が CORDMUX_PROTECTED_PROCESSES のファイルで指定したプロセス名 (小文字にそろえなくてよい)。
// 動いていれば場所に関わらず止める。
export function blockersFromProcessList(list, opts) {
  const callWindow = Boolean(opts && opts.callWindow);
  const protectedKeys = new Set(((opts && opts.protectedNames) || []).map((n) => String(n).toLowerCase()));
  const blockers = [];
  for (const proc of list || []) {
    const rawName = (proc && proc.name) || "";
    const key = rawName.toLowerCase();
    if (key === "obs64.exe") {
      const p = (proc.path || "").toLowerCase();
      const pathUnknown = !proc.path;
      const underProgramFiles = p.indexOf("program files") !== -1;
      if (pathUnknown || underProgramFiles) {
        blockers.push({
          process: "obs64.exe",
          path: proc.path || null,
          reason: pathUnknown ? "obs64.exe の場所が分からない (利用者の OBS の可能性があるため止める)" : "利用者の OBS (Program Files) が動いている",
        });
      }
      continue;
    }
    if (key === "vesktop.exe") {
      if (callWindow) continue; // 4 アカウント通話の実行は Vesktop を使うのが前提
      blockers.push({ process: "vesktop.exe", path: proc.path || null, reason: "Vesktop が動いている" });
      continue;
    }
    if (protectedKeys.has(key)) {
      blockers.push({ process: rawName, path: proc.path || null, reason: "利用者が指定したプロセスが動いている" });
    }
  }
  return blockers;
}

// 利用者が手元のファイルで守るプロセスを足すための環境変数。値はテキストファイルのパス。
export const PROTECTED_PROCESSES_ENV = "CORDMUX_PROTECTED_PROCESSES";
// 何も指定しなくても守るプロセス (名前で見るもの)。obs64.exe は場所も見るので blockersFromProcessList が別に扱う。
export const DEFAULT_GUARDED_PROCESS_NAMES = ["obs64.exe", "vesktop.exe"];

// 守るプロセスのファイルの中身を、プロセス名の一覧にする。1 行に 1 つ。前後の空白は落とし、
// 空行と # で始まる行は読まない。大文字と小文字は区別しないので、同じ名前は 1 つにまとめる (先に出た綴りを残す)。
export function parseProtectedProcessNames(text) {
  const names = [];
  const seen = new Set();
  const body = String(text || "").replace(/^\uFEFF/, "");
  for (const raw of body.split(/\r?\n/)) {
    const line = raw.trim();
    if (line.length === 0 || line.startsWith("#")) continue;
    const key = line.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    names.push(line);
  }
  return names;
}

// 環境変数の値 (ファイルのパス) から、守るプロセスの名前と、起動を止める理由を作る。
// readFile(path) は文字列を返すか、読めなければ例外を投げる関数 (実行スクリプトが fs で渡す)。
// 指定がなければ何も足さない。指定があるのに読めないときは、守るはずのものを守らずに始めないため、blockers を返して止める。
export function loadProtectedProcesses(envValue, readFile) {
  const filePath = String(envValue || "").trim();
  if (filePath.length === 0) return { names: [], blockers: [] };
  try {
    return { names: parseProtectedProcessNames(readFile(filePath)), blockers: [] };
  } catch (e) {
    const message = e && e.message ? String(e.message) : String(e);
    return {
      names: [],
      blockers: [
        {
          process: PROTECTED_PROCESSES_ENV,
          path: filePath,
          reason: "守るプロセスの一覧のファイルが読めないので始めない (" + message + ")",
        },
      ],
    };
  }
}

// text: nvidia-smi --query-gpu=utilization.encoder --format=csv,noheader (複数 GPU なら複数行、
// 単位付き "0 %" でもなし "0" でも受け付ける。"[N/A]" 等の数値でない行は busy に数えない)。
export function encoderBusy(text) {
  const lines = String(text || "")
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l.length > 0);
  const values = [];
  for (const line of lines) {
    const cell = line.split(",")[0].trim();
    const m = cell.match(/-?\d+(?:\.\d+)?/);
    if (!m) continue;
    values.push(Number(m[0]));
  }
  return { busy: values.some((v) => v > 0), values };
}

// 通話の実行 (--call-window) で GPU のエンコーダを使ってよいアプリ。通話に参加したアプリの画面共有がエンコーダを
// 使う (Desktop 版の Discord の Go Live は NVENC で 1920x1080 をエンコードし、使用率が 7% になる)。
// 使用率 0 を求めるのは、利用者の OBS などの GPU の処理を守るためなので、通話の実行ではエンコードの
// 使い手が全部これらのアプリなら通す (1 つでも名前が分からない・ほかのアプリなら止める)。名前は小文字で比べる。
export const CALL_WINDOW_ENCODER_APPS = ["discord.exe", "vesktop.exe", "chrome.exe", "firefox.exe"];

// text: `nvidia-smi encodersessions` の出力 (見出し「# GPU Session Process ...」の後に 1 行 1 セッション。
// 一定の間隔で同じ一覧を繰り返す)。最初の一覧の Process Id (3 列目) を重ねずに返す。
export function parseEncoderSessionPids(text) {
  const pids = [];
  let rowsSeen = false;
  for (const raw of String(text || "").split(/\r?\n/)) {
    const line = raw.trim();
    if (line === "") continue;
    if (line.startsWith("#")) {
      if (rowsSeen) break; // 次の一覧の見出し。最初の一覧だけを読む。
      continue;
    }
    const cols = line.split(/\s+/);
    const pid = Number(cols[2]);
    if (cols.length < 3 || !Number.isInteger(pid) || pid <= 0) continue;
    rowsSeen = true;
    if (!pids.includes(pid)) pids.push(pid);
  }
  return pids;
}

// sessions: [{ pid, name }] (name は tasklist の名前。引けなければ null)。1 つ以上あり、全部が
// CALL_WINDOW_ENCODER_APPS のどれかなら true。
export function encoderUsedOnlyByCallApps(sessions) {
  if (!Array.isArray(sessions) || sessions.length === 0) return false;
  return sessions.every((s) => typeof s.name === "string" && CALL_WINDOW_ENCODER_APPS.includes(s.name.toLowerCase()));
}

// GPU のエンコーダの使用率を確認できたか (nvidia-smi が失敗した・出力が数でない・空、の
// どれでもない) を含めて、実行を始めてよいかを決める (使用率 0 を確認する)。
// encoderError が立っている、または values が空 (= 1 行も数値を読めなかった) ときは
// 「確認できなかった」ので安全側に倒して止める。allowUnknownEncoder は明示の抜け道
// (既定 false)。encoderOnlyCallApps は通話の実行で、エンコードの使い手が全部通話のアプリだと確認したときだけ
// 呼び出し元が立てる (CALL_WINDOW_ENCODER_APPS。立っていれば使用率が 0 でなくても通す)。
export function guardBlocked({ blockers, encoderBusy: busy, encoderValues, encoderError, allowUnknownEncoder, encoderOnlyCallApps }) {
  const hasBlockers = Array.isArray(blockers) && blockers.length > 0;
  const encoderUnknown = !!encoderError || !Array.isArray(encoderValues) || encoderValues.length === 0;
  if (hasBlockers) return true;
  if (busy && encoderOnlyCallApps !== true) return true;
  if (encoderUnknown && !allowUnknownEncoder) return true;
  return false;
}

// OBS のログから「モジュール一覧に moduleFileName があるか」を見る。obs-module.c の
// obs_log_loaded_modules() (blog(LOG_INFO, "  Loaded Modules:") の後に
// blog(LOG_INFO, "    %s", mod->file) を1行ずつ) が書く節の**中だけ**を見る。節の終わりは
// 次の区切り線 (OBSBasic.cpp が前後に blog(LOG_INFO, "---------------------------------") を
// 打つ) とする。読み込みに失敗したモジュールはこの節に現れない (obs_open_module が
// 先に return するため) ので、節の外の "Failed to load module file" 等の行を誤って
// hit=true と数えない。節自体が無ければ (OBS がそこまで進まず落ちた等) hit は false。
export function judgeModuleLoaded(lines, moduleFileName) {
  const list = lines || [];
  const needle = String(moduleFileName || "").toLowerCase();
  const startIdx = list.findIndex((l) => /Loaded Modules:/i.test(l));
  if (startIdx === -1) return { hit: false, blockLines: [] };
  let endIdx = list.length;
  for (let i = startIdx + 1; i < list.length; i += 1) {
    if (/-{10,}/.test(list[i])) {
      endIdx = i;
      break;
    }
  }
  const block = list.slice(startIdx, endIdx);
  const hit = needle.length > 0 && block.some((l) => l.toLowerCase().includes(needle));
  return { hit, blockLines: block };
}

// harness (テスト用の送信側) が書いた結果 JSON のうち、この harness 呼び出しで新しく増えた
// ものだけを選ぶ (before/after はディレクトリの列挙結果。ファイル名の配列)。
export function pickNewResultFiles(before, after, prefix) {
  const beforeSet = new Set(before || []);
  return (after || []).filter((f) => !beforeSet.has(f) && f.startsWith(prefix));
}

// ---- 背圧 (guard のテスト用の送信側の overflow) の判定 ---------------------------

// pipeline.cpp の stats_loop が出す「[cordmux] stats video ... dropped=N no_route_dropped=M」
// (プラグインのログから読む受信側の待ち行列の捨て数)。同じ形式の行が複数あれば最後 (最新の
// 累積値) を返す。1 行も無ければ null (40 秒のタイムアウトで一度も出なかった
// 場合、統計そのものがないことを null で区別する)。
export function parseStatsVideoDropped(lines) {
  let last = null;
  for (const line of lines || []) {
    // 先頭は大文字の "Stats"。小文字の "stats" の行も読む。
    const m = /\[cordmux\] [Ss]tats video .*\bdropped=(\d+)\b/.exec(line);
    if (m) last = Number(m[1]);
  }
  return last;
}

// productResultData: harness (test/tools/harness/sender.ts) の product モードの結果 JSON の
// data 欄 (HarnessRunResult。establishedReached と socketStats.droppedByBackpressure を持つ)。
// pluginLogLines: OBS のプラグインのログ (行の配列)。
// テスト用の送信側の overflow が接続 (established)
// に至ったかを確認し、至らなければ NG にする (接続できないと 40 秒の
// タイムアウトになり測れない)。確立しても、送信側 (droppedByBackpressure) にも受信側
// (stats video の dropped) にも捨てた数が 1 つも無ければ、背圧そのものが一度も働いていない
// (しきい値 1 MiB・受信側の 1 メッセージ上限 64 MiB の暫定値を見直す実測に
// なっていない) ので、これも NG にする (backpressureExercised)。
export function judgeBackpressure(productResultData, pluginLogLines) {
  const hasResult = !!productResultData;
  const establishedReached = hasResult && !!productResultData.establishedReached;
  const droppedByBackpressure =
    hasResult && productResultData.socketStats && typeof productResultData.socketStats.droppedByBackpressure === "number"
      ? productResultData.socketStats.droppedByBackpressure
      : null;
  const statsVideoDropped = parseStatsVideoDropped(pluginLogLines);
  const backpressureExercised = (droppedByBackpressure !== null && droppedByBackpressure > 0) || (statsVideoDropped !== null && statsVideoDropped > 0);
  let reason = null;
  if (!hasResult) {
    reason = "テスト用の送信側の結果 JSON が読めなかった (harness が結果を書けなかった/壊れていた可能性。establishedReached 自体を確認できない)";
  } else if (!establishedReached) {
    reason = "テスト用の送信側の overflow が接続 (established) に至らなかった (接続の確立に失敗している疑い)";
  } else if (!backpressureExercised) {
    reason = "接続はできたが、送信側にも受信側にも捨てた数がない (背圧が一度も働いていない疑い。しきい値の暫定値の見直しに使える実測になっていない)";
  }
  return {
    ok: establishedReached && backpressureExercised,
    reason,
    establishedReached,
    droppedByBackpressure,
    statsVideoDropped,
    backpressureExercised,
  };
}

// ---- 送信側の直近の configure 寸法と facts.stages の抜き出し (--dock-scenario degraded) -----

// productResultData: harness (test/tools/harness/sender.ts) の product モードの結果 JSON の
// data 欄 (HarnessRunResult)。参加者タブの段の文言・解像度の欄・結果 JSON の一致を確認するため、
// 5 段の入れ子 (steps ->
// harness-wait -> productResult -> data -> streams/factsSent) を辿らなくても読める形に
// コピーする (run-window.mjs が record("encode-facts", ...) で結果 JSON の 1 段目に足す)。
// configure の寸法は streams[].encodedFormat (段がまだ決まっていない間と Unavailable のときは
// width/height/fps ともない)、段は factsSent.stages
// (encode.stage ではなく、ドックが実際に読む facts の値をそのままコピーする)。requested は
// この harness 呼び出しが要求した値 (config.maxLongEdge / config.failStage)。「要求した値」と
// 「実際に出た値」を同じ行に並べることで、人が結果 JSON だけで「動作したか」を判断できる。
export function encodeFactsFromHarnessData(productResultData) {
  const data = productResultData || null;
  const streams = (data && data.streams) || [];
  const configure = streams
    .filter((s) => s && s.kind === "screen")
    .map((s) => ({
      streamId: s.streamId,
      width: (s.encodedFormat && s.encodedFormat.width) ?? null,
      height: (s.encodedFormat && s.encodedFormat.height) ?? null,
      fps: (s.encodedFormat && s.encodedFormat.fps) ?? null,
    }));
  const stages = (data && data.factsSent && data.factsSent.stages) || [];
  return {
    hasData: !!data,
    requested: {
      maxLongEdge: typeof data?.config?.maxLongEdge === "number" ? data.config.maxLongEdge : null,
      failStage: typeof data?.config?.failStage === "number" ? data.config.failStage : null,
    },
    configure,
    stages,
  };
}

// ---- シーンコレクション切り替え (--step second-obs) の間、接続が繋がったままだったかの判定 -------

// src-obs/ws/listener.cpp の handle_hello_phase / on_close が出す固定文言 (plugin-main.cpp の
// cordmux_log_sink が "[cordmux] " を前置する)。close は hello に成功した後の切断だけを出す
// (listener.cpp の on_close を参照)。
// ログの文言は英語と日本語の両方がありうるので、両方を数える。
// 英語の文言は**行の終わりまで一致**で数える: close の "Disconnected from Vesktop" はハートビートの切れの行
// "Disconnected from Vesktop: it stopped responding" の先頭と同じで、含むかで数えるとハートビートの切れも close に数える
// (日本語の文言の close は on_close の行だけなので、数える対象は変わらない)。
const HELLO_MARKS = ["[cordmux] Connected to Vesktop", "[cordmux] hello: 合言葉が一致した。接続を確立した"];
const CLOSE_MARKS = ["[cordmux] Disconnected from Vesktop", "[cordmux] close: 接続が切れた"];
function endsWithAny(line, marks) {
  const s = String(line).trimEnd();
  return marks.some((m) => s.endsWith(m));
}
export function countConnectionEvents(lines) {
  const list = lines || [];
  return {
    helloEstablishedCount: list.filter((l) => endsWithAny(l, HELLO_MARKS)).length,
    closeCount: list.filter((l) => endsWithAny(l, CLOSE_MARKS)).length,
  };
}

// beforeLines: 切り替えを始める直前 (wait-connection が hello を見つけた瞬間) の OBS のログ。
// afterLines: 戻した後の録画まで終えた時点の同じログ (ログは追記のみなので beforeLines の
// 続きになっている)。switchApplied: SetCurrentSceneCollection が両方成功し、かつ切り替えの
// 最中に読んだ GetSceneCollectionList が実際に test-empty を返したか (run-window.mjs が
// obsws-request の結果から組み立てる。obsws-request は例外を握って ok:false を返すだけで
// 次の手順へ進むので、「呼んだこと」と「切り替わったこと」は別に確認する必要がある)。
// before の時点では hello は必ず 1 回・close は必ず 0 回 (wait-connection は最初の hello を
// 見つけた瞬間の記録を渡すので、それより前に close は起こりえない)。held (ok:true) は、
// 切り替えの前後で hello と close の回数が増えていない (繋ぎ直しも切断も起きていない) こと。
export function judgeConnectionHeld({ beforeLines, afterLines, switchApplied }) {
  if (!beforeLines) {
    return {
      ok: false,
      reason: "切り替え前に接続の確立を確認できないまま進んだ (wait-connection が hello を見つけられなかった)",
      helloEstablishedCount: null,
      closeCount: null,
      switchApplied: !!switchApplied,
    };
  }
  const before = countConnectionEvents(beforeLines);
  const after = countConnectionEvents(afterLines);
  if (!switchApplied) {
    return {
      ok: false,
      reason: "シーンコレクションの切り替え自体が確認できなかった (SetCurrentSceneCollection の失敗、または切り替え中の GetSceneCollectionList が test-empty を返さなかった)",
      helloEstablishedCount: after.helloEstablishedCount,
      closeCount: after.closeCount,
      switchApplied: false,
    };
  }
  if (after.helloEstablishedCount > before.helloEstablishedCount) {
    return {
      ok: false,
      reason: "切り替えの間に hello が新たに記録された (繋ぎ直された疑いがある)",
      helloEstablishedCount: after.helloEstablishedCount,
      closeCount: after.closeCount,
      switchApplied: true,
    };
  }
  if (after.closeCount > before.closeCount) {
    return {
      ok: false,
      reason: "切り替えの間に close が新たに記録された (接続が切れた疑いがある)",
      helloEstablishedCount: after.helloEstablishedCount,
      closeCount: after.closeCount,
      switchApplied: true,
    };
  }
  return { ok: true, reason: null, helloEstablishedCount: after.helloEstablishedCount, closeCount: after.closeCount, switchApplied: true };
}

// ---- OBS 自身の欠け (--step call-1 の obs-stats) -----------------

// obs-websocket の GetStats が返す 2 回分 (record-start の 15 秒後と record-stop の直前) から、
// 配信用エンコードの欠け (outputSkippedFrames) が同じ区間の出力フレーム数に占める割合を出す
// (起動時の立ち上がりを含めないために 2 点の差分で数える)。
//
// **割合が閾値 (既定 0.02%) を超えたら失敗**。超えたかどうかは、上限を既定値にするか
// 「上限なし」のままにするかを決める材料になる。
//
// 測れなかった場合 (どちらかのサンプルがない・数でない・区間に 1 フレームも出ていない・カウントが
// 戻っている = OBS の出力が途中で作り直された) は **ok:false** にする (割合 0 として成功にしない。
// judgeBackpressure が「捨てた数が 1 つもない」を合格にしないのと同じ方針)。
export function judgeOutputSkipped(first, last, threshold) {
  const limit = typeof threshold === "number" ? threshold : OUTPUT_SKIPPED_RATIO_THRESHOLD;
  const num = (sample, key) => (sample && typeof sample[key] === "number" && Number.isFinite(sample[key]) ? sample[key] : null);
  // GetStats の 4 つの値のうち、割合の判定に要る 3 つだけを必須にする
  // (cpuUsage はログのためだけの値、renderTotalFrames は判定に使わないので任意)。
  const fields = ["outputSkippedFrames", "outputTotalFrames", "renderSkippedFrames"];
  const fail = (reason) => ({ ok: false, reason, threshold: limit, ratio: null, outputSkippedDelta: null, outputTotalDelta: null, renderSkippedDelta: null, cpuUsage: null });

  if (!first || !last) return fail("GetStats のサンプルが 2 つ揃っていない (obs-stats が 2 回取れていない)");
  for (const key of fields) {
    if (num(first, key) === null || num(last, key) === null) return fail(key + " が数として読めない (GetStats の戻り値が想定と違う)");
  }

  const outputSkippedDelta = last.outputSkippedFrames - first.outputSkippedFrames;
  const outputTotalDelta = last.outputTotalFrames - first.outputTotalFrames;
  const renderSkippedDelta = last.renderSkippedFrames - first.renderSkippedFrames;
  const renderTotalDelta =
    num(first, "renderTotalFrames") !== null && num(last, "renderTotalFrames") !== null ? last.renderTotalFrames - first.renderTotalFrames : null;
  const cpuUsage = { first: num(first, "cpuUsage"), last: num(last, "cpuUsage") };

  if (outputSkippedDelta < 0 || outputTotalDelta < 0 || renderSkippedDelta < 0 || (renderTotalDelta !== null && renderTotalDelta < 0)) {
    return { ...fail("カウントが 2 回目で減っている (配信/録画の出力が区間の途中で作り直された疑い)"), outputSkippedDelta, outputTotalDelta, renderSkippedDelta, cpuUsage };
  }
  if (outputTotalDelta === 0) {
    return { ...fail("区間に出力のフレームが 1 つもない (配信が始まっていない疑い)"), outputSkippedDelta, outputTotalDelta, renderSkippedDelta, cpuUsage };
  }

  const ratio = outputSkippedDelta / outputTotalDelta;
  return {
    ok: ratio <= limit,
    reason: ratio <= limit ? null : "配信用符号化の欠けが閾値を超えた (" + (ratio * 100).toFixed(4) + "% > " + (limit * 100).toFixed(4) + "%)",
    threshold: limit,
    ratio,
    outputSkippedDelta,
    outputTotalDelta,
    renderSkippedDelta,
    renderTotalDelta,
    cpuUsage,
  };
}

// ---- 開始ゲートの判定 -------------------------------------

// Windows のパスの比較: 区切りと大文字小文字と末尾の区切りだけをそろえる (それ以外はそろえない。
// 短縮名やジャンクションまで解決するには実ファイルを読む必要があり、このファイルは Node の API を
// 呼ばないため)。
function normalizePathForCompare(value) {
  return String(value == null ? "" : value)
    .replace(/\\/g, "/")
    .replace(/\/+$/, "")
    .toLowerCase();
}

function parseJsonOrNull(text) {
  try {
    return { ok: true, value: JSON.parse(String(text)) };
  } catch (e) {
    return { ok: false, error: e.message };
  }
}

function sameJsonValue(a, b) {
  if (a === b) return true;
  if (typeof a !== typeof b) return false;
  if (a === null || b === null) return a === b;
  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
    return a.every((item, i) => sameJsonValue(item, b[i]));
  }
  if (typeof a === "object") {
    const ak = Object.keys(a).sort();
    const bk = Object.keys(b).sort();
    if (ak.length !== bk.length || ak.some((k, i) => k !== bk[i])) return false;
    return ak.every((k) => sameJsonValue(a[k], b[k]));
  }
  return false;
}

// beforeText: 書き換える前の state.json の全文 (call-gate-a が結果フォルダへ取ったコピー)。
// afterText: 現在の state.json の全文。expectedDir: テスト用にインストールしたファイルの data/vencord の絶対パス。
//
// compareOtherKeys (既定 true) は「vencordDir 以外のキーが 1 つも変わっていないか」を見るかどうか。
//   - call-gate-a (押した直後) は true: 製品は vencordDir だけを差し替えて書き戻す
//     (src-obs/install/vencord_dir.h「他のキーは 1 文字も変えない」) ので、他が変われば失敗。
//   - call-gate-b (利用者が Vesktop を閉じた後) は false: **Vesktop 自身が終了時にウィンドウの大きさなどを
//     書き直す**ので、他のキーの変化は正常である。ここで見たいのは「終了時に vencordDir が
//     上書きされていないか」だけ。
//
// 比較は文字列ではなく解析した値で行う (製品は並びと整形を保つが、整形の細部を失敗の理由に
// しない = 見たいのは値であるため)。
export function judgeVencordDirWritten({ beforeText, afterText, expectedDir, compareOtherKeys }) {
  const compareOthers = compareOtherKeys !== false;
  const after = parseJsonOrNull(afterText);
  if (!after.ok) {
    return { ok: false, reason: "現在の state.json が JSON として読めない (" + after.error + ")", vencordDir: null, otherKeysSame: null, changedKeys: [] };
  }
  if (after.value === null || typeof after.value !== "object" || Array.isArray(after.value)) {
    return { ok: false, reason: "現在の state.json がオブジェクトでない", vencordDir: null, otherKeysSame: null, changedKeys: [] };
  }
  const vencordDir = Object.prototype.hasOwnProperty.call(after.value, "vencordDir") ? after.value.vencordDir : null;
  if (typeof vencordDir !== "string") {
    return { ok: false, reason: "vencordDir のキーがない、または文字列でない", vencordDir, otherKeysSame: null, changedKeys: [] };
  }
  const pointsToOurs = normalizePathForCompare(vencordDir) === normalizePathForCompare(expectedDir);
  if (!pointsToOurs) {
    return {
      ok: false,
      reason: "vencordDir がテスト用にインストールしたファイルの data/vencord を指していない (現在 " + vencordDir + "、期待 " + String(expectedDir) + ")",
      vencordDir,
      otherKeysSame: null,
      changedKeys: [],
    };
  }

  if (!compareOthers) {
    return { ok: true, reason: null, vencordDir, otherKeysSame: null, changedKeys: [] };
  }

  const before = parseJsonOrNull(beforeText);
  if (!before.ok || before.value === null || typeof before.value !== "object" || Array.isArray(before.value)) {
    return { ok: false, reason: "書き換える前の state.json のコピーが読めない (" + STATE_JSON_BEFORE_NAME + " がない、または壊れている)", vencordDir, otherKeysSame: null, changedKeys: [] };
  }
  const keys = new Set([...Object.keys(before.value), ...Object.keys(after.value)]);
  keys.delete("vencordDir");
  const changedKeys = [...keys].filter((k) => !sameJsonValue(before.value[k], after.value[k]));
  return {
    ok: changedKeys.length === 0,
    reason: changedKeys.length === 0 ? null : "vencordDir 以外のキーが変わっている: " + changedKeys.join(", "),
    vencordDir,
    otherKeysSame: changedKeys.length === 0,
    changedKeys,
  };
}

// ---- 触った操作の数 -------------------------------------

// results: run-window.mjs が積んだ結果の配列 ({ name, ... } の列。record() が積むそのもの)。
// **数えるのは「OBS の状態を変えた操作」だけ** (ドックの押下と obs-websocket の Set 系)。
// 読むだけの呼び出し (GetSceneItemList・GetStats)・PNG の撮影・待ちは操作に数えない
// (数えるのは「利用者が触った操作」であり、実行スクリプトの観測は利用者の操作ではない)。
//
// 配信中 (record-start より後、配信を止めるより前) の操作だけを数え、種別ごとに分ける。
// [Vesktop に追加] と [cordmux のシーンを作成] は record-start より前に置いてあるので、
// ここでは before 側に入る。**配信を止めた後 (シーンコレクションの往復と
// 診断情報のエクスポート) は after 側**に分ける: この 3 つの欄はそのまま判定の根拠として
// 読まれるので、名前と中身を食い違わせない。
export function judgeOperationCount(results) {
  const list = results || [];
  const startIdx = list.findIndex((r) => r && r.name === "record-start");
  const stopIdx = list.findIndex((r) => r && r.name === "stream-stop");
  const during = [];
  const before = [];
  // **配信を止めた後の操作を before に混ぜない**: call-1 ではシーンコレクションの
  // 往復と診断情報のエクスポートが stream-stop より後にあり、それを「配信前に触った操作」
  // として数えると、判定の根拠として読む結果 JSON の欄が中身と食い違う (判定そのものは
  // during だけを見るので変わらない)。
  const after = [];
  for (let i = 0; i < list.length; i += 1) {
    const r = list[i] || {};
    let kind = null;
    // dock-click は、参加者の行のチェック (実行計画が slotOp の印を付けた物。run-window.mjs が結果へコピーする) だけを
    // 「付け外し」と数え、それ以外 ([cordmux のシーンを作成] 等) は「その他」に分ける。ウィンドウのコンポーネントを押した dialog-click
    // (メニュー・設定ウィンドウ・確認のウィンドウ) も「その他」(読むだけの -ListOnly は数えない)。実行計画が readOnlyOp の印を付けた押下は、
    // 設定ウィンドウを開いて表を読み、変えずに閉じるだけ (OBS と cordmux の状態を変えない) なので数えない。
    if (r.name === "dock-click") kind = r.slotOp ? "slot" : r.muteOp ? "mute" : "other";
    else if (r.name === "dialog-click") kind = r.pressed === true && r.readOnlyOp !== true ? "other" : null;
    else if (r.name === "obsws-request" && typeof r.request === "string" && r.request.startsWith("Set")) kind = "other";
    else if (r.name === "set-source-filter-setting") kind = "other";
    if (kind === null) continue;
    const entry = { at: i, name: r.name, kind, label: r.request || r.elementName || r.name };
    const inWindow = startIdx >= 0 && i > startIdx && (stopIdx < 0 || i < stopIdx);
    if (inWindow) during.push(entry);
    else if (stopIdx >= 0 && i >= stopIdx) after.push(entry);
    else before.push(entry);
  }
  const counts = { slot: 0, volume: 0, mute: 0, other: 0 };
  for (const e of during) counts[e.kind] += 1;
  return {
    // 配信中に触った操作が「付け外し・音量・ミュート」以外にないこと。
    ok: startIdx >= 0 && counts.other === 0,
    reason: startIdx < 0 ? "record-start がない (配信中の区間が定まらない)" : counts.other === 0 ? null : "付け外し・音量・ミュート以外の操作が配信中にある",
    counts,
    duringCount: during.length,
    during,
    beforeCount: before.length,
    before,
    afterCount: after.length,
    after,
  };
}

// ---- 実行スクリプトだけが知る値 (call-window-artifacts.json) --------

// コマンドライン (--browser-profile / --shared-content-dir / --temp-vencord-build) で渡された
// 「実行スクリプトが用意したもの」を 1 つの形にまとめる。1 つも渡されていなければ null を返す
// (何も書かない = cleanup.ps1 の「無ければ何もしない」と対になる)。
//
// **なぜコマンドラインで渡すのか**: ブラウザを起動するツールはまだなく、call-1 の実行計画にも
// ブラウザを起動する手順がない。それでも一時的な Vencord の
// ビルドとブラウザのプロファイルは実行の当日に PC の上にできるので、**後片付けの側
// だけでも先に成り立たせる**。起動するツールができたら、そのツールが同じファイルへ
// 追記する (下の mergeCallWindowArtifacts は追記する側のための形でもある)。
export function callWindowArtifactsFromArgs(args) {
  const source = args || {};
  const browserProfiles = source.browserProfiles || [];
  const sharedContentDirs = source.sharedContentDirs || [];
  const tempVencordBuild = source.tempVencordBuild || null;
  if (browserProfiles.length === 0 && sharedContentDirs.length === 0 && tempVencordBuild === null) return null;
  return { tempVencordBuild, browserProfiles, sharedContentDirs };
}

// 既にあるファイルの中身 (existing。無ければ null) へ additions を足した中身を返す。
// **消さずに足すだけ**にする: 今回の実行で渡されたものだけを書いて前の実行の分を落とすと、
// 落とした側が PC に残る (テストの残骸ファイルを残さない)。並びは先に書かれたものを
// 先に保ち、同じ場所は 1 度だけにする。tempVencordBuild は 1 つしかないので、渡された
// ときだけ差し替える (渡されなければ既にある値を残す)。
export function mergeCallWindowArtifacts(existing, additions) {
  const base = existing || {};
  const add = additions || {};
  const union = (a, b) => {
    const out = [];
    for (const value of [...(Array.isArray(a) ? a : []), ...(Array.isArray(b) ? b : [])]) {
      if (typeof value !== "string" || value.length === 0) continue;
      if (out.indexOf(value) === -1) out.push(value);
    }
    return out;
  };
  return {
    tempVencordBuild: add.tempVencordBuild || base.tempVencordBuild || null,
    browserProfiles: union(base.browserProfiles, add.browserProfiles),
    sharedContentDirs: union(base.sharedContentDirs, add.sharedContentDirs),
  };
}

// 録画ログの finalize の行の gaps (穴の開始と長さ。どちらもマイクロ秒) を
// black_span.py の --gaps が読む形 ({ start_frame, frames }) に変換する。
//
// **フレーム番号に変換する fps は、その人の映像の区間の行が持つ値を使う** (finalize の行は fps を
// 持たない)。fps が 1 つも読めなければ**失敗にする**: 根拠のない推測 (60 と決め打ち) で
// 番号を作ると、合ったか合わなかったかが分からないまま成功にも失敗にもなる。
// 穴の位置はセッションの原点から数えており、完成した映像も同じ原点から始まる
// (record_args.h の cursor_us が 0 から始まる) ので、そのままフレーム番号になる。
export function buildGapFrames(rows, finalizeRow) {
  if (!finalizeRow || !Array.isArray(finalizeRow.gaps)) return { error: "録画ログに録画ファイルの作成の行の gaps がない" };
  const userId = String(finalizeRow.user_id || "");
  let fps = 0;
  for (const seg of rows ? rows.segment : []) {
    if (String(seg.user_id || "") !== userId) continue;
    if (String(seg.media || "video").toLowerCase() !== "video") continue;
    const num = Number(seg.fps_num);
    const den = Number(seg.fps_den);
    if (Number.isFinite(num) && Number.isFinite(den) && num > 0 && den > 0) {
      fps = num / den;
      break;
    }
  }
  if (fps <= 0) return { error: "録画ログの区間の行から fps を決められない (user_id=" + userId + ")" };
  const frames = [];
  for (const gap of finalizeRow.gaps) {
    const startUs = Number(gap && gap.start_us);
    const durationUs = Number(gap && gap.duration_us);
    if (!Number.isFinite(startUs) || !Number.isFinite(durationUs) || durationUs <= 0) continue;
    frames.push({
      start_frame: Math.round((startUs * fps) / 1e6),
      frames: Math.round((durationUs * fps) / 1e6),
    });
  }
  return { fps, frames };
}

// no-leading-black の --allow-leading に渡すフレーム数: 録画ログの先頭の穴 (start_frame 0 の穴) の長さ。
// セッションの原点は全区間の先頭の最小値なので、遅れて始まった人の完成した映像の先頭は正当に黒で
// 埋まる。**録画ログが読めない・fps が決められない・先頭の穴がないときは 0**
// (先頭の黒を 1 フレームも許さない)。確認していないものを理由に黒を通さない。
export function leadingGapFrames(rows, finalizeRow) {
  const gaps = buildGapFrames(rows, finalizeRow);
  if (gaps.error) return 0;
  const head = gaps.frames.find((g) => g.start_frame === 0);
  return head ? head.frames : 0;
}

// ---- 期限 --------------------------------------------------------------

export function deadlineExceeded(startMs, nowMs, deadlineMin) {
  return nowMs - startMs >= deadlineMin * 60 * 1000;
}

// ---- 配置の一覧 ----------------------------------------------------------

// pluginDir: --plugin の値そのもの (cmake --install の出力にある、cordmux.dll と
// data/ が直下にあるフォルダ。配布用の zip の cordmux/ フォルダと同じ形)。
// cordmux.dll と data/ は必須 (無ければ place 失敗)。cordmux.dll 以外の直下のファイルは個々のファイル名を
// 決め打ちせず「cordmux.dll 以外の直下のファイル全部」を一括で運ぶ (なくても必須にはしない = 任意)。
// 配布用の zip は CI が作り、直下には FFmpeg の 11 個の DLL + ffmpeg.exe が
// 並ぶ (一覧は CMake の 1 か所)。実行スクリプトが同じ一覧を持つと 2 か所になるので、ここは
// 並んでいるものを全部運ぶ形のまま使う。置き先は OBS 33 の置き方 (<obs>/plugins/cordmux/ の直下と
// <obs>/plugins/cordmux/data)。--step install-roundtrip の --layout plugins は、zip の中の
// cordmux フォルダをそのまま <obs>/plugins の中へ置く。
export function filesToPlace(pluginDir) {
  const binDir = pluginDir;
  const dataDir = pluginDir + "/data";
  return [
    {
      id: "cordmux-dll",
      kind: "copy-file",
      required: true,
      label: "cordmux.dll",
      src: binDir + "/cordmux.dll",
      destRel: "plugins/cordmux/cordmux.dll",
    },
    {
      id: "sibling-dlls",
      kind: "copy-dir-contents",
      required: false,
      label: "同梱 DLL 一式 (cordmux.dll を除く直下の全ファイル。FFmpeg の 11 個の DLL + ffmpeg.exe)",
      src: binDir,
      destRel: "plugins/cordmux",
      excludeNames: ["cordmux.dll"],
    },
    {
      id: "data-dir",
      kind: "copy-dir",
      required: true,
      label: "data (locale 等)",
      src: dataDir,
      destRel: "plugins/cordmux/data",
    },
  ];
}

// ---- 実行計画 ------------------------------------------------------------

// place の確認: 種類の id で見る合成のソース (cordmux)。製品はシーンもソースも作らないので、wire-composite が置いた物を見る。
function expectedSources() {
  return [{ kind: "composite" }];
}

// 録画するシーン (Main) へ合成のソース cordmux をキャンバス全体で置く手順 (どの実行も同じ置き方)。画面共有も声も 1 つのソースが描いて
// 鳴らすので、枠ごとのソースは置かない。note はその実行に特有の理由 (置かないと何が起きないか)。
function wireCompositeStep(note) {
  return step("wire-composite", "録画するシーン (" + WIRE_SCENE_NAME + ") へ合成のソース (" + CORDMUX_SOURCE_KINDS.composite + ") をキャンバス全体で置く" + (note ? " (" + note + ")" : ""), {
    inputName: COMPOSITE_INPUT_NAME,
  });
}

// id は組み立て終わった配列に対して後から連番を振る (assignIds)。ここでは kind と label と
// 追加の項目だけを持つ「仮の」手順オブジェクトを作る (モジュールの外に出る共有の可変状態を
// 持たないため、連番はこの関数の外の純粋な後処理に任せる)。
function step(kind, label, extra) {
  return Object.assign({ kind, label }, extra || {});
}

function assignIds(steps) {
  const counts = {};
  return steps.map((s) => {
    const n = (counts[s.kind] || 0) + 1;
    counts[s.kind] = n;
    return Object.assign({ id: s.kind + "-" + n }, s);
  });
}

// 配置 (seed-config + ファイルの配置) と、起動 + obsws 接続を分けたのは --step discord-first
// のため: 送信側を先に起動してから OBS を起動する必要があり、配置だけを先に
// 済ませて起動を遅らせる構成が要る。他の 5 種類の step は続けて呼ぶだけで同じ動きになる。
// OBS の見た目と言語のフラグが 1 つでも渡されたか。
export function obsLookGiven(look) {
  const l = look || {};
  return Boolean(l.language || l.theme || l.fontScale !== null && l.fontScale !== undefined || l.density !== null && l.density !== undefined || l.hdr ||
    typeof l.fullHeightDocks === "boolean");
}
// --no-seed の実行は配置だけ行う (seed-config を通さない。前の実行の OBS の設定・シーンコレクション・cordmux の settings.json を
// そのまま使う)。更新の確認は、古いバージョンのファイルで 1 回起動して閉じ、現在のファイルを --no-seed で置いて
// 起動し、古いバージョンが書いた設定が現在のバージョンでどう読まれるかを見る。
function placeOnlySteps(args) {
  return filesToPlace(args.plugin).map((f) => step(f.kind, "配置: " + f.label + " (--no-seed: 初期設定はしない)", { obs: args.obs, place: f }));
}

function seedAndPlaceSteps(args) {
  const steps = [];
  // 「テスト用にインストールしたファイルを置く」ことと「偽の state.json を作る」ことは切り離す。call-* は
  // --appdata を渡さない (= 偽の state.json を作らない) ので、ファイルだけが置かれ、実際の
  // state.json は製品の [Vesktop に追加] だけが書く。
  const useAppData = !args.realProfile;
  // call-* は必ず立てる (ゲートは製品に実際の state.json を書かせる場面そのもの)。オプションを渡すのは
  // call-* だけである (seed-config.py は call-* 以外のオプションを受け付けない)。書く値は
  // seed-config.py が step と --appdata / --real-profile から決める (call-* 以外で --real-profile なら
  // 必ず false、--appdata なら製品の既定と同じ true)。値は --dry-run の出力 (seedPreview) に出る。
  const allowStateJsonWrite = isCallStep(args.step);
  // 録画の保存先はどの step でも必ず差し替える。ここで渡さない呼び方を残すと、cordmux の既定
  // (OBS の録画の保存先) のまま実行され、実際の録画フォルダーへ書いてしまう。seed-config.py の側にも
  // 同じ安全のための確認 (作業用のフォルダーの下でなければ何も書かずに終了コード 3) がある。
  const isRecordStep = args.step === "record";
  // call-* は、テスト用にインストールしたファイルの data/vencord に 4 ファイルが必ず要る (seed_vencord_bundle は
  // elsewhere / absent では置かない)。呼び出し行の --dock-install に任せると、既定 (elsewhere) のまま実行して
  // [Vesktop に追加] が空のフォルダーを Vesktop に指させることになるので、実行計画が決める (record が assign auto を決めるのと同じ形):
  //   実際の通話の実行 = ours (state.json は作らないので、動作するのは 4 ファイルを置くことだけ)
  //   リハーサル = absent (偽の state.json に vencordDir を持たせず、実行スクリプトが [Vesktop に追加] を押して
  //   製品に書かせる。バックアップができるので アンインストール の判定まで通る。4 ファイルは次の place-vencord-bundle が置く)
  const dockInstall = isCallStep(args.step) ? (args.rehearsal ? "absent" : "ours") : args.dockInstall;
  steps.push(
    step(
      "seed-config",
      "OBS の設定とシーンコレクションを作り直す (seed-config.py)。" +
        (useAppData ? "APPDATA を差し替えた配下に偽の state.json も作る" : "実際の state.json は作らない (--appdata なし)") +
        " (--dock-install " +
        dockInstall +
        "、--assign-mode " +
        args.dockAssignMode +
        "、--decoder " +
        (args.decoder || DEFAULT_DECODER) +
        (args.displayBufferMs != null ? "、--display-buffer-ms " + args.displayBufferMs : "") +
        (args.labelEmRatioPct != null ? "、--label-em-ratio-pct " + args.labelEmRatioPct.toFixed(1) : "") +
        (allowStateJsonWrite ? "、--allow-state-json-write" : "") +
        "、録画の保存先は " +
        RECORD_DIR_REL +
        " へ差し替え)",
      {
        obs: args.obs,
        out: args.out,
        // seed-config.py は step を受け取って、書き込みの規則を適用する。
        step: args.step,
        shape: "test",
        realProfile: Boolean(args.realProfile),
        dockInstall,
        assignMode: args.dockAssignMode,
        decoder: args.decoder || DEFAULT_DECODER,
        displayBufferMs: args.displayBufferMs ?? null,
        // 名前ラベルの板を plate_measure.py で測る call-1 は、板を出して始める (既定は板が出ない)。
        ...(args.step === "call-1" ? { labelPlate: true } : {}),
        allowStateJsonWrite,
        useAppData,
        // リハーサル (call-1 + 差し替えた APPDATA)。seed-config.py はこのオプションがないと call-* を --appdata で断る。
        // 照合の周期は 5 秒に縮める (連続運転の待ちも REHEARSAL_TIME_FACTOR で縮むので、既定の 300 秒では
        // わざと起こした異常の案内が待ちの間に出ない)。
        rehearsal: Boolean(args.rehearsal),
        ...(isCallStep(args.step) && args.rehearsal ? { vencordCheckDisconnectedS: ROUNDTRIP_CHECK_PERIOD_S, vencordCheckConnectedS: ROUNDTRIP_CHECK_PERIOD_S } : {}),
        recordDirRel: RECORD_DIR_REL,
        // OBS の見た目と言語 (渡したときだけ持つ。seed-config.py の --language などになる)。
        ...(obsLookGiven(args.obsLook) ? { obsLook: args.obsLook } : {}),
        // テスト用の OBS のプロファイルにリプレイバッファを設定する (渡したときだけ。seed-config.py の --replay-buffer)。
        ...(args.replayBuffer ? { replayBuffer: true } : {}),
        // hold の --self-mic: 自分の枠の音声ソースの名前を settings.json に書く (seed-config.py の --self-speaking-source-name)。
        ...(args.selfMic ? { selfSpeakingSourceName: SELF_MIC_INPUT_NAME } : {}),
        // hold の --label-em-ratio-pct: 名前の大きさを settings.json に書く (seed-config.py の --label-em-ratio-pct)。
        ...(args.labelEmRatioPct != null ? { labelEmRatioPct: args.labelEmRatioPct } : {}),
        ...(isRecordStep
          ? {
              recordSpaceCheckIntervalS: RECORD_SPACE_CHECK_INTERVAL_S,
              recordSpaceWarnMinutes: RECORD_SPACE_WARN_MINUTES,
            }
          : {}),
      },
    ),
  );
  if (isCallStep(args.step) && args.rehearsal) {
    // absent は 4 ファイルを置かないので、実行スクリプトが VENCORD_DIST_REL から、Vesktop に見せる場所と原本の両方へコピーする
    // (seed-config.py の ours と同じ中身。tamper の判定が原本との照合で出るために原本も要る)。
    steps.push(
      step("place-vencord-bundle", "テスト用にインストールしたファイルの data/vencord と data/vencord-src へ Vencord のビルド成果 (" + VENCORD_DIST_REL + ") をコピーする (リハーサル: absent の初期設定は置かないため)", {
        obs: args.obs,
        distRel: VENCORD_DIST_REL,
        names: VENCORD_BUNDLE_FILE_NAMES,
        gate: true,
      }),
    );
  }
  for (const f of filesToPlace(args.plugin)) {
    steps.push(step(f.kind, "配置: " + f.label, { obs: args.obs, place: f }));
  }
  return steps;
}

function launchAndConnectSteps(args) {
  // call-* は実際の保存先のまま起動する (実機の送信部が読む
  // %LOCALAPPDATA%\cordmux\session.json と、受信側が書く先を同じにするため)。
  const profileNote = args.realProfile
    ? "実際の %LOCALAPPDATA% / %APPDATA% のまま"
    : "LOCALAPPDATA を " + LOCAL_APPDATA_OVERRIDE_REL + " に差し替え";
  return [
    step("launch-obs", "OBS を起動する (" + profileNote + "、" + OBS_LAUNCH_ARGS.join(" ") + ")", {
      obs: args.obs,
      wsPort: OBSWS_CONTROL_PORT,
      realProfile: Boolean(args.realProfile),
    }),
    step("obsws-connect", "obsws.mjs で obs-websocket に繋ぐ", { wsPort: OBSWS_CONTROL_PORT }),
  ];
}

// 起動と配置は place/through/integration/guard/second-obs の 5 種類の step すべてで共通 (place だけを
// 先に実行したあとの他の step から呼んでも、配置をやり直すだけで副作用はない = 冪等)。
function commonSetupSteps(args) {
  return [...seedAndPlaceSteps(args), ...launchAndConnectSteps(args)];
}

function closeSteps(target, label) {
  return [step("close-obs", label || "OBS を穏やかに閉じる (駄目なら強制終了)", { target: target || "first" })];
}

// write-result -> summarize -> cleanup の並びは STEP_VALUES すべてで共通。各 step の終わりに
// summarize-window.py を結果 JSON と OBS のログに対して実行し、出力を結果 JSON の summary に入れる。
// record だけ、後始末で録画のフォルダーを消さない
// (--step record の録画を、あとで別の確認に使うため)。残したものを消すのは
// 確認が終わった後に cleanup.ps1 をオプションなしで 1 回実行するとき (README.md の実行の手順)。
// 実行スクリプトの finally の後始末にも同じ一覧を渡す。finishSteps の cleanup が残しても、
// 手順の外の finally が -KeepRecordings なしで cleanup.ps1 をもう 1 回呼ぶと、録画物が毎回消えるため。
// call-1 も残す。実行の途中で録画を ON にした録画を、実行の後に tone_scan.py や目視で確認するため。
// -KeepRecordings が残すのは名前が recordings のフォルダーの下だけで、OBS の録画 (av1 / av2 / main など) は
// --out の直下に書かれるので、このオプションがあっても消える。OBS の録画は keep-obs-recordings が recordings の下へ移す
// (STEPS_KEEPING_OBS_RECORDINGS)。残した録画物は、実行の後に人が cleanup.ps1 をオプションなしで 1 回実行して消す。
// call-hold も残す (border_timing.py が実行の後に録画を読むため。STEPS_KEEPING_OBS_RECORDINGS の欄を参照)。
export const STEPS_KEEPING_RECORDINGS = ["record", "call-1", "call-hold"];

// 後始末で録画のフォルダーを残すか。手順の cleanup と実行スクリプトの finally の cleanup が同じこの関数を読むので、
// 片方だけ残す状態にならない。上の一覧の step は常に残す。hold は --keep-recordings を渡したときだけ残す。
// cordmux の録画の途中でテスト用の OBS を強制終了し、起動し直して作成待ちのカードから [作成を再開] を押す実行では、
// 素の hold の後始末が *.mov と *.jsonl を _work の下から全部消すので、起動し直す前に落ちた実行の区間と録画ログがなくなる。
// hold を一覧に足して常に残すと、録画を使わない hold の後でも、残した録画物を人が消す手間が増える。
// 落ちた実行のフォルダーを build_x64 へコピーして起動し直しの前に戻す方法は、手作業が多くコピーが残る。
export function keepsRecordings(args) {
  const stepName = args && args.step;
  if (STEPS_KEEPING_RECORDINGS.includes(stepName)) return true;
  return stepName === "hold" && Boolean(args.keepRecordings);
}

function finishSteps(args, stepName) {
  const keepRecordings = keepsRecordings({ ...args, step: stepName });
  // call-1 は OBS の録画も残す (STEPS_KEEPING_OBS_RECORDINGS の欄)。OBS を閉じた後、結果を書く前に移し、移した先を
  // 結果 JSON に残す (途中で抜けた経路は finally が同じことをする)。
  const keepObs = STEPS_KEEPING_OBS_RECORDINGS.includes(stepName)
    ? [step("keep-obs-recordings", "OBS の録画 (label の付いたもの) を " + OBS_RECORDINGS_KEEP_DIR_PREFIX + "<tag> へ移す (後始末が残す場所。消すのは後で cleanup.ps1 を素で実行したとき)", {})]
    : [];
  return [
    ...keepObs,
    step("write-result", "結果 JSON を書く", { out: args.out, step: stepName, tag: args.tag }),
    step("summarize", "summarize-window.py を結果 JSON と OBS のログに対して実行し、出力を summary に入れる", {}),
    // call-* でも callWindow は false のまま。-CallWindow は vencordDir をバックアップの元の値へ
    // 戻す操作であり、これを step の終わりに毎回実行するとゲートの (6)(7) (書いた値を残したまま
    // Vesktop を閉じて開き直す) が成り立たなくなる (次の step が始まる前に戻ってしまう)。
    // 戻す操作は `cleanup.ps1 -CallWindow` を単独で実行して行う
    // (OBS が落ちていても戻せる)。最後に戻す実行 (call-1・call-hold) が
    // 戻ったと確認する前に終わったときだけ、実行スクリプトの finally が -CallWindow を付ける (finallyVencordRestorePlan)。
    // この手順の cleanup には付けない。
    step(
      "cleanup",
      "cleanup.ps1 を通す (巻き戻し (-CallWindow) は含まない" + (keepRecordings ? "。録画のフォルダは残す = -KeepRecordings" : "") + ")",
      { callWindow: false, keepRecordings },
    ),
  ];
}

// --step mic-check: 声のテスト用の信号を、ブラウザ (Chromium 系) の起動オプションで作れるかを、
// 4 アカウントの通話より前に、テスト用の送信側だけで 1 回確認する。
// OBS は一切起動しない (seed-config も配置も launch-obs も含まない。commonSetupSteps を使わない
// 唯一の --step)。tasklist / nvidia-smi の開始前の確認は run-window.mjs の main() が全 --step
// 共通で行うので、ここでは持たない。test/tools/mic-check/run.mjs が harness と同じ Electron
// (test/tools/harness/node_modules/electron) をフェイクデバイスの起動オプションつきで起動し、
// 結果 JSON を読んで判定する。write-result/summarize/cleanup は他の --step と同じ形を保つ
// (summarize-window.py は "mic-check" という名前の手順を知らないが、知らない手順は無視して
// 進むだけなので壊れない)。
function buildMicCheckSteps(args) {
  return [
    step(
      "mic-check",
      "ブラウザ (Chromium 系) の起動時の指定 (--use-fake-device-for-media-stream / " +
        "--use-file-for-fake-audio-capture) で、マイク入力の代わりに音のファイルを読ませられるかを確認する",
      {},
    ),
    ...finishSteps(args, "mic-check"),
  ];
}

function buildPlaceSteps(args) {
  const steps = commonSetupSteps(args);
  steps.push(step("read-obs-log", "OBS のログでモジュール一覧に cordmux があるか (Loaded Modules)", { pattern: "module-loaded", moduleName: "cordmux" }));
  steps.push(wireCompositeStep("置いた後の存在を次の手順で見る"));
  steps.push(step("obsws-call", "合成のソースの存在を型 id で見る", { request: "GetInputList", expectSources: expectedSources() }));
  steps.push(step("check-file-exists", "session.json の存在", { path: SESSION_JSON_REL }));
  steps.push(step("icacls", "session.json の権限 (利用者だけ読めるか)", { path: SESSION_JSON_REL }));
  steps.push(step("launch-obs-second", "2 つ目の OBS を起こす (待ち受けを開かないはず)", { obs: args.obs }));
  steps.push(step("read-obs-log", "2 つ目の OBS のログで BlockedByOtherWriter を確認する", { pattern: "blocked-by-other-writer", target: "second" }));
  steps.push(...closeSteps("second", "2 つ目の OBS を閉じる"));
  steps.push(...closeSteps("first"));
  steps.push(...finishSteps(args, "place"));
  return steps;
}

// 録画は必ず harness (テスト用の送信側) より前に始め、harness が終わってから止める
// (OBS の出口の欠落・重複・A/V を測るには、送信側の立ち上がり
// (直列のラダー確保 + ウォームアップ 60 フレーム x ストリーム数 + 接続) から録画している必要がある。
// StartRecord -> 送信側 -> 送信側の終了待ち -> StopRecord の順を実行計画そのもので強制する)。
// record-stop の tailSeconds は、送信側が終わってから念のため録画を続ける余裕である。
const THROUGH_RECORD_TAIL_SEC = 5;
const INTEGRATION_RECORD_TAIL_SEC = 10;

function buildThroughSteps(args) {
  const steps = commonSetupSteps(args);
  steps.push(wireCompositeStep("画面 1 本は合成のソースが全面に描く"));
  if (args.stream) steps.push(step("stream-sink-start", "配信の受け口 (ffmpeg -listen rtmp) を起こす", {}));
  steps.push(step("record-start", "録画を開始する (送り手より前に開始し、cordmux の絵を録り逃さない)", { stream: args.stream }));
  steps.push(step("harness", "テスト用の送信側を product モードで 1 本、10 秒 (HARNESS_MODE=product)", { mode: "product", n: 1, seconds: 10, hw: args.hw, warmup: args.warmup, syncTone: DEFAULT_SYNC_TONE }));
  steps.push(step("record-stop", "送り手の終了を待ってから録画を終了する (末尾の余裕 " + THROUGH_RECORD_TAIL_SEC + " 秒)", { stream: args.stream, tailSeconds: THROUGH_RECORD_TAIL_SEC }));
  if (args.stream) steps.push(step("stream-sink-stop", "配信の受け口を閉じる", {}));
  steps.push(step("collect-recording", "録画ファイルの回収"));
  steps.push(step("judge", "frames.py で欠落・重複・凍りを見る", { tool: "frames.py" }));
  steps.push(step("collect-plugin-log", "プラグインの記録 ([cordmux] 行) の回収"));
  steps.push(...closeSteps("first"));
  steps.push(...finishSteps(args, "through"));
  return steps;
}

// --integration-config が選ぶ、この呼び出しでの画面と声のストリーム数・秒数・
// マーカーの音の乗せ先・全種の判定ツールを実行するか。"default" は --n-screen/--n-voice/--seconds を
// そのまま使う (4 ストリーム + 8 ストリームの定常の欠落の測り直しはこれで行う)。
// "one-screen" と "one-screen-one-voice" は固定値の構成で、CLI の
// --n-screen/--n-voice/--seconds を上書きする。値が食い違っていても黙って上書きする。
// 名前付き構成を選んだ時点で、数値側の指定は無視してよいため。
function integrationScenario(args) {
  if (args.integrationConfig === "one-screen") {
    // 画面 1 + 声 0 で 1 分の構成。syncTone=screen を明示する。
    return { nScreen: 1, nVoice: 0, seconds: ONE_SCREEN_SECONDS, syncTone: DEFAULT_SYNC_TONE, fullJudge: true };
  }
  if (args.integrationConfig === "one-screen-one-voice") {
    return { nScreen: 1, nVoice: 1, seconds: ONE_SCREEN_ONE_VOICE_SECONDS, syncTone: "voice", fullJudge: false };
  }
  return { nScreen: args.nScreen, nVoice: args.nVoice, seconds: args.seconds, syncTone: DEFAULT_SYNC_TONE, fullJudge: true };
}

function buildIntegrationSteps(args) {
  const scenario = integrationScenario(args);
  const cells = scenario.nScreen;
  const steps = commonSetupSteps(args);
  steps.push(wireCompositeStep("画面 " + scenario.nScreen + " 本を" + (cells <= 1 ? "全面" : " 4 等分") + "、声 " + scenario.nVoice + " 本は合成のソースの音"));
  if (args.stream) steps.push(step("stream-sink-start", "配信の受け口 (ffmpeg -listen rtmp) を起こす", {}));
  steps.push(step("record-start", "録画を開始する (送り手より前に開始し、cordmux の絵を録り逃さない)", { stream: args.stream }));
  steps.push(
    step("harness", "テスト用の送信側を product モードで " + scenario.nScreen + " 本 + " + scenario.nVoice + " 本、" + scenario.seconds + " 秒 (--integration-config " + args.integrationConfig + ")", {
      mode: "product",
      nScreen: scenario.nScreen,
      nVoice: scenario.nVoice,
      seconds: scenario.seconds,
      hw: args.hw,
      warmup: args.warmup,
      syncTone: scenario.syncTone,
    }),
  );
  steps.push(step("record-stop", "送り手の終了を待ってから録画を終了する (末尾の余裕 " + INTEGRATION_RECORD_TAIL_SEC + " 秒)", { stream: args.stream, tailSeconds: INTEGRATION_RECORD_TAIL_SEC }));
  if (args.stream) steps.push(step("stream-sink-stop", "配信の受け口を閉じる", {}));
  steps.push(step("collect-recording", "録画ファイルの回収"));
  if (scenario.fullJudge) {
    steps.push(step("judge", "frames.py で欠落・重複・凍りを見る" + (cells > 1 ? " (2x2 の区画ごと)" : ""), { tool: "frames.py", cells }));
    steps.push(step("judge", "sync.py で A/V のずれを見る" + (cells > 1 ? " (2x2 の区画ごと)" : ""), { tool: "sync.py", cells }));
    steps.push(step("judge", "smooth.py でフレーム間隔の粗さを見る", { tool: "smooth.py", ncells: cells }));
  } else {
    // A/V の測り直し (画面 1 + 声 1 でマーカーは声)。判定は sync.py を画面の
    // 区画で実行し、その値を「声の音 - 画面の映像」として記録する。
    // sync.py 自体は cell の映像 (白のマーカー) と録画全体の音 (声の 2 kHz、1 ストリームしかない) を
    // 組にするだけなので、呼び方は通常の sync.py と同じで、label で意味を書き添える。
    steps.push(step("judge", "sync.py で「声の音 - 画面の映像」のずれを見る", { tool: "sync.py", cells, label: "voice_minus_screen_video_ms" }));
  }
  steps.push(step("collect-plugin-log", "プラグインの記録から段ごとの計数 (StreamStageCounters) を集める"));
  steps.push(...closeSteps("first"));
  steps.push(...finishSteps(args, "integration"));
  return steps;
}

// pipeline.cpp の stats_loop は kStatsLogInterval (10 秒) ごとにしか
// [cordmux] stats video 行を出さない。この 10 秒の周期は OBS が起動した時点から進んでおり
// harness の送信開始とは無関係にずれているので、既定の 10 秒 (HARNESS_SECONDS の既定) のままだと
// 送信側が繋がって送り終わるまでの間に 1 回も出ないことがありうる。2 周期ぶんの余裕を見て
// 20 秒にする。
const GUARD_OVERSEND_HARNESS_SECONDS = 20;

function buildGuardSteps(args) {
  const steps = commonSetupSteps(args);
  steps.push(step("check-file-exists", "session.json を読んで番号と合言葉を取る", { path: SESSION_JSON_REL }));
  steps.push(step("ws-probe", "合言葉なしで繋ぐ (拒否されるはず)", { variant: "no-secret" }));
  steps.push(step("ws-probe", "違う合言葉で繋ぐ (拒否されるはず)", { variant: "wrong-secret" }));
  steps.push(
    step("harness", "背圧: 送りすぎのパラメータで 1 本、" + GUARD_OVERSEND_HARNESS_SECONDS + " 秒 (overflow は 4 倍速に刻み直され、必ず接続まで進む)", {
      mode: "product",
      n: 1,
      seconds: GUARD_OVERSEND_HARNESS_SECONDS,
      extra: { oversend: true },
      syncTone: DEFAULT_SYNC_TONE,
    }),
  );
  // 背圧を含む守りの確認: テスト用の送信側の overflow が接続
  // (established) に至ったかを確認し、送信側の droppedByBackpressure と受信側のログの待ち
  // 行列の破棄数 (stats video の dropped) の両方を結果に残す。確立しなかった、または破棄した数が
  // 0 なら NG にする (judgeBackpressure)。
  steps.push(step("judge-backpressure", "背圧: テスト用の送信側の overflow が接続に至ったか (establishedReached) と、送り手/受け手それぞれの捨てた数を確認する", {}));
  // failStage は「この段数まで isConfigSupported を故意に不支持にする」個数 (harness README の
  // HARNESS_FAIL_STAGE の定義。文字列ではなく数)。3 にして Native/HalfFps/Downscaled の 3 段を
  // 落とし、SoftwareFloor に着地させる (降格ラダーの実行そのものを踏む)。
  steps.push(step("harness", "降格: 3 段 (Native/HalfFps/Downscaled) を故意に落として SoftwareFloor に着地させる", { mode: "product", n: 1, hw: "prefer-hardware", extra: { failStage: 3 }, syncTone: DEFAULT_SYNC_TONE }));
  steps.push(step("harness", "負荷を下げる手段 1: 枠ごとの長辺の上限", { mode: "product", n: 1, extra: { maxLongEdge: 1280 }, syncTone: DEFAULT_SYNC_TONE }));
  steps.push(step("harness", "負荷を下げる手段 2: fps を半分にする", { mode: "product", n: 1, extra: { fpsDivisor: 2 }, syncTone: DEFAULT_SYNC_TONE }));
  steps.push(step("harness", "負荷を下げる手段 3: prefer-software を選ぶ", { mode: "product", n: 1, hw: "prefer-software", syncTone: DEFAULT_SYNC_TONE }));
  steps.push(step("collect-plugin-log", "プラグインのログに合言葉が 1 文字もないことを確認する (残りの経路)"));
  steps.push(...closeSteps("first"));
  steps.push(...finishSteps(args, "guard"));
  return steps;
}

// --step second-obs: 2 つ目の OBS の起動と、シーンコレクションの切り替えを確認する。
// 2 つ目の OBS の BlockedByOtherWriter の確認は place と同じ手順をそのまま繰り返す。
// 切り替え自体は録画中に行わない。OBS の UI は出力中のシーンコレクションの切り替えを禁じていて、
// obs-websocket 経由でも同じ制限が掛かるかは測っていないので、録画は切り替えを戻した後だけにする。
//
// harness-start の直後にそのまま切り替えると、送信側がまだ接続を確立する
// 前に切り替えが終わってしまう (エンコーダーの直列のラダー確保 +
// ウォームアップ 60 フレームが socket を開くより前に走るため。sender.ts 784-813)。wait-connection で
// hello を実際に見つけるまで待ってから切り替える。judgeConnectionHeld も「hello の総数が
// 1 回」だけでは、切り替えが起きなかった (obsws-request は例外を握って ok:false を返す
// だけで先へ進む) 場合や、切り替えの後に接続が切れたまま繋ぎ直らなかった場合を見分け
// られないので、切り替え前 (wait-connection の時点) と切り替え後 (record-stop の直後、
// harness-wait より前 = harness 自身が終わるときの close がログに混ざる前) の 2 点のログを
// 比べ、hello/close が増えていないか、かつ切り替えが実際に (SetCurrentSceneCollection の
// 成功と、切り替え中の GetSceneCollectionList が test-empty を返したことの両方で) 確認
// できたかを見る。
function buildSecondObsSteps(args) {
  const steps = commonSetupSteps(args);
  steps.push(wireCompositeStep("画面 1 本を全面"));
  steps.push(step("launch-obs-second", "2 つ目の OBS を起こす (待ち受けを開かないはず)", { obs: args.obs }));
  steps.push(step("read-obs-log", "2 つ目の OBS のログで BlockedByOtherWriter を確認する", { pattern: "blocked-by-other-writer", target: "second" }));
  steps.push(...closeSteps("second", "2 つ目の OBS を閉じる"));
  steps.push(
    step(
      "harness-start",
      "テスト用の送信側を product モードで 1 本、" + SECOND_OBS_HARNESS_SECONDS + " 秒を起動する (シーンコレクションの切り替えをまたいで繋がったままにする)",
      { mode: "product", n: 1, seconds: SECOND_OBS_HARNESS_SECONDS, hw: args.hw, warmup: args.warmup, syncTone: DEFAULT_SYNC_TONE },
    ),
  );
  steps.push(step("wait-connection", "送り手が接続を確立するまで待つ (確立を見ずに切り替えると測れないため)", { timeoutMs: SECOND_OBS_CONNECT_WAIT_TIMEOUT_MS }));
  steps.push(
    step("obsws-request", "空のシーンコレクション (" + SECOND_OBS_EMPTY_SCENE_COLLECTION + ") へ切り替える", {
      request: "SetCurrentSceneCollection",
      requestData: { sceneCollectionName: SECOND_OBS_EMPTY_SCENE_COLLECTION },
      track: "scene-collection-switch",
      trackField: "switchAwayOk",
    }),
  );
  steps.push(
    step("obsws-request", "切り替えが実際に反映されたか確認する", {
      request: "GetSceneCollectionList",
      track: "scene-collection-switch",
      trackField: "duringSwitchCollectionName",
    }),
  );
  steps.push(step("wait", "切り替えたまま待つ (送り手が繋がったままかを見る猶予)", { ms: SECOND_OBS_SWITCH_HOLD_MS }));
  steps.push(
    step("obsws-request", "元のシーンコレクション (" + SECOND_OBS_MAIN_SCENE_COLLECTION + ") へ戻す", {
      request: "SetCurrentSceneCollection",
      requestData: { sceneCollectionName: SECOND_OBS_MAIN_SCENE_COLLECTION },
      track: "scene-collection-switch",
      trackField: "switchBackOk",
    }),
  );
  steps.push(step("obsws-request", "戻した後、Main に枠のソースが残っているか確認する", { request: "GetSceneItemList", requestData: { sceneName: "Main" } }));
  steps.push(step("record-start", "録画を開始する (戻した後に枠のソースへ映像が再び乗ることを見る)", { stream: false }));
  steps.push(step("wait", "枠のソースへ映像が再び乗るのを録る猶予", { ms: SECOND_OBS_POST_SWITCH_RECORD_SEC * 1000 }));
  steps.push(step("record-stop", "録画を終了する", { stream: false, tailSeconds: 0 }));
  // harness-wait より前に置く: harness 自身が終わるときに送信側側から普通に切る close が
  // 記録に混ざる前の状態で、切り替えの前後だけを比べるため。
  steps.push(step("judge-connection-held", "OBS の記録から、切り替えの前後で hello / close が増えていないか (繋がったまま切り替えられたか) を確認する", {}));
  steps.push(step("harness-wait", "送り手の終了を待つ (establishedReached を記録する)", { seconds: SECOND_OBS_HARNESS_SECONDS }));
  steps.push(step("collect-recording", "録画ファイルの回収"));
  steps.push(step("judge", "frames.py で、戻った後に枠のソースへ映像が再び乗ることを見る", { tool: "frames.py" }));
  steps.push(step("collect-plugin-log", "プラグインの記録の回収"));
  steps.push(...closeSteps("first"));
  steps.push(...finishSteps(args, "second-obs"));
  return steps;
}

// --step discord-first: Discord を先に起動する場合の確認。送信側 (harness) を先に
// 起動し、DISCORD_FIRST_OBS_DELAY_MS ミリ秒後に OBS を起動して、送信側が繋ぎ直しで確立する
// (establishedReached) ことと絵が乗ることを確認する。
function buildDiscordFirstSteps(args) {
  const steps = [...seedAndPlaceSteps(args)];
  steps.push(
    step(
      "harness-start",
      "テスト用の送信側を先に起動する (OBS がまだない状態で繋ぎ直しを試みる)。product モードで 1 本、" + DISCORD_FIRST_HARNESS_SECONDS + " 秒",
      { mode: "product", n: 1, seconds: DISCORD_FIRST_HARNESS_SECONDS, hw: args.hw, warmup: args.warmup, syncTone: DEFAULT_SYNC_TONE },
    ),
  );
  steps.push(step("wait", "OBS を起動するまで待つ (Discord 先行の間隔)", { ms: DISCORD_FIRST_OBS_DELAY_MS }));
  steps.push(...launchAndConnectSteps(args));
  // obs-websocket に繋げた直後はプラグインの読み込みが終わっていないことがある。second-obs と同じく session.json (待ち受けが
  // 開いた印) を待ってから合成のソースを置く。
  steps.push(step("check-file-exists", "session.json の存在 (待ち受けができるまで待つ)", { path: SESSION_JSON_REL }));
  steps.push(step("wait", "プラグインの読み込みが落ち着く猶予", { ms: DISCORD_FIRST_SOURCES_WAIT_MS }));
  steps.push(wireCompositeStep("画面 1 本を全面"));
  steps.push(step("record-start", "録画を開始する (送り手の繋ぎ直しの後に絵が乗ることを見る)", { stream: false }));
  steps.push(step("wait", "繋ぎ直しと絵が乗るのを録る猶予", { ms: DISCORD_FIRST_RECORD_WAIT_SEC * 1000 }));
  steps.push(step("record-stop", "録画を終了する", { stream: false, tailSeconds: 0 }));
  steps.push(step("harness-wait", "送り手の終了を待つ (繋ぎ直しで establishedReached になることを確認する)", { seconds: DISCORD_FIRST_HARNESS_SECONDS }));
  steps.push(step("collect-recording", "録画ファイルの回収"));
  steps.push(step("judge", "frames.py で絵が乗ることを見る", { tool: "frames.py" }));
  steps.push(step("collect-plugin-log", "プラグインの記録の回収"));
  steps.push(...closeSteps("first"));
  steps.push(...finishSteps(args, "discord-first"));
  return steps;
}

// --step dock-view: ドックの表示を、状態ごとに PNG に撮って確認する。目視の対象を、
// 実行スクリプトが撮った PNG にして、人と機械が見られるようにする (実行の間、
// OBS の画面を人が見張らなくてよい)。--dock-scenario ごとに実行計画を変える
// (他の step と同じく seed-config から finishSteps までの形は踏襲する)。
const DOCK_VIEW_SETTLE_WAIT_MS = 5000; // scenario "none" の待ち (送信側がないので描画が落ち着くまでの猶予だけでよい)。
// obsws-connect が返った直後は、OBS のウィンドウやドックがまだ配置・描画の途中である
// 可能性があるため、撮る前に少し待つ。値は見積もりで、実測で見直す。

// scenario "connected" / "degraded" / "no-scene" / "contact-missing" で共通の接続待ち
// (second-obs の SECOND_OBS_CONNECT_WAIT_TIMEOUT_MS と同じ見積もり根拠: エンコーダーの直列の
// ラダー確保 + ウォームアップ 60 フレーム + electron の起動オーバーヘッド。値は見積もりで、実測で見直す)。
const DOCK_VIEW_CONNECT_WAIT_TIMEOUT_MS = 15000;
// 接続してから 10 秒後に撮る。connected 以外の 3 つ (no-scene / degraded /
// contact-missing) にも同じ猶予を揃える。揃えない理由がないため。
const DOCK_VIEW_POST_CONNECT_WAIT_MS = 10000;
// 警告が出ている間も出口の映像に差分がないことの再確認 (差分 0) 用の録画の長さ。
const DOCK_VIEW_AC24_RECORD_SECONDS = 10;
// harness-start の seconds (見積もり。実測で見直す):
// 接続待ち (最大 15 秒) + wire-composite + 撮るまでの猶予 (10 秒) + screenshot-window
// + 行のチェック + 5 秒 + screenshot-window (この 3 つは子プロセスの起動 + Add-Type の
// C# コンパイルを毎回挟むため 1 回あたり 3 から 8 秒。idle-10min の注記と同じ見積もり) +
// screenshot-source + 差分の再確認の録画 (10 秒) + record-stop と録画ファイルの回収 + judge +
// ログ回収で、最悪 70 秒前後。
// 送信側が録画の途中で自分で止まると frames.py の欠落が跳ね上がり、「警告が出ている間も
// 出口に差分がない」ことが判定できず、実行が丸ごと無駄になるので、
// idle-10min (600 秒の撮影ループに対して 780 秒) と同じくらいの厚みの余裕を取る (最悪の
// およそ 2 倍以上。run-window-plan.test.ts の「2 倍以上の長さで動く」テストが確認する)。
const DOCK_VIEW_CONNECTED_HARNESS_SECONDS = 150;
// no-scene / contact-missing は wire-composite も録画も行わない分、connected/degraded
// より短くてよい。
const DOCK_VIEW_CONNECT_ONLY_HARNESS_SECONDS = 45;
// scenario "contact-missing" が HARNESS_FAIL_CONTACT に渡す接点 id。8 接点のどれでも同じ
// 「missing を facts.warnings に載せる」経路を通るので、1 つを選んだ。roster は
// 「通話に参加している人が分からない」という結果が画面に大きく出るため選んだ。
export const DOCK_VIEW_FAIL_CONTACT_ID = "roster";

// scenario "idle-10min" (ドックを開いてから 10 分間、裏で動くコンポーネントが終了しないこと)。
export const DOCK_VIEW_IDLE_INTERVAL_MS = 60000; // 1 分ごと。
export const DOCK_VIEW_IDLE_INTERVALS = 10; // 10 分 = 1 分間隔 x 10 回。
// harness-start の seconds: 接続待ち (最大 15 秒) + 10 分間の撮影ループ (600 秒) +
// screenshot.ps1 を 10 回呼ぶ処理時間 (子プロセスの起動 + Add-Type の C# コンパイルを毎回
// 挟むため 1 回あたり 3 から 8 秒程度掛かりうる。合計で最大 80 秒を見込む) + 最後の
// screenshot-source + ログ回収の余裕を足した (見積もり。実測で見直す)。
// 送信側がここより先に自分で止まると、10 回目の PNG が「繋がりましたが切れました」を撮って
// しまい、確認したい「終了していないこと」と見分けが付かなくなり、実行が丸ごと
// 無駄になるため、余裕を厚めに取った。
const DOCK_VIEW_IDLE_HARNESS_SECONDS = 780;

// ---- ウィンドウのコンポーネントを押す手順 ------------------------------------------------
// ドックはタブを持たない 1 面で、設定とアンインストールは「ドックのメニュー -> cordmux の
// 設定 -> ページ」の順に開く。アンインストールの入口は OBS の [ツール] にもある (uninstallSteps の entry)。
// 押すのは dialog-click.ps1 (ウィンドウのタイトルか、メニューのウィンドウで探す。コンポーネントの種類と行き先を名指す)。
// ドックにタブがないので、タブを切り替える dock-tab.ps1 は使わない。
//
// コンポーネントを探す時限 (dialog-click.ps1 の -Timeout)。導入と撤去の往復の確認のウィンドウの待ち (ROUNDTRIP_DIALOG_TIMEOUT_MS
// の 15 秒) より短い 10 秒にする。メニューや設定のウィンドウは押した直後に開くため。
export const UI_FIND_TIMEOUT_MS = 10000;
// 設定のウィンドウの [OK] / [適用] の後、値がドックと配信の映像に届くまでの待ち (ドックの周期 500 ms の 4 周期 + 余裕。
// CALL_GATE_SHOT_SETTLE_MS と同じ 3 秒)。
export const UI_APPLY_SETTLE_MS = 3000;

// コンポーネントを探すウィンドウの名指し (dialog-click.ps1 の -Title か -Popup)。
const DOCK_WINDOW = { titleTextId: TEXT_ID_DOCK_WINDOW_TITLE };
const SETTINGS_WINDOW = { titleTextId: TEXT_ID_SETTINGS_WINDOW_TITLE };
const UNINSTALL_DIALOG = { titleTextId: TEXT_ID_DIALOG_UNINSTALL_TITLE };
const MENU_POPUP = { popup: true };

// ウィンドウのコンポーネントを押す 1 手。opts: controlType (Button / CheckBox / MenuItem / ListItem / ComboBox)、desired (on / off)、
// noWait (モーダルのウィンドウを開くコンポーネント。Invoke が閉じるまで戻らない)、timeoutMs、label (読んだ文を判定へ渡す名前)、gate。
// ComboBox のときは選ぶ項目を selectTextId (ini の id) か selectText (文字列そのまま) で渡す。
function uiClickStep(what, where, buttonTextId, opts) {
  const o = opts || {};
  return step("dialog-click", what, {
    target: "first",
    ...where,
    buttonTextId,
    ...(o.buttonText ? { buttonText: o.buttonText } : {}),
    controlType: o.controlType || "Button",
    ...(o.namePrefix ? { namePrefix: true } : {}),
    ...(o.selectTextId ? { selectTextId: o.selectTextId } : {}),
    ...(o.selectText ? { selectText: o.selectText } : {}),
    ...(o.group ? { buttonGroupTextId: o.group } : {}),
    ...(o.desired ? { desired: o.desired } : {}),
    ...(o.noWait ? { noWait: true } : {}),
    expect: "present",
    timeoutMs: o.timeoutMs ?? UI_FIND_TIMEOUT_MS,
    ...(o.label ? { label: o.label } : {}),
    ...(o.gate ? { gate: true } : {}),
  });
}

// ドックの [...] からメニューの項目を選ぶ (2 手)。メニューは QMenu::exec で開くので、[...] の Invoke はメニューが
// 閉じるまで戻らない (noWait)。項目のほうも、押した先がモーダルのウィンドウ (保存のウィンドウ) を開くことがあるので noWait。
function dockMenuSteps(itemTextId, what, opts) {
  const o = opts || {};
  return [
    uiClickStep("ドックの [" + TEXT_ID_DOCK_MENU_BUTTON + "] を押してメニューを開く (" + what + ")", DOCK_WINDOW, TEXT_ID_DOCK_MENU_BUTTON, { noWait: true, gate: o.gate }),
    uiClickStep("メニューの [" + itemTextId + "] を選ぶ (" + what + ")", MENU_POPUP, itemTextId, { controlType: "MenuItem", noWait: true, desired: o.desired, gate: o.gate }),
  ];
}

// cordmux の設定のウィンドウを開いて、ページを選ぶ (3 手)。設定のウィンドウはモーダルではないので、開いたまま
// ドックも押せる。
function settingsPageSteps(pageTextId, what, opts) {
  const o = opts || {};
  return [
    ...dockMenuSteps(TEXT_ID_MENU_SETTINGS, what, o),
    uiClickStep("cordmux の設定のウィンドウで [" + pageTextId + "] のページを選ぶ (" + what + ")", SETTINGS_WINDOW, pageTextId, { controlType: "ListItem", gate: o.gate }),
  ];
}

// ドックの [表示] タブを開く (1 手)。タブの切り替えはすぐ反映される。表示タブの入力欄は、このタブを開いてから押す。
function displayTabSteps(what, opts) {
  const o = opts || {};
  return [uiClickStep("ドックの [" + TEXT_ID_DOCK_TAB_DISPLAY + "] タブを選ぶ (" + what + ")", DOCK_WINDOW, TEXT_ID_DOCK_TAB_DISPLAY, { controlType: "TabItem", gate: o.gate })];
}

// ドックの [参加者] タブを開く (1 手)。参加者の行のチェックとミュートのボタンは、このタブにだけある (ほかのタブを開いたままだと、
// dock-click.ps1 は行のボタンを見つけられない)。行を押す手順の並びの前に置き、そのタブにいることを保証する。タブを選ぶだけで
// OBS と cordmux の状態を変えないので、配信中でも「その他の操作」に数えない (readOnlyOp。judgeOperationCount)。
function peopleTabStep(what) {
  return { ...uiClickStep("ドックの [" + TEXT_ID_TAB_PEOPLE + "] タブを選ぶ (" + what + ")", DOCK_WINDOW, TEXT_ID_TAB_PEOPLE, { controlType: "TabItem" }), readOnlyOp: true };
}

// リハーサル (テスト用の送信側) では確かめられないので判定しない項目と、その理由。リハーサルの実行計画は、判定の手順の代わりに
// 手順 skip (item と、行の一部だけなら part) を置き、judge-lines は item の行を「判定しない」にする (judgeLinesSummary)。
// 赤として数えると、リハーサルの結果を「想定の赤」と読み分けることになり、ほかの赤を見落とす。確かめられる物は、リハーサルでも判定する。
export const REHEARSAL_NOT_JUDGED = {
  "self-account": "テスト用の送信側は Vesktop のアカウントではなく、自分のアカウントを送らない (確かめるのは、製品の送信側が Vesktop のアカウントを読むこと)",
  "version-row": "テスト用の送信側は Vesktop の中で動かないので、Vesktop のバージョンを送らない",
  latency: "比べる相手の Vesktop のウィンドウがない",
  "encode-choice": "テスト用の送信側は、符号化の設定で確保し直す製品の送信側の動き (src-vencord/index.ts) を持たず、共有を始め直すこともできない",
  "self-frame-share": "テスト用の送信側は、メインアカウントの画面共有を始めることも止めることもできない (ドックのチェックの付け外しは判定する)",
  "broadcaster-left": "テスト用の送信側は、メインアカウントを通話から抜けさせることも戻すこともできない",
  "receive-continues": "テスト用の送信側には、スピーカーミュートも最小化もない (Vesktop の動き)",
  rejoin: "テスト用の送信側は、受け手の Vesktop として通話から抜けることも戻ることもできない",
  "align-drift": "リハーサルは短く、傾きを読む長さ (" + ALIGN_DRIFT_MIN_SPAN_S / 60 + " 分) 続くストリームがない (長く続いたストリームがあれば判定する)",
};
// リハーサルで判定の手順の代わりに置く 1 手 (理由と要件を結果に残す)。opts.part は行の一部 (まとめる読みの label) だけを判定しないとき、
// opts.reasonKey は REHEARSAL_NOT_JUDGED の別の鍵の理由を使うとき、opts.reason は理由の文そのもの。
function rehearsalSkipStep(item, label, opts) {
  const o = opts || {};
  return step("skip", label + " (リハーサルでは判定しない)", {
    item,
    ...(o.part ? { part: o.part } : {}),
    requirement: "実際の通話",
    reason: o.reason || REHEARSAL_NOT_JUDGED[o.reasonKey || item] || null,
  });
}
// テスト用の送信側に声だけの人がいないリハーサル (メインアカウントにできる人がいない) の理由。
const REHEARSAL_NO_MAIN_REASON = "テスト用の送信側に声だけの人がいない (--n-voice 0) ので、メインアカウントにできる人がいない";

// 設定のウィンドウの下のボタン (OBS 自身の文字列の [OK] / [適用])。[OK] は反映して閉じる。
function settingsButtonStep(obsTextId, what, opts) {
  return uiClickStep("cordmux の設定のウィンドウの [" + obsTextId + "] を押す (" + what + ")", SETTINGS_WINDOW, obsTextId, opts);
}

// 参加者の行のチェック (枠に載せる / 外す)。読み上げの名前は人の見えている名前 (person_row.cpp)。押すのは dock-click.ps1
// (ドックのウィンドウの CheckBox を名前で探し、行き先と違うときだけ Toggle する)。slotOp は操作数の集計 (judgeOperationCount) 用のマーカー。
// 座標で押す方式 (dock-click-at) は使わない。行の高さと列の位置が変わりうるが、名前で探せば座標が要らない。
function rowCheckStep(personName, desired, what, opts) {
  const o = opts || {};
  return step("dock-click", "参加者の行「" + personName + "」のチェックを" + (desired === "off" ? "外す" : "付ける") + " (" + what + ")", {
    target: "first",
    name: personName,
    index: 0,
    desired,
    slotOp: true,
    ...(o.gate ? { gate: true } : {}),
  });
}

// 参加者の行の声 / 画面共有の音のミュートのボタン (読み上げの名前は「声をミュート (人の名前)」「画面共有の音声をミュート (人の名前)」。
// 押した状態がミュート中なので、desired が on ならミュートにし、off なら外す)。配信中に押しても、操作数の「ミュート」に数える (muteOp)。
export const TEXT_ID_ROW_VOICE_MUTE = "cordmux.dock.row_voice_mute.a11y";
export const TEXT_ID_ROW_SCREEN_MUTE = "cordmux.dock.row_screen_mute.a11y";
// opts.absentOk: ボタンが出ていないことを失敗にしない (画面共有の音のボタンは、その人の共有に音がないときは出ない = 混ざる音がない。
// docs/spec.md と rules_dock.cpp の speaker_state_of)。ほかの人の画面共有の音をミュートにする手順だけが付ける。
function rowMuteStep(kind, person, desired, what, opts) {
  const target = kind === "voice" ? "声" : "画面共有の音";
  return step("dock-click", "参加者の行「" + person + "」の" + target + "のミュートを" + (desired === "on" ? "入れる" : "外す") + " (" + what + ")", {
    target: "first",
    textId: kind === "voice" ? TEXT_ID_ROW_VOICE_MUTE : TEXT_ID_ROW_SCREEN_MUTE,
    textArgs: [person],
    index: 0,
    desired,
    muteOp: true,
    ...(opts && opts.absentOk ? { absentOk: true } : {}),
  });
}

// ini の文字列の %1 %2 ... を args で置き換える (ドックの行のボタンの名前を人の名前で組む)。
export function fillTextArgs(text, args) {
  const list = args || [];
  return String(text).replace(/%(\d+)/g, (m, n) => (Number(n) >= 1 && Number(n) <= list.length ? String(list[Number(n) - 1]) : m));
}

// 声を持つ人の見えている名前 (通話の中で自分以外。受信側のアカウントは本人の声の行がないので入れない)。実際の通話の実行は Chrome と Firefox のアカウント、リハーサルはテスト用の送信側の声のストリーム
// (画面のストリームの後の番号。voiceSourcePeople と同じ)。自分 (メインのアカウント) の声のミュートは既定でミュートなので触らない。
export function callVoicePersons(args) {
  if (!args || !args.rehearsal) {
    const acc = callAccounts(args);
    return [acc.chrome, acc.firefox];
  }
  const out = [];
  for (let v = 1; v <= callVoices(args); v += 1) out.push(harnessPersonName("voice", callScreens(args) + v));
  return out;
}

// 画面共有をしている人の見えている名前 (参加の文で共有を頼む人)。実際の通話はメインアカウント・Chrome・Firefox のアカウント
// (--vesktop-share でも、Vesktop のアカウントの共有は枠に載らないので入れない。docs/spec.md の「枠と配置」)、リハーサルはテスト用の送信側の画面の人。
export function callScreenSharers(args) {
  if (args && args.rehearsal) {
    const out = [];
    for (let s = 1; s <= callScreens(args); s += 1) out.push(harnessPersonName("screen", s));
    return out;
  }
  const acc = callAccounts(args);
  return [acc.main, acc.chrome, acc.firefox];
}

// 別アカウント方式でメインアカウントにする人 (メニューから選ぶ人と、自分の枠のチェックの行)。実際の通話はメインアカウント。
// リハーサルは、テスト用の送信側の声だけの人の 1 人目 (画面共有していない人。実際の通話の、自分の枠を確かめ始めるときの
// メインアカウントと同じく、共有していない状態から始まる)。声だけの人がいなければ null (別アカウント方式の手順を判定しない)。
export function mainAccountFor(args) {
  if (!args || !args.rehearsal) return callAccounts(args).main;
  return callVoices(args) >= 1 ? harnessPersonName("voice", callScreens(args) + 1) : null;
}

// 実際のアバターの並び (rosterCheckSteps) に並ぶはずの人数。docs/spec.md の「名前ラベルと話している人の光」の決まりから出す:
// 通話に参加している人のうち、サブアカウント (別アカウント方式の Vesktop のアカウント) を除いた全員が、枠に名前ラベル付きで
// 描かれていなければ並びに出る。並びを撮るのは全員が共有を止め、自分の枠のチェックも外した後なので、通話にいる 4 つの
// アカウント (CALL_ACCOUNT_ROLES) から、別アカウント方式ならサブアカウント 1 つを引いた数 (メインアカウントも並ぶ)。
// --broadcaster-id がない (メインアカウントを選ばない) と、Vesktop のアカウントはサブアカウントとして外れず、4 人が並ぶ。
export function rosterExpectedPeople(args) {
  const inCall = CALL_ACCOUNT_ROLES.length;
  const subAccount = args && args.broadcasterId !== null && args.broadcasterId !== undefined ? 1 : 0;
  return inCall - subAccount;
}

// 録画のミックスに入れたくない声を、ドックの行のミュートで切る (keep の人の声だけ残す。keep が null なら全員切る)。
// 戻すときは voiceRestoreSteps。画面共有の音は共有が始まるたびにミュートから始まるので、ここでは触らない。
function voiceIsolateSteps(people, keep, what) {
  return people.map((p) => rowMuteStep("voice", p, p === keep ? "off" : "on", what));
}
function voiceRestoreSteps(people, what) {
  return people.map((p) => rowMuteStep("voice", p, "off", what));
}

function dockViewScreenshotSteps(opts) {
  // opts.selectFirst: 1 人目の行のチェックを付けて 5 秒待ってから撮る。誰を載せるかは手動が既定なので、
  // 付けないと画質や解像度の欄も枠の映像も出ない。
  const selectFirst = Boolean(opts && opts.selectFirst);
  const steps = [step("screenshot-window", "OBS の見えているトップレベルウィンドウを全部 PNG に撮る (ドックが浮いていれば別のウィンドウも含む)", { target: "first", shotLabel: "dock" })];
  if (selectFirst) {
    steps.push(rowCheckStep(harnessPersonName("screen", 1), "on", "手動の既定なので付けないと枠に入らない"));
    steps.push(step("wait", "枠に入って画質と解像度の欄が埋まるまでの猶予", { ms: SELECT_FOUR_SETTLE_WAIT_MS }));
    steps.push(step("screenshot-window", "枠に載せた後のドックを PNG に撮る", { target: "first", shotLabel: "dock-selected" }));
  }
  // 合成のソースを置いたときだけ、そのソースの映像を撮る (置いていないときは撮る対象がない)。
  if (opts && opts.composite) steps.push(step("screenshot-source", "合成のソースの絵を PNG に撮る (obs-websocket GetSourceScreenshot)", { source: { kind: "composite" } }));
  return steps;
}

// 警告が出ている間も出口の映像に差分がないことの再確認: 10 秒録画し、
// frames.py の欠落を見る。呼び出し側 (connected/degraded) が撮影の前に既に wire-composite を
// 済ませている前提 (下の buildDockViewSteps 参照。no-scene との対比のため、撮る前に
// 配線する)。degraded 側の judge と、connected 側 (警告なしの基準) の judge を label で
// 見分けられるようにし、両方の結果 JSON を人が並べて同じ判定 (欠落 0) になることを見る
// (この 2 ファイルは Node の API を呼ばないため、別プロセスとして出る 2 つの結果 JSON を
// 突き合わせる仕組みそのものは持たない。summarize-window.py の出力を並べて読む)。
function dockViewAc24RecordingSteps(label) {
  return [
    step("record-start", "録画を開始する (警告の有無で出口の映像に差分がないかを見る)", { stream: false }),
    step("wait", "差分の再確認の録画区間 (" + DOCK_VIEW_AC24_RECORD_SECONDS + " 秒)", { ms: DOCK_VIEW_AC24_RECORD_SECONDS * 1000 }),
    step("record-stop", "録画を終了する", { stream: false, tailSeconds: 0 }),
    step("collect-recording", "録画ファイルの回収"),
    step("judge", "frames.py で欠落を見る (" + label + ")", { tool: "frames.py", label }),
  ];
}

// scenario "no-scene" / "contact-missing" で共通の形 (繋ぐだけで Main へは配線しない):
// harness-start -> wait-connection -> 10 秒待つ -> 撮る -> ログ回収 -> harness-wait。
// harnessExtra は harness-start の extra 欄 (contact-missing の failContact など。
// 無ければ渡さない = harnessExtra がない step と同じ形にする)。
function dockViewConnectOnlySteps(args, harnessLabel, harnessExtra) {
  return [
    step("harness-start", harnessLabel, {
      mode: "product",
      n: 1,
      seconds: DOCK_VIEW_CONNECT_ONLY_HARNESS_SECONDS,
      hw: args.hw,
      warmup: args.warmup,
      syncTone: DEFAULT_SYNC_TONE,
      ...(harnessExtra ? { extra: harnessExtra } : {}),
    }),
    step("wait-connection", "送り手が接続を確立するまで待つ", { timeoutMs: DOCK_VIEW_CONNECT_WAIT_TIMEOUT_MS }),
    step("wait", "繋がってから撮るまでの猶予 (10 秒後に撮る)", { ms: DOCK_VIEW_POST_CONNECT_WAIT_MS }),
    ...dockViewScreenshotSteps(),
    step("collect-plugin-log", "プラグインの記録の回収"),
    step("harness-wait", "送り手の終了を待つ", { seconds: DOCK_VIEW_CONNECT_ONLY_HARNESS_SECONDS }),
  ];
}

// scenario "select-four" (合成のソースの 4 等分の確認)。
//
// 「ドックで人を選べること」と「4 分割が出ること」を、1 つの流れで撮る。
//
// 誰を載せるかの決め方は手動が既定である。
// 画面 4 ストリームを harness で繋いでも、枠へは 1 人も入らない (StreamRouter::apply_configure が
// 手動のときに枠へ入れるのは、利用者が入れた人と「いつも載せる人」だけ)。そこで:
//
//   (a) 繋いだ直後に 1 枚撮る (枠が空のまま = 手動の既定そのものの証拠)。
//   (b) 参加者の行のチェックを 4 人分、人の見えている名前 (harnessPersonName) で付ける
//       (rowCheckStep。行き先 on なので、既に付いている行は押し直さない)。
//   (c) 5 秒待って撮る。合成のソースが 4 人ぶんの画面共有を 4 等分に敷いたことを、OBS のログの配置の行から
//       judge-composite-placement が判定する。
//
// これで「obs-websocket を使わず、ドックの操作だけで載せる人を選べて 4 分割が出る」ことが
// 1 つの実行計画で確認できる。押す前と後の PNG の差そのものが、人を選べることの証拠になる
// (前は空、後は 4 分割)。
//
// 未確認の点: 行のチェック (QCheckBox を継ぐ RowCheck) を dock-click.ps1 の
// UI Automation が名前で見つけて押せるかは、実機で確認していない。表の升目の中のボタンは辿れないが、行は表ではない。Qt は QCheckBox に InvokePattern も付けるので、
// dock-click.ps1 -Desired は状態を TogglePattern で先に読み、違うときだけ Invoke で押す (結果の reachedDesired)。
// 見つからなければ ok:false になり、枠が空のままなので judge-composite-placement も失敗する (黙って合格にならない)。
function selectFourSteps(args) {
  const nScreen = SELECT_FOUR_N_SCREEN;
  const nVoice = SELECT_FOUR_N_VOICE;
  const checks = [];
  for (let i = 1; i <= nScreen; i += 1) checks.push(rowCheckStep(harnessPersonName("screen", i), "on", "4 人を枠に載せる " + i + "/" + nScreen));
  return [
    step(
      "harness-start",
      "テスト用の送信側を product モードで画面 " +
        nScreen +
        " 本 + 声 " +
        nVoice +
        " 本で繋ぐ (手動が既定なので、画面はまだ 1 つも枠に入らない。声は上限まで入る)",
      { mode: "product", nScreen, nVoice, seconds: SELECT_FOUR_HARNESS_SECONDS, hw: args.hw, warmup: args.warmup, syncTone: DEFAULT_SYNC_TONE },
    ),
    step("wait-connection", "送り手が接続を確立するまで待つ", { timeoutMs: SELECT_FOUR_CONNECT_WAIT_TIMEOUT_MS }),
    wireCompositeStep("枠に載った人を 4 等分で描く合成のソース"),
    step("wait", "参加者一覧に 4 人が並ぶまでの猶予 (5 秒)", { ms: SELECT_FOUR_SETTLE_WAIT_MS }),
    step("screenshot-window", "押す前の OBS のウィンドウを PNG に撮る (手動の既定: 繋がっていても枠は空)", { target: "first", shotLabel: "before-select" }),
    ...checks,
    step("wait", "合成のソースが配置を敷くまでの猶予 (5 秒後に撮る)", { ms: SELECT_FOUR_SETTLE_WAIT_MS }),
    step("screenshot-window", "OBS のウィンドウを PNG に撮る (4 人を載せた後)", { target: "first", shotLabel: "after-select" }),
    step("screenshot-source", "合成のソースの絵を PNG に撮る (obs-websocket GetSourceScreenshot)", { source: { kind: "composite" } }),
    compositeSnapshotStep("select-four", "4 人を載せた後"),
    step("judge-composite-placement", "読んだ配置の行が 4 等分の決まった位置 (画面共有 4 つ) になっているかを judgeCompositePlacement で判定する", {
      label: "select-four",
      count: nScreen,
    }),
    step("collect-plugin-log", "プラグインの記録の回収"),
    step("harness-wait", "送り手の終了を待つ", { seconds: SELECT_FOUR_HARNESS_SECONDS }),
  ];
}

// scenario "dummy" (ダミー表示の確認)。繋がずにドックの [表示] タブを開き、
// 「ダミー映像を表示」を入れて、画面共有の枠 1 に色タイルが出ることを見る。
// ダミーの地の上で測るため、撮った枠 1 の PNG の名前ラベルの板を plate_measure.py で
// 読む (measure-plate。地 0x7A2E2E の上で、値の上に重ねるなら R 約 31、光の量の上に重ねるなら約 62)。--obs-hdr の実行は、HDR の出力で
// GetSourceScreenshot の PNG がどの形式になるか (PQ か、SDR に修正したもの) が分からないので、出力の録画を 5 秒録り、その
// フレームを PQ として読む (measure-plate の transfer pq)。録画は後始末で消えるので、読むのは手順の中で行う。
export const DUMMY_PLATE_BG = "7A2E2E";
export const DUMMY_HDR_RECORDING_LABEL = "t16-hdr";
export const DUMMY_HDR_RECORD_MS = 5000;
function dummyScenarioSteps(args) {
  const hdr = Boolean(args && args.obsLook && args.obsLook.hdr);
  const hdrSteps = hdr
    ? [
        step("record-start", "HDR: 出力を録る (PQ のフレームで板を読むため)", { stream: false, label: DUMMY_HDR_RECORDING_LABEL }),
        step("wait", "HDR: 録る区間", { ms: DUMMY_HDR_RECORD_MS }),
        step("record-stop", "HDR: 録画を終了する", { stream: false, tailSeconds: 0, label: DUMMY_HDR_RECORDING_LABEL }),
        step("collect-recording", "HDR: 録画ファイルの回収", { label: DUMMY_HDR_RECORDING_LABEL }),
        step("measure-plate", "HDR: 録画の真ん中のフレームの名前ラベルの板を plate_measure.py で読む (transfer pq)", {
          recording: DUMMY_HDR_RECORDING_LABEL,
          atS: DUMMY_HDR_RECORD_MS / 2000,
          transfer: "pq",
          bg: DUMMY_PLATE_BG,
          corner: "bl",
        }),
      ]
    : [];
  return [
    // 撮る前に、合成のソースを録画するシーン (Main) へ置く。libobs がソースの映像を GPU のテクスチャへ描くのは、そのソースを
    // 実際に描いている間だけである。出ているシーンに置かないと、OBS は 1 度も描かず、GetSourceScreenshot は空 (透明) の PNG を返す。
    // connected/degraded が撮る前に wire-composite を呼んでいるのと同じ理由である。
    wireCompositeStep("置かないと OBS が合成のソースを 1 度も描かず、ダミーのフレームがテクスチャへ載らない"),
    // ダミーの入り切りはドックの [表示] タブにあり、押すとすぐ動作する。
    ...displayTabSteps("ダミー"),
    uiClickStep("[" + TEXT_ID_LOOK_SHOW_PLACEHOLDERS + "] を入れる (既に入っていれば押さない)", DOCK_WINDOW, TEXT_ID_LOOK_SHOW_PLACEHOLDERS, { controlType: "CheckBox", desired: "on" }),
    step("screenshot-window", "ダミーを入れた直後のドックの [表示] タブを PNG に撮る", { target: "first", shotLabel: "dock-display" }),
    step("wait", "ダミーの色タイルが乗るまでの猶予 (5 秒後に撮る)", { ms: DUMMY_SETTLE_WAIT_MS }),
    step("screenshot-source", "合成のソースの絵を PNG に撮る (色タイルが出ているかを判定する材料)", { source: { kind: "composite" } }),
    step("screenshot-window", "OBS のウィンドウを PNG に撮る (ダミーを入れた後)", { target: "first", shotLabel: "dummy-on" }),
    step("judge-dummy-tile", "撮った合成のソースの PNG が黒でない (色タイルが出ている) かを dummy_tile.py で判定する", {}),
    step("measure-plate", "撮った合成のソースの PNG の名前ラベルの板を plate_measure.py で読む (地 " + DUMMY_PLATE_BG + "。判定はしない)", {
      transfer: "srgb",
      bg: DUMMY_PLATE_BG,
      corner: "bl",
    }),
    ...hdrSteps,
    step("collect-plugin-log", "プラグインの記録の回収"),
  ];
}

// scenario "export-bundle" (診断情報の zip の確認)。繋がずにドックのメニューの
// [診断情報をエクスポート...] を選び、開いた保存のウィンドウへファイル名を入れて確定する。
function exportBundleSteps() {
  return [
    // メニューの項目を押すと、診断情報を集めてから保存のウィンドウ (QFileDialog::getSaveFileName。モーダル) を開く。
    ...dockMenuSteps(TEXT_ID_MENU_EXPORT_DIAGNOSTICS, "診断情報のエクスポート"),
    step("dialog-sendkeys", "保存のウィンドウ ([" + TEXT_ID_DIALOG_EXPORT_TITLE + "]) にファイル名を入れて確定する (dialog-sendkeys.ps1)", {
      target: "first",
      titleTextId: TEXT_ID_DIALOG_EXPORT_TITLE,
      fileNameRel: EXPORT_BUNDLE_FILE_NAME,
      timeoutMs: EXPORT_BUNDLE_DIALOG_TIMEOUT_MS,
    }),
    step("wait", "zip が書き終わるまでの猶予 (5 秒後に確認する)", { ms: EXPORT_BUNDLE_SETTLE_WAIT_MS }),
    step("check-bundle-zip", "zip があり、中身が揃っていて合言葉がないかを check_bundle_zip.py で判定する", {}),
    step("collect-plugin-log", "プラグインの記録の回収"),
  ];
}

function buildDockViewSteps(args) {
  const scenario = DOCK_SCENARIO_VALUES.includes(args.dockScenario) ? args.dockScenario : DEFAULT_DOCK_SCENARIO;
  const pre = [...seedAndPlaceSteps(args), ...launchAndConnectSteps(args)];
  const post = [...closeSteps("first"), ...finishSteps(args, "dock-view")];

  if (scenario === "select-four") {
    return [...pre, ...selectFourSteps(args), ...post];
  }

  if (scenario === "dummy") {
    return [...pre, ...dummyScenarioSteps(args), ...post];
  }

  if (scenario === "export-bundle") {
    return [...pre, ...exportBundleSteps(), ...post];
  }

  if (scenario === "no-scene") {
    // 利用者の録画/配信用シーン (Main) へまだ合成のソース cordmux を置いていない状態を撮る (ドックが「合成のソースがない」を
    // どう表示するかを見るため)。connected とは wire-composite を呼ぶかどうかだけで分け、対比が実際の相違点 (OBS にソースが有るかないか)
    // になるようにする。合成のソースがないので、ソースの絵は撮らない。
    return [...pre, ...dockViewConnectOnlySteps(args, "テスト用の送信側を product モードで 1 本繋ぐ (Main へは配線しない)"), ...post];
  }

  if (scenario === "connected" || scenario === "degraded") {
    const degraded = scenario === "degraded";
    // failStage=3 は degraded 自体の定義である。
    // 加えて degraded は maxLongEdge を渡す (既定 DEFAULT_DOCK_MAX_LONG_EDGE
    // = 640、--dock-max-long-edge で変えられる)。connected は両方とも渡さない
    // (extra 自体を持たない = failStage なしの挙動のまま)。
    const harnessExtra = degraded ? { failStage: 3, maxLongEdge: args.dockMaxLongEdge } : undefined;
    const harnessLabel = degraded
      ? "テスト用の送信側を product モードで 1 本繋ぐ (failStage=3 で降格させ、HARNESS_MAX_LONG_EDGE=" + args.dockMaxLongEdge + " で長辺を縮める)"
      : "テスト用の送信側を product モードで 1 本繋ぐ";
    const judgeLabel = degraded ? "ac24_degraded" : "ac24_connected_baseline";
    // 参加者一覧の段の文言・解像度の欄・結果 JSON が一致することを見るため、
    // degraded だけ、送信側の終了を待った直後に直近の configure の寸法と facts.stages を
    // 結果 JSON へコピーする (run-window.mjs の harness-wait ケースが copyEncodeFacts を見て
    // record("encode-facts", ...) を足す。encodeFactsFromHarnessData 参照)。
    const harnessWaitLabel = degraded
      ? "送り手の終了を待つ (直近の configure の寸法と facts.stages を結果 JSON へコピーする)"
      : "送り手の終了を待つ";
    return [
      ...pre,
      step("harness-start", harnessLabel, {
        mode: "product",
        n: 1,
        seconds: DOCK_VIEW_CONNECTED_HARNESS_SECONDS,
        hw: args.hw,
        warmup: args.warmup,
        syncTone: DEFAULT_SYNC_TONE,
        ...(harnessExtra ? { extra: harnessExtra } : {}),
      }),
      step("wait-connection", "送り手が接続を確立するまで待つ", { timeoutMs: DOCK_VIEW_CONNECT_WAIT_TIMEOUT_MS }),
      // no-scene との対比のため、撮る前に Main へ合成のソースを置く (差分の再確認の録画も同じ配置を使う。wire-composite 自体は
      // 一瞬で終わるので、「10 秒後に撮る」という秒数の意味は変わらない)。
      wireCompositeStep("no-scene との対比 + 差分の再確認用"),
      step("wait", "繋がってから撮るまでの猶予 (10 秒後に撮る)", { ms: DOCK_VIEW_POST_CONNECT_WAIT_MS }),
      ...dockViewScreenshotSteps({ selectFirst: true, composite: true }),
      ...dockViewAc24RecordingSteps(judgeLabel),
      step("collect-plugin-log", "プラグインの記録の回収"),
      step("harness-wait", harnessWaitLabel, {
        seconds: DOCK_VIEW_CONNECTED_HARNESS_SECONDS,
        ...(degraded ? { copyEncodeFacts: true } : {}),
      }),
      ...post,
    ];
  }

  if (scenario === "contact-missing") {
    const label = "テスト用の送信側を product モードで 1 本繋ぐ (HARNESS_FAIL_CONTACT=" + DOCK_VIEW_FAIL_CONTACT_ID + " でその接点を Missing として facts に載せる)";
    return [...pre, ...dockViewConnectOnlySteps(args, label, { failContact: DOCK_VIEW_FAIL_CONTACT_ID }), ...post];
  }

  if (scenario === "idle-10min") {
    const loop = [];
    for (let i = 1; i <= DOCK_VIEW_IDLE_INTERVALS; i += 1) {
      loop.push(step("wait", "1 分待つ (" + i + "/" + DOCK_VIEW_IDLE_INTERVALS + ")", { ms: DOCK_VIEW_IDLE_INTERVAL_MS }));
      loop.push(step("screenshot-window", "OBS の見えているトップレベルウィンドウを撮る (" + i + "/" + DOCK_VIEW_IDLE_INTERVALS + "分。裏のコンポーネントが終了していないかを最後の 1 枚で見る)", { target: "first" }));
    }
    return [
      ...pre,
      step("harness-start", "テスト用の送信側を product モードで 1 本、10 分より長く繋いだままにする", { mode: "product", n: 1, seconds: DOCK_VIEW_IDLE_HARNESS_SECONDS, hw: args.hw, warmup: args.warmup, syncTone: DEFAULT_SYNC_TONE }),
      step("wait-connection", "送り手が接続を確立するまで待つ", { timeoutMs: DOCK_VIEW_CONNECT_WAIT_TIMEOUT_MS }),
      ...loop,
      step("collect-plugin-log", "プラグインの記録の回収 (裏のコンポーネントが終了していないかを受信側のログでも見る)"),
      step("harness-wait", "送り手の終了を待つ", { seconds: DOCK_VIEW_IDLE_HARNESS_SECONDS }),
      ...post,
    ];
  }

  // scenario "none" (既定。送信側なし)。--dock-scenario を指定しない呼び出しもここに来る。
  return [...pre, step("wait", "OBS の起動とドックの描画が落ち着くまで待つ (送り手なし)", { ms: DOCK_VIEW_SETTLE_WAIT_MS }), ...dockViewScreenshotSteps(), ...post];
}

// ---- 通話の開始ゲート (--step call-gate-a / -b / -c / -d) ----------------------
//
// 実行スクリプトは人の操作を待てないので、人の操作の前後で step を割り、
// 操作の完了は人が通知して次の step を起動する。ゲートは自分の終了コードで失敗を返し (gate:true の
// 手順が 1 つでも ok:false なら run-window.mjs が 3 を返す)、本体 (call-1) は人が合格を見てから
// 起動する (1 つの step の中で分岐しない)。
//
// 7 つの手順との対応:
//   (1) 実際の state.json のコピーを結果フォルダーへ          -> call-gate-a の copy-state-json
//   (2) 利用者が Vesktop を終了する                         -> call-gate-a の人の操作 (追加の依頼と 1 つ)
//   (3) 利用者が [Vesktop に追加] を押す                     -> call-gate-a の人の操作
//   (4) 直後に state.json を読み、他のキーが変わっていないか -> call-gate-a の judge-vencord-dir
//   (5) (Vesktop は終了したまま)                            -> step の境目
//   (6) state.json を読み直す                               -> call-gate-b
//   (7) 利用者が起動し、送信部が繋いでくるか                -> call-gate-c
// 始めは (2) を「Vesktop を起動したまま」にして、Vesktop が終了するときに vencordDir を書き直すかを (6) で見ていた。Vesktop (v1.6.9 の
// src/main/settings.ts と mainWindow.ts) は、起動したときに読んだ値をメモリに持ち、ウィンドウを動かすなどで値が変わるたびに
// メモリの値の全体で state.json を書き直すので、起動中に書いた値は消えうる。製品は Vesktop の起動中に [Vesktopに追加] を押せなく
// したので (ledger/actions.cpp の vesktop_busy_reason)、(2) は終了に替えた。(6) は、Vesktop を起動していない間に値が変わらないことを
// 見る確認として残す。
// package.json を退避して 1 回だけ見る確認は call-gate-d。

function vesktopStateJsonStep(kind, label, extra) {
  return step(kind, label, Object.assign({ appDataRel: VESKTOP_STATE_JSON_REAL_REL }, extra || {}));
}

function bundledVencordDir(args) {
  return args.obs + "/" + BUNDLED_VENCORD_REL;
}

function vencordDirBackupPath(args) {
  return args.obs + "/" + VENCORD_DIR_BACKUP_REL;
}

// 人の操作待ち。message は操作する人に見せる平易な文。until と timeoutMs は HUMAN_* の欄を参照。
// waitSeconds は人の操作の見込み (plannedMinutes が入れる)。until が seconds のときだけ実際にその秒数を待つ。
// state.json を見る until は、実際の state.json の場所 (appDataRel) と比べる先 (expectedDir / backupPath) を持つ。
function humanStep(message, until, timeoutMs, waitSeconds, extra) {
  return step("human", "人の操作を待つ: " + message, {
    message,
    until,
    timeoutMs,
    waitSeconds,
    appDataRel: VESKTOP_STATE_JSON_REAL_REL,
    ...(extra || {}),
  });
}

function buildCallGateASteps(args) {
  return [
    ...seedAndPlaceSteps(args),
    ...launchAndConnectSteps(args),
    vesktopStateJsonStep("copy-state-json", "実際の state.json のコピーを結果フォルダへ取る (証拠。読むだけで、書くのは製品の [" + TEXT_ID_ADD_TO_VESKTOP + "] を押した結果)", {
      destName: STATE_JSON_BEFORE_NAME,
    }),
    // 押す前にメッセージが描かれるのを待つ。メッセージは os_probe が state.json を読んで
    // 「別の場所を指している」と判定してからドックの周期 (settings.h の dock_poll_ms
    // 既定 200 ms) で描かれる。--step dock-view が同じ場面に置いている
    // 5 秒 (DOCK_VIEW_SETTLE_WAIT_MS と同じ値) をここにも置く。
    step("wait", "ドックがメッセージを描くまでの猶予 (押す前)", { ms: SELECT_FOUR_SETTLE_WAIT_MS }),
    // [Vesktop に追加] は利用者が押す (Vesktop の設定を書き換えるボタン。実行スクリプトは押さずに、実際の state.json の
    // vencordDir がテスト用にインストールしたファイルを指すまで待つ = judge-vencord-dir と同じ判定を until に使う)。時限 10 分で失敗にする。
    // state.json が別の Vencord を指していれば、押した後に切り替えの確認のウィンドウが出る。
    // 文は Vesktop の終了 (起動中は押せない) と、確認のウィンドウのボタンも挙げる (vesktopQuitAndAddMessage)。
    humanStep(
      vesktopQuitAndAddMessage(),
      "state-json-points-to-placed",
      HUMAN_TIMEOUT_MS,
      HUMAN_ALLOWANCE_SHORT_S,
      {
      expectedDir: bundledVencordDir(args),
      gate: true,
    }),
    step("wait", "state.json が書き終わるまでの猶予", { ms: CALL_GATE_WRITE_SETTLE_MS }),
    vesktopStateJsonStep("read-state-json", "押した直後の state.json を読む", { label: "after-write" }),
    step("judge-vencord-dir", "vencordDir がテスト用にインストールしたファイルの data/vencord を指し、他のキーが 1 文字も変わっていないかを見る", {
      expectedDir: bundledVencordDir(args),
      compareOtherKeys: true,
      gate: true,
    }),
    step("check-file-exists", "製品がバックアップを書いたか (" + VENCORD_DIR_BACKUP_REL + ")", { path: args.obs + "/" + VENCORD_DIR_BACKUP_REL }),
    step("screenshot-window", "OBS のメインウィンドウとドックを PNG に撮る", { target: "first", shotLabel: "dock" }),
    step("collect-plugin-log", "プラグインの記録の回収"),
    ...closeSteps("first"),
    ...finishSteps(args, "call-gate-a"),
  ];
}
// call-gate-b は **OBS を起動しない** (mic-check と同じ。読むのは Vesktop を閉じた後の state.json
// だけで、OBS もドックも要らない)。判定の基準になる「書き換える前のコピー」は call-gate-a が
// 同じ --out に置いた STATE_JSON_BEFORE_NAME を読む。
//
// **他の鍵の比較はしない** (compareOtherKeys: false): Vesktop は終了時にウィンドウの大きさなどを
// 書き直すので、ここで他の鍵の変化を失敗にすると正常な終了まで失敗になる。見たいのは
// 「終了時に vencordDir が上書きされないか」だけである。
function buildCallGateBSteps(args) {
  return [
    vesktopStateJsonStep("read-state-json", "Vesktop を閉じた後の state.json を読み直す (終了時に上書きされないか)", { label: "after-close" }),
    step("judge-vencord-dir", "vencordDir がテスト用にインストールした data/vencord を指したままかを見る", {
      expectedDir: bundledVencordDir(args),
      compareOtherKeys: false,
      gate: true,
    }),
    ...finishSteps(args, "call-gate-b"),
  ];
}

// call-gate-c / call-gate-d は seed-config もインストールもやり直さない: seed-config.py は
// data/vencord を消して作り直すので、**Vesktop が指している先を実行の途中で作り直す**ことに
// なる。置いたものは call-gate-a のものをそのまま使う。
function buildCallGateCSteps(args) {
  return [
    ...launchAndConnectSteps(args),
    step("wait-connection", "開き直した Vesktop の送信部が繋いでくるのを待つ", {
      timeoutMs: CALL_GATE_CONNECT_WAIT_TIMEOUT_MS,
      gate: true,
    }),
    step("wait", "ドックの 1 行目と参加者の一覧が引き直されるまでの猶予", { ms: CALL_GATE_SHOT_SETTLE_MS }),
    step("screenshot-window", "ドック (1 行目と参加者の一覧) を PNG に撮る", { target: "first", shotLabel: "dock" }),
    step("collect-plugin-log", "プラグインの記録の回収"),
    ...closeSteps("first"),
    ...finishSteps(args, "call-gate-c"),
  ];
}

// call-gate-d。**繋いでくるかどうかは答えであって失敗ではない**ので、
// wait-connection は gate にしない (4 ファイルだけでは有効と見なされない、という答えでも
// この step は成功で終わる)。
//
// 失敗は 2 つあり、**どちらも自分の手順で返す**: (1) 移動できなかった (move-file。dist に
// package.json がないバージョンを使ったときなど。4 ファイルだけの確認がそもそも始まっていない)、(2) 戻せ
// なかった (restore-file。置いたものが残る)。move-file を gate にしないと、(1) のときに
// restore-file が「退かした記録がない」で失敗を返すため、終了コードからは (1) と (2) の
// 区別が付かない。
function buildCallGateDSteps(args) {
  const packageJson = args.obs + "/" + BUNDLED_VENCORD_PACKAGE_JSON_REL;
  return [
    step("move-file", "テスト用にインストールした data/vencord/package.json を結果フォルダーへ移動する (4 ファイルだけの形で「有効」と見なされるか)", {
      path: packageJson,
      destName: "package.json.moved",
      restoreId: "vencord-package-json",
      gate: true,
    }),
    ...launchAndConnectSteps(args),
    step("wait-connection", "開き直した Vesktop の送信部が繋いでくるかを待つ (繋がなければ「package.json も要る」が答え)", {
      timeoutMs: CALL_GATE_CONNECT_WAIT_TIMEOUT_MS,
    }),
    step("wait", "ドックの 1 行目が引き直されるまでの猶予", { ms: CALL_GATE_SHOT_SETTLE_MS }),
    step("screenshot-window", "ドック (1 行目) を PNG に撮る", { target: "first", shotLabel: "dock" }),
    step("restore-file", "退かした package.json をすぐ戻す", { restoreId: "vencord-package-json", gate: true }),
    step("collect-plugin-log", "プラグインの記録の回収"),
    ...closeSteps("first"),
    ...finishSteps(args, "call-gate-d"),
  ];
}

// ---- call-1 の本体 (--step call-1) ------------------------------------------------
//
// **[Vesktop に追加] と [cordmux のシーンを作成] は
// record-start より前**に置く (配信中の操作として数えるのは、配信を始めた後の操作だけである)。
// 配信を止めるのは、参加者の行のチェックの付け外しと声のミュートの往復が終わってから (この 2 つは配信中の操作として数える側)。
// シーンコレクションの往復は配信を止めた後に置く: OBS の UI は出力中の切り替えを
// 禁じており、obs-websocket 経由で同じ制限が掛かるかは未実測である (--step second-obs の
// と同じ扱い)。

// 参加者の行のチェックで 1 人を外して戻す + 前後の配置の行と PNG。
// 行は人の見えている名前で名指す (行の並びに頼らない)。合成のソース cordmux は、1 つのソースで自分の枠と参加者ごとの画面共有を 4 等分の
// 決まった位置に描くので、判定は OBS の記録の最後の配置の行 (Composite layout ...) を前後で読んで、外した人のタイルが消えて後ろが
// 詰まり、戻すと元の並びと位置に戻ることを当てる (judge-composite-layout)。枠の番号と人の対応は記録の「Screen slot N carries ...」
// の行から引く (テスト用の送信側は stream の番号から名前が分かる。実際の通話では名前が行にないので、人は不明のままストリームの番号の並びで見る)。
export const COMPOSITE_INPUT_NAME = "cordmux";

// 配置の行を読む手順 (OBS の記録の最後の配置の行と、枠の番号 -> 人の対応)。label が結果の鍵。
function compositeSnapshotStep(label, note, extra) {
  return step("composite-layout-snapshot", "合成のソース: " + note + "の配置の行 (OBS の記録の最後の Composite layout の行) と枠の番号 -> 人の対応を読む", { label, ...(extra || {}) });
}

function callAc30Steps(person, namesUnknown) {
  // 実際の通話はストリームの番号が人と結び付かないので、人の名前は引かず、番号の並びで見る。
  const snapshotExtra = namesUnknown ? { namesUnknown: true } : undefined;
  const shot = (shotLabel, note) => step("screenshot-window", "参加者の行のチェック: " + note + " を PNG に撮る", { target: "first", shotLabel });
  return [
    compositeSnapshotStep(AC30_ITEM_LABELS.before, "付け外しの前", snapshotExtra),
    peopleTabStep("参加者の行のチェック: 行を押す前"),
    shot("ac30-before", "付け外しの前"),
    rowCheckStep(person, "off", "参加者の行のチェック: 枠から外す"),
    step("wait", "枠から外れて合成のソースが配置を敷き直すまでの猶予", { ms: SELECT_FOUR_SETTLE_WAIT_MS }),
    compositeSnapshotStep(AC30_ITEM_LABELS.afterRemove, "外した後", snapshotExtra),
    shot("ac30-after-remove", "外した後"),
    rowCheckStep(person, "on", "参加者の行のチェック: 外した人を戻す"),
    step("wait", "枠へ戻って合成のソースが配置を敷き直すまでの猶予", { ms: SELECT_FOUR_SETTLE_WAIT_MS }),
    compositeSnapshotStep(AC30_ITEM_LABELS.afterRestore, "戻した後", snapshotExtra),
    shot("ac30-after-restore", "戻した後"),
  ];
}

// 声のミュートを、ドックの人の行のミュートのボタンで往復し、配信の音 (録画の波形) に反映されるかを見る。
// 録画を 1 本始め、先に声を全員ミュートにしてから、1 人ずつ「ミュートを外す (基準) -> ミュート -> 外す (戻した)」を区間として残す
// (区間の端は mark-time の印。ac32MarkLabel)。1 人ずつ鳴らすのは、複数の声の 1 kHz が重なってうなりにならないようにするため。
// 判定は OBS を閉じた後 (judge-ac32)。
// 声の音量 (200%) は、ドックの [音声] タブのつまみで決める。ツールにつまみを動かす手順がないので、この往復は
// ミュートだけを見る (音量が配信の音に反映されるかは --extra-checks の (f) で、人がつまみを動かして確認する)。
export function ac32MarkLabel(slot, segment) {
  return "ac32-v" + slot + "-" + segment;
}
function callAc32Steps(args) {
  const people = callVoicePersons(args);
  const steps = [
    ...voiceIsolateSteps(people, null, "声のミュート: 録画の前に声を全員ミュートにする"),
    step("record-start", "声のミュートの往復の録画を始める (声を 1 人ずつ鳴らし、ミュートの往復の音量を区間ごとに読む)", { stream: false, label: "ac32" }),
  ];
  people.forEach((person, k) => {
    const v = k + 1;
    const mark = (segment, what) => step("mark-time", "声のミュート: 声 " + v + " (" + person + ") の" + what, { label: ac32MarkLabel(v, segment) });
    steps.push(
      rowMuteStep("voice", person, "off", "声のミュート: 声 " + v + " の基準の区間のため、ミュートを外す"),
      mark("base", "基準の区間の始まり"),
      step("wait", "声のミュート: 声 " + v + " の基準の区間", { ms: AC32_SEGMENT_MS }),
      rowMuteStep("voice", person, "on", "声のミュート: 声 " + v + " をミュートする"),
      mark("muted", "ミュートの区間の始まり"),
      step("wait", "声のミュート: 声 " + v + " のミュートの区間", { ms: AC32_SEGMENT_MS }),
      rowMuteStep("voice", person, "off", "声のミュート: 声 " + v + " のミュートを外す"),
      mark("restored", "戻した区間の始まり"),
      step("wait", "声のミュート: 声 " + v + " の戻した区間", { ms: AC32_SEGMENT_MS }),
      mark("end", "区間の終わり"),
      rowMuteStep("voice", person, "on", "声のミュート: 次の人のため、この声をミュートに戻す"),
    );
  });
  steps.push(step("record-stop", "声のミュートの往復の録画を終了する", { stream: false, tailSeconds: 0, label: "ac32" }));
  steps.push(...voiceRestoreSteps(people, "声のミュート: ミュートを全員外して元に戻す"));
  return steps;
}

// --step second-obs のシーンコレクションの往復の手順を流用する。
// **基準の記録を取り直してから切り替える**: call-1 の wait-connection は 20 分以上前に
// 終わっているので、その時点の記録を基準にすると途中の繋ぎ直しまで「切り替えのせい」に
// 見えてしまう (wait-connection は hello が既にあれば即座に戻り、基準を現在の記録に置き直す)。
function callAc35Steps() {
  return [
    step("wait-connection", "切り替えの直前の記録を基準として取り直す (既に繋がっているので即座に戻る)", { timeoutMs: SECOND_OBS_CONNECT_WAIT_TIMEOUT_MS }),
    step("obsws-request", "空のシーンコレクション (" + SECOND_OBS_EMPTY_SCENE_COLLECTION + ") へ切り替える", {
      request: "SetCurrentSceneCollection",
      requestData: { sceneCollectionName: SECOND_OBS_EMPTY_SCENE_COLLECTION },
      track: "scene-collection-switch",
      trackField: "switchAwayOk",
    }),
    step("obsws-request", "切り替えが実際に反映されたか確認する", {
      request: "GetSceneCollectionList",
      track: "scene-collection-switch",
      trackField: "duringSwitchCollectionName",
    }),
    step("wait", "切り替えたまま待つ", { ms: SECOND_OBS_SWITCH_HOLD_MS }),
    step("obsws-request", "元のシーンコレクション (" + SECOND_OBS_MAIN_SCENE_COLLECTION + ") へ戻す", {
      request: "SetCurrentSceneCollection",
      requestData: { sceneCollectionName: SECOND_OBS_MAIN_SCENE_COLLECTION },
      track: "scene-collection-switch",
      trackField: "switchBackOk",
    }),
    step("obsws-request", "戻した後、Main に枠のソースが残っているか確認する", { request: "GetSceneItemList", requestData: { sceneName: "Main" }, label: "ac35-after-switch-back" }),
    // 確認することは「切り替えの間に hello / close が増えず、**戻した後に欠落が
    // ない**」の 2 つである。項目が並んでいること (上の GetSceneItemList) と繋がったままで
    // あること (下の judge-connection-held) だけでは、**絵が再び乗ったこと**を示す材料が
    // 1 つも残らない。流用元 (--step second-obs) と同じく、戻した後に短く録って frames.py に
    // 掛ける (call-1 のこの時点では録画も配信も止まっているので、新しく録れる)。
    step("record-start", "戻した後に枠のソースへ映像が再び乗ることを録る", { stream: false, label: "ac35" }),
    step("wait", "枠のソースへ映像が再び乗るのを録る猶予", { ms: SECOND_OBS_POST_SWITCH_RECORD_SEC * 1000 }),
    step("record-stop", "切り替え後の録画を終了する", { stream: false, tailSeconds: 0, label: "ac35" }),
    step("judge-connection-held", "切り替えの前後で hello / close が増えていないか (繋がったまま切り替えられたか) を確認する", { gate: true }),
  ];
}

// 名前ラベルの見た目の設定が合成のソースの絵に反映されること。設定を変える前の絵と、ドックの [表示] タブの
// 「名前」の [表示] を外した後の絵を、合成のソースの GetSourceScreenshot で撮り、後の絵で名前ラベルの板が狭まるか消えることを
// 比べる (judge-ac31。plate_measure.py で板を測る)。見た目の細部 (位置・大きさ・色・光) は composite-check の c1-c8 が受け持つので、
// ここでは「設定が絵に反映される」だけを見る。表示タブは押すとすぐ反映されるので、[適用] はない。最後に [表示] を戻す。
// **録画と配信を始める前に置く** (配信中の操作の数に入れないため)。
export const AC31_LABELS = { before: "ac31-before", after: "ac31-after" };
function callAc31Steps() {
  const showName = (desired, note) =>
    uiClickStep("名前ラベルの設定: [" + TEXT_ID_LOOK_HEADING_NAME + "] の [" + TEXT_ID_LOOK_SHOW + "] を" + (desired === "on" ? "入れる" : "外す") + " (" + note + ")", DOCK_WINDOW, TEXT_ID_LOOK_SHOW, {
      controlType: "CheckBox",
      group: TEXT_ID_LOOK_HEADING_NAME,
      desired,
    });
  return [
    step("screenshot-source", "名前ラベルの設定の確認 (設定を変える前): 合成のソースの絵を PNG に撮る", { source: { kind: "composite" }, label: AC31_LABELS.before }),
    ...displayTabSteps("名前ラベルの設定: 見た目の設定を変える"),
    showName("off", "見た目の設定を変える"),
    step("wait", "名前ラベルの設定: 変えた見た目が絵に出るまでの猶予", { ms: UI_APPLY_SETTLE_MS }),
    step("screenshot-source", "名前ラベルの設定の確認 (名前の [表示] を外した後): 合成のソースの絵を PNG に撮る", { source: { kind: "composite" }, label: AC31_LABELS.after }),
    showName("on", "元に戻す"),
    // 後の手順 (callAc30Steps などの行を押す手順) のために [参加者] タブへ戻す。
    peopleTabStep("名前ラベルの設定: 見た目の設定を戻した後"),
    step("wait", "名前ラベルの設定: 戻した見た目が絵に出るまでの猶予", { ms: UI_APPLY_SETTLE_MS }),
    step("judge-ac31", "名前ラベルの設定: 前後の絵の名前ラベルの板を plate_measure.py で測り、外した後に板が狭まるか消えたかを当てる (記録するだけ)", { before: AC31_LABELS.before, after: AC31_LABELS.after }),
  ];
}

// ドックのウィンドウの文を押さずに読む (dialog-click.ps1 -ListOnly。rtDockListStep と同じツール)。案内はプラグインの
// 記録に出ない (ledger の規則がドックへ描くだけ) ので、案内が出た / 消えたはここで読んだ文で判定する。
// opts.buttonTextId を渡すとそのボタンが「出ている」(expect button) か「消えている」(expect no-button) かを見る。
function dockTextsStep(label, opts) {
  const o = opts || {};
  const absent = o.expect === "no-button";
  const what = o.buttonTextId
    ? "ドックのウィンドウを押さずに読む: [" + o.buttonTextId + "] が" + (absent ? "消えている" : "出ている") + "か (" + label + ")"
    : "ドックのウィンドウの文を押さずに読む (" + label + ")";
  return step("dialog-click", what, {
    target: "first",
    ...DOCK_WINDOW,
    buttonTextId: o.buttonTextId || null,
    expect: o.buttonTextId ? (absent ? "no-button" : "button") : "present",
    listOnly: true,
    timeoutMs: absent ? ROUNDTRIP_DIALOG_ABSENT_TIMEOUT_MS : ROUNDTRIP_DIALOG_TIMEOUT_MS,
    label,
    ...(o.gate ? { gate: true } : {}),
  });
}

// 読んだ文に、メッセージの見出し (ini の値の %1 より前の字) が有る / ないを判定する (純関数は judgeNamedInTexts /
// judgeNotNamedInTexts。文言の解決は run-window.mjs が ini から行う = textPrefix)。textIds は 1 つか並び。並びのとき、
// 「有る」はどれか 1 つが出ていれば緑 (anyName)、「ない」はどれも出ていなければ合格。見出しは人の名前が分かるかで
// 2 つの鍵を使い分ける (main_account_not_in_call と _no_name) ので、両方を渡す。
function noticeJudgeStep(present, textIds, dialogLabel, gate, what) {
  return step("judge-rt", what, {
    judge: present ? "named-in-texts" : "not-named-in-texts",
    inputs: { texts: { dialog: dialogLabel }, names: { textPrefix: textIds } },
    ...(present && Array.isArray(textIds) ? { anyName: true } : {}),
    ...(gate ? { gate: true } : {}),
  });
}

// (a) 別アカウント方式。ドックの上部のメインの行の [変更] を押し、メニューから配信者 (通話中の人の見えている名前) を選んで、
// 「メインアカウントが通話にいない」と「映像が届かない」のメッセージが出て消えることを、人の操作の前後でドックの文から判定する。
// **メニューから選ぶのは実行スクリプト、通話を抜ける / 戻る / ミュートは人が行う**。
// 製品はメインアカウントをメッセージ (ledger) にだけ使い、枠の割り当てには使わない。枠の数の変化は
// GetInputList と PNG に**記録するだけ**で判定しない (README)。以後は別アカウント方式のまま続ける。
// メニューの項目は人の見えている名前なので、ini の id ではなく文字列そのもので指す (uiClickStep の buttonText)。
// 数字の ID は、メニューが人の名前で並ぶので画面に出ない。--broadcaster-id は、別アカウント方式のときに実行する手順を決める印と、
// 記録から ID を伏せるために残す。
const NOT_IN_CALL_HEADLINES = [TEXT_ID_MAIN_ACCOUNT_NOT_IN_CALL, TEXT_ID_MAIN_ACCOUNT_NOT_IN_CALL_NO_NAME];
// リハーサルは、テスト用の送信側の声だけの人をメインアカウントに選び (mainAccountFor)、メニューと配信者が通話にいる間の判定を通す。
// 配信者が抜ける・戻る判定は、テスト用の送信側では人を抜けさせられないので判定しない (REHEARSAL_NOT_JUDGED)。
function callBroadcasterSteps(args, poGate) {
  const main = mainAccountFor(args);
  if (main === null) return [rehearsalSkipStep("broadcaster-left", "別アカウント方式の手順", { reason: REHEARSAL_NO_MAIN_REASON })];
  const leftJudge = (dialogLabel, present, what) =>
    args.rehearsal ? rehearsalSkipStep("broadcaster-left", what) : noticeJudgeStep(present, NOT_IN_CALL_HEADLINES, dialogLabel, poGate, what);
  return [
    uiClickStep("ドックの [" + TEXT_ID_DOCK_ACCOUNT_CHANGE + "] を押してメニューを開く (別アカウント方式に切り替える)", DOCK_WINDOW, TEXT_ID_DOCK_ACCOUNT_CHANGE, { noWait: true, gate: true }),
    uiClickStep("メニューから配信者「" + main + "」を選ぶ (別アカウント方式に切り替える)", MENU_POPUP, null, { controlType: "MenuItem", buttonText: main, noWait: true, gate: true }),
    step("screenshot-window", "メインアカウントを選んだ後のドックを PNG に撮る", { target: "first", shotLabel: "broadcaster-change" }),
    step("wait", "選んだ値が保存され、メッセージが引き直されるまでの猶予", { ms: CALL_ACCOUNT_CHANGE_SETTLE_MS }),
    step("obsws-request", "別アカウント方式に切り替えた直後の入力の一覧 (枠に載っている人の名前の記録)", { request: "GetInputList", label: "broadcaster-after-change" }),
    dockTextsStep("broadcaster-in-call"),
    noticeJudgeStep(false, NOT_IN_CALL_HEADLINES, "broadcaster-in-call", poGate, "配信者が通話に参加している間は「ふだんのアカウントがこの通話に参加していません」のメッセージがない"),
    humanStep("Desktop版のDiscord (" + main + ") を通話から抜けさせてください", "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", "配信者が抜けたことがドックに届くまでの猶予", { ms: CALL_NOTICE_SETTLE_MS }),
    step("screenshot-window", "配信者が通話に参加していない間のドックを PNG に撮る", { target: "first", shotLabel: "broadcaster-left" }),
    step("collect-plugin-log", "プラグインの記録の回収 (配信者が通話に参加していない間)"),
    dockTextsStep("broadcaster-left"),
    leftJudge("broadcaster-left", true, "配信者が通話に参加していないと「ふだんのアカウントがこの通話に参加していません」のメッセージが出る"),
    humanStep(main + "を通話に戻してください", "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", "配信者が戻ったことがドックに届くまでの猶予", { ms: CALL_NOTICE_SETTLE_MS }),
    step("screenshot-window", "配信者が戻った後のドックを PNG に撮る", { target: "first", shotLabel: "broadcaster-back" }),
    dockTextsStep("broadcaster-back"),
    leftJudge("broadcaster-back", false, "配信者が戻るとメッセージが消える"),
    // --skip-speaker-mute (CALL_ONE_OPTION_FLAGS のコメント): ここから下のスピーカーミュートと解除を省く。
    ...(args.skipSpeakerMute ? [] : callSpeakerMuteSteps(poGate)),
  ];
}

// 別アカウント方式の段のスピーカーミュートと解除。「メッセージが出ている」と「映像が実際に止まっている」の
// 一致だけを見る (speakerMuteJudgeSteps)。
function callSpeakerMuteSteps(poGate) {
  return [
    humanStep("Vesktopのスピーカーをミュートにしてください", "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", "スピーカーミュートがドックに届くまでの猶予", { ms: CALL_NOTICE_SETTLE_MS }),
    step("screenshot-window", "スピーカーミュートの間のドックを PNG に撮る", { target: "first", shotLabel: "speaker-mute" }),
    step("collect-plugin-log", "プラグインの記録の回収 (スピーカーミュートの間)"),
    dockTextsStep("speaker-mute"),
    ...speakerMuteJudgeSteps("speaker-mute", poGate, "スピーカーミュートの間: メッセージは映像が実際に止まったときだけ出る (止まったかどうかとメッセージの有無を記録する)"),
    humanStep("Vesktopのスピーカーのミュートを解除してください", "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", "ミュートの解除がドックに届くまでの猶予", { ms: CALL_NOTICE_SETTLE_MS }),
    dockTextsStep("speaker-unmuted"),
    ...speakerMuteJudgeSteps("speaker-unmuted", poGate, "ミュートを解除した後: メッセージは映像が実際に止まったときだけ出る"),
  ];
}

// スピーカーミュートの判定 (VIDEO_RATE_WINDOW_LINES): 直前に読んだドックの文 (dialogLabel) に案内が出ていることと、プラグインの記録の
// 映像の数が止まっていることが一致するか。ミュートしても映像が止まるとは限らないので、「ミュートの間は案内が
// 出る」とは決め打ちしない。
function speakerMuteJudgeSteps(dialogLabel, gate, what) {
  return [
    step("measure-video-rate", "プラグインの記録から、届いている映像の数 (stats video received_hz の直近 " + VIDEO_RATE_WINDOW_LINES + " 行) を読む (" + dialogLabel + ")", {
      label: dialogLabel,
      windowLines: VIDEO_RATE_WINDOW_LINES,
    }),
    step("judge-rt", what, {
      judge: "notice-iff-frames-stopped",
      inputs: { texts: { dialog: dialogLabel }, names: { textPrefix: TEXT_ID_NO_VIDEO }, videoRate: { videoRate: dialogLabel } },
      ...(gate ? { gate: true } : {}),
    }),
  ];
}

// (b)(g) テスト動画で A/V。共有の中身は白のマーカー + 2 kHz のテスト用の動画 (sync.py がそのまま
// 使える形)。配信は続いているので録画だけを始める (stream:false)。label は av1 / av2。
// **録画の間はドックの行で、声をみんなミュートにし、ほかの人の画面共有の音をミュートにして、テスト動画を共有している人の画面共有の音の
// ミュートを外す**。合成のソースの音は全員の音を混ぜた 1 本なので、そうしないと、Chrome のテスト用のマイクの 1 kHz や、
// ほかの人の画面共有の音 (画面全体の共有には、スピーカーから出たテスト動画の音も入る) が録画に混ざり、マーカーの組が別の人の音と組になる。
// 画面共有の音は共有が始まるたびにミュートから始まる (docs/spec.md) が、前の手順で外したままの人がいても混ざらないよう、毎回ミュートにする
// (ミュートのときに押しても状態は変わらない。dock-click.ps1 は行き先と同じなら押さない)。
// 戻すのは録画を止めた直後で、声のミュートを外し、テスト動画の人の画面共有の音を既定のミュートに戻す (ほかの人の画面共有の音は既定のミュートの
// まま)。ドックの行のミュートは覚えられない (次に OBS を起動すると元に戻る) ので、途中で抜けた経路の後始末は要らない。
// message が null のときは人に頼まない (--recheck は参加の文でテスト動画の共有まで頼んでいる)。
function callAvSteps(args, label, message, screens = CALL_SCREEN_COUNT, targetAccount = callAccounts(args).chrome) {
  const everyone = callVoicePersons(args);
  const others = callScreenSharers(args).filter((p) => p !== targetAccount);
  const isolate = [
    peopleTabStep("A/V の録画 (" + label + "): 行のミュートを押す前"),
    ...voiceIsolateSteps(everyone, null, "A/V の録画 (" + label + "): 声をミュートにして、録画のミックスにマーカー以外の 1 kHz を混ぜない"),
    ...others.map((p) => rowMuteStep("screen", p, "on", "A/V の録画 (" + label + "): ほかの人の画面共有の音を録画のミックスに混ぜない", { absentOk: true })),
    rowMuteStep("screen", targetAccount, "off", "A/V の録画 (" + label + "): テスト動画の音を鳴らす"),
  ];
  const restore = [
    rowMuteStep("screen", targetAccount, "on", "A/V の録画 (" + label + "): 画面共有の音を既定のミュートに戻す"),
    ...voiceRestoreSteps(everyone, "A/V の録画 (" + label + "): 声のミュートを戻す"),
  ];
  return [
    ...(message === null ? [] : [humanStep(message, "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHARE_S)]),
    step("wait", "共有の映像と音が枠に乗るまでの猶予", { ms: CALL_AV_SHARE_SETTLE_MS }),
    ...isolate,
    step("record-start", "A/V の録画を始める (" + label + "。配信は続けたまま録画だけ)", { stream: false, label }),
    step("wait", "A/V の録画の区間 (" + CALL_AV_RECORD_MS / 60000 + " 分)", { ms: CALL_AV_RECORD_MS }),
    step("record-stop", "A/V の録画を終了する (" + label + ")", { stream: false, tailSeconds: 0, label }),
    ...restore,
    step("collect-recording", "録画ファイルの回収 (" + label + ")", { label }),
    step("judge", "sync.py でテスト動画の A/V のずれを見る (2x2 の区画ごと。" + label + ")", { tool: "sync.py", cells: screens, recording: label, label }),
  ];
}

// (c) 途中で録画 ON。--step record と同じボタン (ドックの下のバー) を押し、同じ判定ツール 3 つを再利用する。
function callMidRecordingSteps() {
  return [
    step("dock-click", "ドックの [" + TEXT_ID_RECORD_START + "] を押してcordmuxの録画を始める (配信の途中で)", {
      target: "first",
      textId: TEXT_ID_RECORD_START,
      desired: "on",
      gate: true,
    }),
    step("wait", "配信の途中で始めた録画の区間 (" + CALL_MID_RECORD_MS / 60000 + " 分)", { ms: CALL_MID_RECORD_MS }),
    step("screenshot-window", "録画中のドックを PNG に撮る (ボタンのテキストと状態)", { target: "first", shotLabel: "mid-record-on" }),
    step("dock-click", "ドックの [" + TEXT_ID_RECORD_STOP + "] を押してcordmuxの録画を終える", {
      target: "first",
      textId: TEXT_ID_RECORD_STOP,
      namePrefix: true,
      desired: "off",
      gate: true,
    }),
    step("wait-finalize", "録画ログ (ledger.jsonl) に録画ファイルの作成の行が出そろうのを待つ", {
      recordDirRel: RECORD_DIR_REL,
      timeoutMs: RECORD_FINALIZE_WAIT_TIMEOUT_MS,
      pollMs: RECORD_FINALIZE_POLL_MS,
      gate: true,
    }),
    step("collect-record-session", "cordmux の録画のフォルダ (録画ログ・区間・黒・完成) の一覧を結果に残す", { recordDirRel: RECORD_DIR_REL }),
    step("judge-record", "ledger_check.py: 録画ログと ffprobe を整数で突き合わせる (--step record と同じ判定)", { tool: "ledger_check.py", gate: true }),
    step("judge-record", "black_span.py: 完成の最初の実フレームより前に黒がないこと (先頭の黒は録画ログの欠落のぶんだけ許す = --allow-leading)", {
      tool: "black_span.py",
      mode: "no-leading-black",
      target: "cordmux",
      gate: true,
    }),
    step("judge-record", "au_hash.py: 送った AU と完成の AU の同一性 (区間ごとの framemd5)", { tool: "au_hash.py", gate: true }),
  ];
}

// (d) 使う声の置き換え。行のメニューの操作は人が行う (下の階層のメニューの項目を実行スクリプトのツールで
// 押したことはまだない = README.md の実測)。判定は PNG と記録。人への文はメニューのテキスト (ja-JP.ini の値) を textRef で
// そのまま引く (画面に出ている語で依頼する。「%1 の声を使う」を連結で組み直すと、ini の書き方が変わったときに
// 文字列が食い違う)。「使う声」の下の項目は「本人の声」と通話に参加している人の名前 (row_menu.cpp)。
// available が false のときだけ 人に依頼せずに飛ばし、飛ばした事実と理由を skip の手順が結果に残す (行のメニューを
// 持たない版の製品のための形)。
export function callVoiceSourceSteps(available = true, args = null) {
  if (!available) {
    return [
      step("skip", "使う声の置き換えを飛ばす: このバージョンの製品は行のメニューに「使う声」を出さない", {
        requirement: "voice source replacement",
        reason: "the product has no voice-from menu in the participant row",
      }),
    ];
  }
  return [
    humanStep(
      "ドックの [" + textRef(TEXT_ID_TAB_PEOPLE) + "] タブの一覧で、" + callAccounts(args).firefox + "の行の [...] (" + textRef(TEXT_ID_ROW_MORE_ACTIONS) + ") を押し、[" +
        textRef(TEXT_ID_ROW_VOICE_FROM) + "] の中の「" + callAccounts(args).chrome + "」を選んでください",
      "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", "使う声が置き換わるまでの猶予", { ms: CALL_VOICE_SOURCE_SETTLE_MS }),
    step("screenshot-window", "使う声を置き換えた後のドックを PNG に撮る", { target: "first", shotLabel: "voice-source-changed" }),
    step("collect-plugin-log", "プラグインの記録の回収 (使う声の置き換え)"),
    humanStep(
      "同じ行の [...] を押し、[" + textRef(TEXT_ID_ROW_VOICE_FROM) + "] の中の「" + textRef(TEXT_ID_ROW_OWN_VOICE) + "」を選んで元に戻してください",
      "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", "使う声が戻るまでの猶予", { ms: CALL_VOICE_SOURCE_SETTLE_MS }),
  ];
}

// --extra-checks の (c) (CALL_ONE_OPTION_FLAGS のコメント): 使う声の置き換えを、音で確認する短い録画を挟んだ形で実行する。
// 置き換えの後 (PNG と記録の回収の直後) と、戻した後 (20 秒の待ちの直後) に、VOICE_SOURCE_CHECKS の 2 つずつを録る。
// 録画ごとに「その人の声だけを鳴らし、ほかの人の声を全部ミュート」(ドックの行の声のミュート) -> 録画 -> 戻す。
// 人の名前は実際の通話では画面共有している人 / 声を貸す人 (役割 firefox / chrome のアカウント)、リハーサルではテスト用の送信側の 1 つ目と 2 つ目の声 (人の操作が
// ないので置き換わらず、判定は記録だけ)。テスト用の送信側は、声のストリームに画面のストリームの後の番号を付ける (test/tools/harness/sender_pure.ts の
// assignStreamIds。画面が 3 ストリームなら声は harness-voice-4 から)。判定 (tone_scan.py の 1 kHz) は OBS を閉じた後 (callAnalysisSteps の judge-voice-source)。
// 1 つの録画に時刻の印を打って区間で読む方法は、区間ごとに鳴らす人を替えるので、結局ミュートの付け替えごとに区切りが要る。
// OBS の録画の音のトラックを参加者ごとに分ける方法は、テスト用の OBS の出力の設定を書き換えることになり、ほかの録画の判定と食い違う。
export function voiceSourcePeople(args) {
  return args && args.rehearsal
    ? { owner: harnessPersonName("voice", callScreens(args) + 1), from: harnessPersonName("voice", callScreens(args) + 2) }
    : { owner: callAccounts(args).firefox, from: callAccounts(args).chrome };
}

function voiceSourceCheckRecordingSteps(args, phase) {
  const people = voiceSourcePeople(args);
  const everyone = callVoicePersons(args);
  const steps = [];
  for (const check of VOICE_SOURCE_CHECKS.filter((c) => c.phase === phase)) {
    const person = people[check.keep];
    steps.push(
      ...voiceIsolateSteps(everyone, person, "使う声の確認 (" + check.label + "): " + person + " の声だけを鳴らし、ほかの人の声をミュートする"),
      step("record-start", "使う声の確認の録画を始める (" + check.label + "。配信は続けたまま録画だけ)", { stream: false, label: check.label }),
      step("wait", "使う声の確認の録画の区間 (" + VOICE_SOURCE_CHECK_RECORD_MS / 1000 + " 秒)", { ms: VOICE_SOURCE_CHECK_RECORD_MS }),
      step("record-stop", "使う声の確認の録画を終了する (" + check.label + ")", { stream: false, tailSeconds: 0, label: check.label }),
      ...voiceRestoreSteps(everyone, "使う声の確認 (" + check.label + "): ミュートを戻す"),
    );
  }
  return steps;
}

// 声の音量 (VOICE_VOLUME_PCT のコメント): 置き換えた後の画面共有している人の枠で、人が [音声] タブの声のつまみを 200% にし、
// 100% の録画 (VOICE_VOLUME_BEFORE_LABEL = 置き換えの確認の 1 つ目。画面共有している人の声だけを鳴らす条件) の次に、同じ条件でもう 1 回録る。
// 戻さない: 次に依頼する「本人の声」に戻した後は画面共有している人の声が無音で、200% のままでもこの後の確認に適用されない。
function voiceVolumeSteps(args) {
  const people = voiceSourcePeople(args);
  const everyone = callVoicePersons(args);
  const what = "声の音量 (" + VOICE_VOLUME_PCT + "%)";
  return [
    ...voiceIsolateSteps(everyone, people.owner, what + ": " + people.owner + " の声だけを鳴らし、ほかの人の声をミュートする"),
    humanStep(
      "ドックの [" + textRef(TEXT_ID_TAB_AUDIO) + "] タブを開き、" + people.owner + "の行の [" + textRef(TEXT_ID_AUDIO_VOICE_KIND) +
        "] のつまみを右端 (" + VOICE_VOLUME_PCT + "%) まで動かしてください。動かしたら [" + textRef(TEXT_ID_TAB_PEOPLE) + "] タブへ戻してください",
      "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", what + ": 音量が配信の音に届くまでの猶予", { ms: VOICE_VOLUME_SETTLE_MS }),
    step("record-start", what + ": 音量を上げた後の録画を始める (配信は続けたまま録画だけ)", { stream: false, label: VOICE_VOLUME_AFTER_LABEL }),
    step("wait", what + ": 録画の区間 (" + VOICE_SOURCE_CHECK_RECORD_MS / 1000 + " 秒)", { ms: VOICE_SOURCE_CHECK_RECORD_MS }),
    step("record-stop", what + ": 録画を終了する", { stream: false, tailSeconds: 0, label: VOICE_VOLUME_AFTER_LABEL }),
    ...voiceRestoreSteps(everyone, what + ": ミュートを戻す"),
  ];
}

export function callVoiceSourceStepsWithCheck(args) {
  const base = callVoiceSourceSteps(true, args);
  // base は [依頼, 待つ, PNG, 記録, 戻してもらう, 待つ] (callVoiceSourceSteps)。置き換えた後の録画は記録の回収の直後、音量の録画はその次
  // (置き換えたまま)、戻した後の録画は最後。
  return [
    ...base.slice(0, 4),
    ...voiceSourceCheckRecordingSteps(args, "switched"),
    ...voiceVolumeSteps(args),
    ...base.slice(4),
    ...voiceSourceCheckRecordingSteps(args, "restored"),
  ];
}

// --extra-checks の (b): cordmux の設定のウィンドウの [バージョン情報] のページの「バージョン」を読み、Vesktop の行が「-」でないかを見る
// (バージョンを運ぶ経路がないと、ドックの Vesktop のバージョンが「-」になる)。読むだけで設定は変えないので [キャンセル] で閉じる。
// 録画と配信を始める前に置く (配信中の操作の数を増やさない)。
function callVersionCheckSteps(args) {
  return [
    ...settingsPageSteps(TEXT_ID_SETTINGS_PAGE_ABOUT, "バージョンの表示を読む"),
    step("dialog-click", "cordmux の設定のウィンドウの [" + TEXT_ID_SETTINGS_PAGE_ABOUT + "] のページの文を押さずに読む (バージョンの表)", {
      target: "first",
      ...SETTINGS_WINDOW,
      buttonTextId: null,
      expect: "present",
      listOnly: true,
      timeoutMs: UI_FIND_TIMEOUT_MS,
      label: "about-versions",
    }),
    step("screenshot-window", "[" + TEXT_ID_SETTINGS_PAGE_ABOUT + "] のページを PNG に撮る (バージョンの表)", { target: "first", shotLabel: "about-versions" }),
    // バージョンの行の読み: ラベルと値の順に頼らない。表の 7 つのラベルのテキスト (labels) でラベルと値を分け、cordmux (OBS) の行 (いつも
    // バージョンのテキストが出る行 = reference) でラベルと値のどちらが先かを確認してから Vesktop の行の値を読む。値が読めない行は製品が読み上げの
    // 名前を「取得できません」(unavailable) にするので、UI Automation で読むテキストは「-」ではなくそれになる。
    args && args.rehearsal
      ? rehearsalSkipStep("version-row", "バージョンの表の Vesktop の行")
      : step("judge-rt", "バージョンの表の Vesktop の行が「-」でない (ラベルと値の順は cordmux (OBS) の行で確認する)", {
          judge: "version-row-shown",
          inputs: {
            texts: { dialog: "about-versions" },
            names: { textPrefix: TEXT_ID_SETTINGS_VERSION_VESKTOP },
            labels: { textPrefix: TEXT_ID_SETTINGS_VERSION_LABELS },
            reference: { textPrefix: TEXT_ID_SETTINGS_VERSION_OBS_PLUGIN },
            unavailable: { textPrefix: TEXT_ID_SETTINGS_UNAVAILABLE_A11Y },
          },
        }),
    settingsButtonStep(OBS_TEXT_CANCEL, "読むだけなので変えずに閉じる"),
  ];
}

// --extra-checks の (f): メインの手順の録画の間(定常状態の終わり)に、ドックの [状態] タブを開き、参加者ごとの映像の受信の表
// (途切れ・合計・遅延・最大。行のツールチップに直近 1 分の遅延フレーム・カクつき・平均遅延・最大遅延) を PNG と文で残す。遅延と最大は
// 直近 1 分の値なので、録画と配信の負荷の下の定常状態の終わりに読む。タブを開いて読んで [参加者] タブへ戻すだけで、設定は変えない。
// 配信中に押すので、押す手順に readOnlyOp を付ける (judgeOperationCount は読むだけの操作を配信中の「その他の操作」に数えない)。
// タブの名前は、メッセージの件数があると「状態 (N)」になるので、名前の先頭で探す (namePrefix)。
export const INCOMING_VIDEO_LABEL = "incoming-video";
// タブを開いてから表が描き直されるまでの猶予 (表は周期で更新される。設定の反映を待つ猶予 UI_APPLY_SETTLE_MS と同じ長さ)。
export const INCOMING_VIDEO_SETTLE_MS = 3000;
function incomingVideoSteps() {
  const readOnly = (list) => list.map((s) => (s.kind === "dialog-click" ? { ...s, readOnlyOp: true } : s));
  return [
    ...readOnly([uiClickStep("ドックの [" + TEXT_ID_TAB_STATUS + "] タブを選ぶ (映像の受信の統計を読む)", DOCK_WINDOW, TEXT_ID_TAB_STATUS, { controlType: "TabItem", namePrefix: true })]),
    step("wait", "[" + TEXT_ID_TAB_STATUS + "] タブの表が描かれるまでの猶予", { ms: INCOMING_VIDEO_SETTLE_MS }),
    step("dialog-click", "ドックの [" + TEXT_ID_TAB_STATUS + "] タブの文を押さずに読む (映像の受信の表)", {
      target: "first",
      ...DOCK_WINDOW,
      buttonTextId: null,
      expect: "present",
      listOnly: true,
      timeoutMs: UI_FIND_TIMEOUT_MS,
      label: INCOMING_VIDEO_LABEL,
    }),
    step("screenshot-window", "[" + TEXT_ID_TAB_STATUS + "] タブを PNG に撮る (映像の受信の表)", { target: "first", shotLabel: INCOMING_VIDEO_LABEL }),
    ...readOnly([uiClickStep("ドックの [" + TEXT_ID_TAB_PEOPLE + "] タブへ戻す", DOCK_WINDOW, TEXT_ID_TAB_PEOPLE, { controlType: "TabItem" })]),
  ];
}

// --extra-checks の (a): 送信部の接続の数を保存する名前と、それと比べる手順。参加と共有の前 (before-join) と比べるのは共有の後 (after-join)、
// 共有の後に取り直した数 (before-main) と比べるのはメインの手順の後 (after-main)。取り直すのは、共有で落ちて起き直したときでも、メインの手順の間に落ちたかを
// 分けて読むため (同じバックアップと比べると、共有のときの切断がメインの手順の後の判定にも残る)。
export const JOIN_BASELINE_LABEL = "before-join";
export const MAIN_BASELINE_LABEL = "before-main";
function connectionsKeptStep(label, baseline, what) {
  return step("judge-connections-kept", what, { baseline, label });
}

// ---- 追加の手順 ------------------------------------------------------------------------

// コピーして比べるのは --start-with-add の実際の通話だけ (リハーサルは偽の APPDATA を使うので、コピーするものがない)。
export function vencordSettingsCopied(args) {
  return Boolean(args && args.step === "call-1" && args.startWithAdd && args.realProfile && !args.rehearsal);
}
function vencordSettingsCopyStep() {
  return step("copy-vencord-settings", "実際の Vesktop の中の Vencord の設定ファイル (" + VENCORD_SETTINGS_REAL_REL + ") を結果フォルダへコピーする (読むだけ・コピーするだけ。戻すのは利用者の同意を得てから)", {
    appDataRel: VENCORD_SETTINGS_REAL_REL,
    destName: VENCORD_SETTINGS_BEFORE_NAME,
  });
}
function vencordSettingsRestoredStep() {
  return step("compare-vencord-settings", "アンインストールの後の Vencord の設定ファイルが、実行の始めのコピーと鍵の上で同じか (違えば失敗。利用者の設定を壊したことになる)", {
    appDataRel: VENCORD_SETTINGS_REAL_REL,
    beforeName: VENCORD_SETTINGS_BEFORE_NAME,
    afterName: VENCORD_SETTINGS_AFTER_UNINSTALL_NAME,
    mustMatch: true,
    gate: true,
  });
}
function vencordSettingsCompareStep() {
  return step("compare-vencord-settings", "Vesktop を終了してもらった後の Vencord の設定ファイルを、コピーと鍵の上で比べる (違いの鍵の道と変わり方だけを記録する。戻さない)", {
    appDataRel: VENCORD_SETTINGS_REAL_REL,
    beforeName: VENCORD_SETTINGS_BEFORE_NAME,
    afterName: VENCORD_SETTINGS_AFTER_NAME,
  });
}

// アンインストールの後の比べ。実行の始めのコピーを取れていない (copied が偽) か、比べが失敗したか、鍵に違いがあれば失敗にする。
// コピーを取れていないのに「違いがない」とは言えないので、失敗にする。compared は compare-vencord-settings の結果 (ok, changed, changes)、
// copied は実行の始めのコピーを取れたか (copy-vencord-settings が済んだか)。
export function judgeVencordSettingsRestored({ copied, compared }) {
  if (!copied) return { ok: false, reason: "the copy taken before the run is missing, so the settings cannot be compared" };
  if (!compared || compared.ok !== true) return { ok: false, reason: "the comparison failed: " + String((compared && compared.error) || "no result") };
  const changed = Number(compared.changed) || 0;
  if (changed > 0) return { ok: false, reason: changed + " settings key(s) differ from the copy taken before the run" };
  return { ok: true, reason: null };
}

// 保存した診断情報の zip が、この実行の書き出しの後に書かれたものか。notBeforeMs は書き出しの操作を始めた時刻 (ms)。
// 時刻が分からない (zip がない、書き出しを始めていない) ときは新しいと言えないので false にする。
export function judgeBundleZipFresh({ mtimeMs, notBeforeMs }) {
  if (typeof mtimeMs !== "number" || !Number.isFinite(mtimeMs)) return { fresh: false, reason: "the zip has no modification time (it does not exist)" };
  if (typeof notBeforeMs !== "number" || !Number.isFinite(notBeforeMs)) return { fresh: false, reason: "the export was not started in this run" };
  if (mtimeMs < notBeforeMs) return { fresh: false, reason: "the zip is older than this run's export" };
  return { fresh: true, reason: null };
}

// Main の 2x2 で、画面の枠 screens 個が埋めた後の最初の空き (0 始まりの区画の番号)。2x2 に空きがあるのは 2 か 3 のときだけ
// (1 本なら枠が全面 = 合成のソースの 1 枠、4 本なら空きがない)。
export function latencyFreeCell(screens) {
  const n = Number(screens);
  return n === 2 || n === 3 ? n : null;
}
// 遅れを読む画面共有している人。実際の通話では呼び出し行の名前、リハーサルではマーカーと帯を出すテスト用の画面の 1 つ目 (参加者の行のチェックの手順と同じ置き換え)。
export function latencyPersonFor(args) {
  if (!args) return null;
  return args.rehearsal ? harnessPersonName("screen", 1) : args.latencyPerson || null;
}
// 人に見せる文 (名前は呼び出し行の値。文字列として持たない)。
export function latencyHumanMessage(person) {
  return (
    "Vesktopで、" + person + "の画面共有を押して大きく表示してください。表示できたら伝えてください (録り終わるまでの約" +
    LATENCY_RECORD_MS / 60000 + "分はそのままにし、Vesktopのウィンドウを最小化しないでください)"
  );
}

// 遅れを測る区間 (LATENCY_WINDOW_CAPTURE_SETTINGS のコメント)。人が大きく表示 -> 置く -> 落ち着くまで待つ -> 捉えたかを
// 見る (ソースの大きさと PNG) -> 録る -> 外す。判定 (latency.py) は OBS を閉じた後 (callExtraAnalysisSteps の judge-latency)。
// ウィンドウキャプチャは録る間だけ置き、すぐ外す (Vesktop のウィンドウをほかの録画と PNG に入れない)。
// 置くのは人が通知した後にする。依頼の前に置くと、配信が続いている間、人が表示を切り替えて
// 通知するまで (時限 10 分) の Vesktop のウィンドウも配信の受け口の flv に入ってしまう。
// 依頼の前に sceneItemEnabled false で置き、通知の後に出す方法は、出す手順の種類が 1 つ増えるので使わない (置いてからウィンドウを捉えるまでの
// 猶予は、どちらの形でも後ろの LATENCY_SETTLE_MS が持つ)。
function callLatencySteps(args, screens) {
  const person = latencyPersonFor(args);
  const cell = latencyFreeCell(screens);
  return [
    humanStep(latencyHumanMessage(person), "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("place-window-capture", "遅れ: Vesktop のウィンドウのウィンドウキャプチャを Main の空いた区画 (" + (cell + 1) + " 番目) に置く (vesktop.exe のウィンドウだけ。タイトルは持たない。人の通知の後)", {
      inputName: LATENCY_INPUT_NAME,
      inputKind: LATENCY_WINDOW_CAPTURE_KIND,
      inputSettings: LATENCY_WINDOW_CAPTURE_SETTINGS,
      sceneName: WIRE_SCENE_NAME,
      cell,
    }),
    step("wait", "遅れ: ウィンドウキャプチャがウィンドウを捉えて大きく表示した絵が乗るまでの猶予", { ms: LATENCY_SETTLE_MS }),
    step("check-window-capture", "遅れ: ウィンドウキャプチャが Vesktop のウィンドウを捉えたか (ソースの大きさと PNG が黒でないか)", {
      inputName: LATENCY_INPUT_NAME,
      sceneName: WIRE_SCENE_NAME,
      shotLabel: "latency-reference",
    }),
    step("switch-to-monitor-capture", "遅れ: 捉えられていなければ、Vesktop が最大化されて画面を覆うときだけ、その画面の画面キャプチャに切り替える (捉えていれば何もしない)", {
      inputName: LATENCY_INPUT_NAME,
      trialName: LATENCY_MONITOR_TRIAL_NAME,
      inputKind: LATENCY_MONITOR_CAPTURE_KIND,
      inputSettings: LATENCY_MONITOR_CAPTURE_SETTINGS,
      sceneName: WIRE_SCENE_NAME,
      settleMs: LATENCY_SETTLE_MS,
      shotLabel: "latency-reference",
    }),
    step("record-start", "遅れの録画を始める (配信は続けたまま録画だけ)", { stream: false, label: LATENCY_RECORDING_LABEL }),
    step("wait", "遅れの録画の区間 (" + LATENCY_RECORD_MS / 60000 + " 分)", { ms: LATENCY_RECORD_MS }),
    step("record-stop", "遅れの録画を終了する", { stream: false, tailSeconds: 0, label: LATENCY_RECORDING_LABEL }),
    step("remove-window-capture", "遅れ: Vesktop のウィンドウのキャプチャ (ウィンドウ・画面のどちらでも) を外す (この後の録画と PNG に Vesktop のウィンドウを入れない)", { inputName: LATENCY_INPUT_NAME }),
  ];
}

// sync.py の結果 (judge の記録。recording の label で引く) に合否の基準を当てる手順 (記録するだけ)。person を渡すと、その人の枠
// (テスト動画を共有している人。枠は OBS の記録の枠の割り当てと cordmux の録画の録画ログから引く) だけを判定する。合成のソースの音は
// 全員の音を混ぜた 1 本なので、ほかの人の枠に白が写っていても (画面全体の共有にテスト動画のウィンドウが写る)、その組は別の経路の映像と
// テスト動画の音の差で、映像と音のずれではない。
function avSyncJudgeStep(recording, person) {
  return step(
    "judge-av-sync",
    "合否の基準: " + recording + " の sync.py の" + (person ? " " + person + " の枠の" : "") + "平均のずれ (音が先 " + AV_SYNC_AUDIO_LEAD_LIMIT_MS + " ms 未満・音が遅れ " +
      AV_SYNC_AUDIO_LAG_LIMIT_MS + " ms 未満) と、一時的な段を除いた傾き (" + AV_TREND_DEFECT_MS_PER_MIN + " ms/分を超えて 0 と分けられれば欠陥)。記録するだけ",
    { recording, ...(person ? { person } : {}) },
  );
}

// (e) 連続運転。5 分周期の照合が正常に 6 周する間、ドックに上書きのメッセージがないことを周期ごとに読み、
// そのあとでわざと 1 ファイルに 1 バイト足してメッセージと [再インストール] が出ること、元に戻して消えることを見る。足す前に
// コピーを取り、戻すのはコピーから (untamper-file。途中で抜けた経路でも finally が戻す)。
// **メッセージはファイルの名前を出さない** (rules_dock.cpp の VencordOverwritten は見出しとボタンだけ)。
// 足したファイルの名指しは記録 (プラグインのログ) の側で、
// 画面の判定は見出しとボタンが出ること。
function callContinuousSteps(args) {
  const steps = [];
  for (let i = 1; i <= CALL_CHECK_CYCLES; i += 1) {
    steps.push(step("wait", "照合の周期を 1 つ待つ (" + i + "/" + CALL_CHECK_CYCLES + ")", { ms: CALL_CHECK_PERIOD_MS }));
    steps.push(step("collect-plugin-log", "プラグインの記録の回収 (照合 " + i + "/" + CALL_CHECK_CYCLES + ")"));
    steps.push(dockTextsStep("check-cycle-" + i));
    steps.push(noticeJudgeStep(false, TEXT_ID_VENCORD_OVERWRITTEN, "check-cycle-" + i, true, "照合 " + i + "/" + CALL_CHECK_CYCLES + ": 正常な間は上書きのメッセージがない"));
  }
  steps.push(
    step("tamper-file", "テスト用にインストールした data/vencord/" + CALL_TAMPER_FILE + " に 1 バイト足す (コピーを取ってから。わざと異常)", {
      name: CALL_TAMPER_FILE,
      dirRel: BUNDLED_VENCORD_REL,
      obs: args.obs,
      keepCopy: true,
      restoreId: "call-tamper",
      gate: true,
    }),
    step("wait", "照合の周期が回ってメッセージが出るまで (" + CALL_TAMPER_WAIT_MS / 60000 + " 分)", { ms: CALL_TAMPER_WAIT_MS }),
    step("screenshot-window", "上書きのメッセージが出ているドックを PNG に撮る", { target: "first", shotLabel: "tamper-warned" }),
    step("collect-plugin-log", "プラグインの記録の回収 (わざと異常の後)"),
    dockTextsStep("tamper-warned", { buttonTextId: TEXT_ID_REINSTALL, expect: "button", gate: true }),
    noticeJudgeStep(true, TEXT_ID_VENCORD_OVERWRITTEN, "tamper-warned", true, "足したファイルがあると上書きのメッセージが出る"),
    step("untamper-file", "足す前のコピーから " + CALL_TAMPER_FILE + " を元に戻す", { restoreId: "call-tamper", gate: true }),
    step("wait", "照合の周期が回ってメッセージが消えるまで (" + CALL_TAMPER_WAIT_MS / 60000 + " 分)", { ms: CALL_TAMPER_WAIT_MS }),
    step("screenshot-window", "元に戻した後のドックを PNG に撮る", { target: "first", shotLabel: "tamper-restored" }),
    step("collect-plugin-log", "プラグインの記録の回収 (元に戻した後)"),
    dockTextsStep("tamper-restored", { buttonTextId: TEXT_ID_REINSTALL, expect: "no-button", gate: true }),
    noticeJudgeStep(false, TEXT_ID_VENCORD_OVERWRITTEN, "tamper-restored", true, "元に戻すと上書きのメッセージが消える"),
  );
  return steps;
}

// 実際のアバターの並び (judgeRosterLabels のコメント) を撮るかどうか。--extra-checks か --recheck の実際の call-1 だけ (リハーサルのテスト用の送信側は
// アバターを送らず、共有も止めない)。
export function rosterCheckPlanned(args) {
  return Boolean(args && args.step === "call-1" && (args.extraChecks || args.recheck) && !args.avOnly && !args.rehearsal);
}

// 共有を止めてもらう -> 並びが描かれるまで待つ -> cordmux のソースの絵と全体の PNG を撮る -> 並びの名前ラベルの行を数える。
// 通話から抜ける前なので、画面共有していない参加者が全員、実際のアバターで並ぶはず。期待の人数は rosterExpectedPeople。
function rosterCheckSteps(args) {
  const expectedPeople = rosterExpectedPeople(args);
  return [
    step("count-roster-labels", "共有を止めてもらう前の、並びの名前ラベルの行 (Composite roster label ready) の数を保存する", { label: ROSTER_BASELINE_LABEL }),
    humanStep(VESKTOP_STOP_SHARING_MESSAGE, "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", "共有をやめた人が並びに移り、名前ラベルが描かれるまでの猶予", { ms: ROSTER_LABEL_SETTLE_MS }),
    step("screenshot-source", "実際のアバターの並び: 合成のソースの絵を PNG に撮る (共有していない参加者が実際のアバターで並ぶ)", { source: { kind: "composite" }, label: ROSTER_SHOT_LABEL }),
    step("screenshot-window", "実際のアバターの並び: ドックを PNG に撮る (参加者の行)", { target: "first", shotLabel: ROSTER_SHOT_LABEL }),
    step("judge-roster-labels", "並びの名前ラベルの行が、共有を止めた後に出たか (並んだ人数を期待の人数 " + expectedPeople + " と比べて記録するだけ)", {
      baseline: ROSTER_BASELINE_LABEL,
      expectedPeople,
    }),
  ];
}

// (f) 締めのアンインストール。実際の通話では人が押し、実行スクリプトは state.json がバックアップの previous に戻るまで待つ。人への文は
// 手順書 (guide/ja.md の「アンインストール」の手順 2) と同じ道: OBS の [ツール] -> 「cordmuxをアンインストール...」->
// 確認のウィンドウ -> 結果のウィンドウ。リハーサルは
// 実行スクリプトがもう 1 つの入口 (設定のページのボタン) で押す (uninstallSteps の entry)。
function callUninstallSteps(args) {
  // アンインストールはバックアップ (vencord_dir_backup.json) を消すので、押す前に中身を読んでおく (judge-vencord-dir-restored の比べる相手)。
  const capture = step("capture-backup", "製品のバックアップ (" + VENCORD_DIR_BACKUP_REL + ") をアンインストールの前に読んでおく (押すと消えるため)", {
    backupPath: vencordDirBackupPath(args),
    gate: true,
  });
  if (args.rehearsal) {
    return [capture, ...uninstallSteps("rehearsal", REHEARSAL_UNINSTALL_DONE_TIMEOUT_MS, { entry: "settings" })];
  }
  // --av-only: 人に頼むのは、通話から抜けて Vesktop を終了することだけ。アンインストールは実行スクリプトが押す (リハーサルと同じ入口)。
  // 手順書の道を人が押す確認は --recheck の締めにあり、ここで確かめたいのは映像と音のずれなので、人の操作を増やさない。
  if (args.avOnly) {
    return [
      { ...humanStep(AV_ONLY_LEAVE_AND_QUIT_MESSAGE, "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S), quitVesktop: true },
      // 製品は Vesktop が起動している間はアンインストールの確認を開かない。接続が切れている間は Vesktop の終了を毎秒見る
      // (dock_poll.cpp の vesktop_quit_check_due) が、返事の時点で Vesktop のプロセスがまだ終わりきっていないことがあるので、少し待ってから押す。
      step("wait", "Vesktop の終了がドックに届くまでの猶予 (アンインストールを押す前)", { ms: AV_ONLY_QUIT_SETTLE_MS }),
      capture,
      ...uninstallSteps("av-only", ROUNDTRIP_UNINSTALL_DONE_TIMEOUT_MS, { entry: "settings" }),
    ];
  }
  // 製品は Vesktop が動いている間はアンインストールできない (settings の uninstall_vesktop_running) ので、
  // 先に Vesktop を終了してもらう。Vesktop は終了時に state.json を
  // 書き直すので、この順を守らないとアンインストールで戻した値が上書きされる恐れもある (call-gate-b の存在理由と同じ)。
  // ウィンドウの閉じるボタンではトレイに残って Vesktop.exe が動き続け、製品はそれを名前で見て断るので、文は通知領域の [Quit] を言う
  // (VESKTOP_HANG_UP_AND_QUIT_MESSAGE のコメント)。
  const split = rosterCheckPlanned(args);
  return [
    ...(split ? rosterCheckSteps(args) : []),
    { ...humanStep(split ? VESKTOP_LEAVE_AND_QUIT_MESSAGE : VESKTOP_HANG_UP_AND_QUIT_MESSAGE, "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S), quitVesktop: true },
    capture,
    humanStep(
      "テスト用のOBSの [" + textRef(OBS_TEXT_TOOLS_MENU) + "] メニューから [" + textRef(TEXT_ID_TOOLS_MENU_UNINSTALL) +
        "] を選んでください。確認のウィンドウで [" + textRef(TEXT_ID_DIALOG_UNINSTALL_BUTTON) + "]、結果のウィンドウで [" + textRef(OBS_TEXT_OK) +
        "] を押してください。確認のウィンドウが出ないときは、開いた [" + textRef(TEXT_ID_SETTINGS_PAGE_ABOUT) +
        "] ページのボタンの下に出ている理由を、そのまま伝えてください",
      "state-json-restored",
      HUMAN_UNINSTALL_TIMEOUT_MS,
      HUMAN_ALLOWANCE_UNINSTALL_S,
      {
        backupPath: vencordDirBackupPath(args),
        gate: true,
      },
    ),
  ];
}

// アンインストールの入口。
//   tools: OBS の [ツール] -> 「cordmuxをアンインストール...」。押すと設定のウィンドウが [バージョン情報] で開き、押せる状態なら
//          確認のウィンドウがその上に出る (guide/ja.md の「アンインストール」の手順と同じ道)。
//   settings: ドックの [...] -> 「cordmuxの設定」-> [バージョン情報] -> [cordmuxをアンインストール...]。
// install-roundtrip は tools (手順書が案内する道)、リハーサルは settings で押す。2 つの入口を
// 実行のどれかで 1 度は通すため (両方 tools にするとページのボタンの道を実行スクリプトが 1 度も押さなくなり、
// 両方 settings にすると手順書の道を機械で 1 度も通さない)。実際の通話では人が手順書の道 (tools) で押す。
export const UNINSTALL_ENTRY_VALUES = ["tools", "settings"];

// [ツール] の項目から確認のウィンドウまで (3 手)。[ツール] は下にメニューを持つ項目 (dialog-click.ps1 は Expand で開く)。
// メニューバーは OBS のメインウィンドウにありタイトルがバージョンと実行で変わるので、メニューと同じくタイトルを問わずに探す (MENU_POPUP)。
// 項目は押すと確認のウィンドウ (QDialog::exec) まで戻らないので noWait。続けて設定のウィンドウを読むだけの 1 手 (押さない。gate に
// しない) で、開いたページと、確認のウィンドウが出ないときにボタンの下に出る理由のテキストを残す (状態の確認が済む前に押した
// ときの切り分け。製品は押せない状態なら確認のウィンドウを出さず、理由を見せる)。
function toolsMenuUninstallSteps(labelPrefix) {
  return [
    uiClickStep("OBS のメニューバーの [" + OBS_TEXT_TOOLS_MENU + "] を開く (" + labelPrefix + ": アンインストール)", MENU_POPUP, OBS_TEXT_TOOLS_MENU, {
      controlType: "MenuItem",
      noWait: true,
      gate: true,
    }),
    uiClickStep("[" + OBS_TEXT_TOOLS_MENU + "] の [" + TEXT_ID_TOOLS_MENU_UNINSTALL + "] を選ぶ (設定のウィンドウと確認のウィンドウが開く)", MENU_POPUP, TEXT_ID_TOOLS_MENU_UNINSTALL, {
      controlType: "MenuItem",
      noWait: true,
      gate: true,
    }),
    step("dialog-click", "cordmux の設定のウィンドウを読む (押さない。[" + TEXT_ID_SETTINGS_UNINSTALL + "] のページで開いたか、ボタンの下の理由のテキスト)", {
      target: "first",
      ...SETTINGS_WINDOW,
      buttonTextId: TEXT_ID_SETTINGS_UNINSTALL,
      expect: "button",
      listOnly: true,
      timeoutMs: UI_FIND_TIMEOUT_MS,
      label: "uninstall-entry-page",
    }),
  ];
}

// アンインストールを実行スクリプトが押す手順 (リハーサルと install-roundtrip で共通)。
// 確認のウィンドウの [アンインストール] は、押すとアンインストールが走り、続けて結果のウィンドウ (同じタイトル) が開くまで Invoke が戻らない
// (QDialog::exec) ので noWait。結果のウィンドウは [フォルダーを開く] と [OK] の 2 つのボタンを持つので、[OK] を ini の文字列
// (OBS 自身の ini) で名指す。場所の行は長いと読むだけの入力欄になるので、dialog-click.ps1 は Edit の名前と値も文として返す。
// 結果のウィンドウの文は label (uninstall-done) で判定へ渡る (install-roundtrip の judge-rt)。opts.entry は入口 (上の
// UNINSTALL_ENTRY_VALUES。既定は tools)。
function uninstallSteps(labelPrefix, doneTimeoutMs, opts) {
  const o = opts || {};
  const entry = o.entry === "settings" ? "settings" : "tools";
  const entrySteps =
    entry === "settings"
      ? [
          ...settingsPageSteps(TEXT_ID_SETTINGS_PAGE_ABOUT, labelPrefix + ": アンインストール", { gate: true }),
          uiClickStep("[" + TEXT_ID_SETTINGS_UNINSTALL + "] を押す (確認のウィンドウが開く)", SETTINGS_WINDOW, TEXT_ID_SETTINGS_UNINSTALL, { noWait: true, gate: true }),
        ]
      : toolsMenuUninstallSteps(labelPrefix);
  return [
    ...entrySteps,
    uiClickStep("確認のウィンドウで [" + TEXT_ID_DIALOG_UNINSTALL_BUTTON + "] を押す", UNINSTALL_DIALOG, TEXT_ID_DIALOG_UNINSTALL_BUTTON, {
      noWait: true,
      timeoutMs: ROUNDTRIP_DIALOG_TIMEOUT_MS,
      label: "uninstall-confirm",
      gate: true,
    }),
    step("dialog-click", "結果のウィンドウの文を読む (削除するフォルダーと録画の場所)", {
      target: "first",
      ...UNINSTALL_DIALOG,
      buttonTextId: OBS_TEXT_OK,
      expect: "button",
      listOnly: true,
      timeoutMs: doneTimeoutMs,
      label: "uninstall-done",
      ...(o.gateResult ? { gate: true } : {}),
    }),
    uiClickStep("結果のウィンドウの [" + OBS_TEXT_OK + "] を押して閉じる", UNINSTALL_DIALOG, OBS_TEXT_OK, { label: "uninstall-close" }),
  ];
}

// 実行計画が見込む待ちの合計 (分)。数えるのは wait の ms・human の waitSeconds (人の操作の見込み。until が seconds の
// ときは実際の待ち)・record-stop の tailSeconds・measure-input-level の seconds。時限 (timeoutMs) や条件待ち (wait-connection / wait-finalize /
// harness-wait) は数えない。小数 1 桁。
export function plannedMinutes(steps) {
  return Math.round((plannedSeconds(steps) / 60) * 10) / 10;
}

// 実行計画の見込みが実行の期限 (deadlineMin) に足りない分 (分。足りていれば 0)。期限になると run-window.mjs は残りの
// 手順を打ち切るので、--dry-run の出力に出して呼び出し行の --deadline-min の不足を当日の前に見つける。
export function deadlineShortfallMinutes(steps, deadlineMin) {
  const shortfall = plannedMinutes(steps) - (Number(deadlineMin) || 0);
  return shortfall > 0 ? Math.round(shortfall * 10) / 10 : 0;
}

function stepPlannedSeconds(s) {
  if (!s) return 0;
  if (s.kind === "wait") return (Number(s.ms) || 0) / 1000;
  if (s.kind === "human") return Number(s.waitSeconds) || 0;
  if (s.kind === "record-stop") return Number(s.tailSeconds) || 0;
  // 音量メーターを読む長さ (実際にその秒数を待つ)。
  if (s.kind === "measure-input-level") return Number(s.seconds) || 0;
  return 0;
}

function plannedSeconds(steps) {
  return (steps || []).reduce((sum, s) => sum + stepPlannedSeconds(s), 0);
}

// 見込みの分の累計が marks (分) を初めて超えた手順の直後に obs-stats (label "t<分>min") を差し込む。OBS のメモリと
// 欠けを 30 分ごとに取る位置を、手順の並びから機械で決めるため。届かない mark は差し込まない。
export function insertStatsAtMinutes(steps, marks) {
  const list = steps || [];
  const pending = [...(marks || [])].sort((a, b) => a - b);
  const out = [];
  let cumulative = 0;
  for (const s of list) {
    out.push(s);
    cumulative += stepPlannedSeconds(s);
    while (pending.length > 0 && cumulative >= pending[0] * 60) {
      const mark = pending.shift();
      out.push(step("obs-stats", "GetStats を取る (見込み " + mark + " 分の位置。OBS のメモリと欠け)", { label: "t" + mark + "min" }));
    }
  }
  return out;
}

// --rehearsal の置き換え (純関数)。human は until "seconds" (REHEARSAL_HUMAN_WAIT_SECONDS) に、1 分以上の wait は
// REHEARSAL_TIME_FACTOR で縮める (数秒の落ち着きの待ちはそのまま)。
export function rehearseSteps(steps) {
  return (steps || []).map((s) => {
    if (s.kind === "human") return { ...s, until: "seconds", waitSeconds: REHEARSAL_HUMAN_WAIT_SECONDS };
    if (s.kind === "wait" && Number(s.ms) >= REHEARSAL_SCALE_MIN_MS) return { ...s, ms: Math.round(Number(s.ms) * REHEARSAL_TIME_FACTOR), rehearsalScaled: true };
    return s;
  });
}

// 本体 (定常状態の確認と、参加者の行のチェック・名前ラベルの設定・声のミュートの確認) の後、締めの前までの段の並び。
// 参加と共有の後に、合成のソースを置いて人が枠に載った絵を残すところまで (callBodySteps と callRecheckBodySteps が共有する)。
function callJoinedPictureSteps(args) {
  const screens = callScreens(args);
  const voices = callVoices(args);
  const steps = [];
  steps.push(wireCompositeStep("画面 " + screens + " 本を 4 等分、声 " + voices + " 本は合成のソースの音"));
  steps.push(step("wait", "参加者一覧に人が並ぶまでの猶予", { ms: SELECT_FOUR_SETTLE_WAIT_MS }));
  // 枠の割り当ては自動 (buildCallOneSteps のコメント) なので、ここでは押さずに、載ったことの証拠 (ドックの PNG と
  // シーンの項目一覧) だけを残す。手動で付け外しする形は callAc30Steps と select-four にある。
  steps.push(step("screenshot-window", "自動で枠に載った後のドックを PNG に撮る (並んでいる行とチェックの証拠)", { target: "first", shotLabel: "people-before-slot" }));
  steps.push(step("wait", "自動配置の周期が回るまでの猶予", { ms: SELECT_FOUR_SETTLE_WAIT_MS }));
  steps.push(step("screenshot-window", "自動配置の後のドックを PNG に撮る", { target: "first", shotLabel: "people-after-slot" }));
  steps.push(compositeSnapshotStep("slot-items", "枠に載った後"));
  return steps;
}

// ---- --recheck の足した確認 (バージョンの表示・画面共有の音・解像度の変化・エンコードの設定・自分の枠・受け取りの継続・Vesktop の抜け戻り) ----
// どれも実際の通話でしか答えられない物で、記録するだけ (gate にしない)。メインの手順の録画と配信は行わない (callClosingSteps)。
// 記録の行を数える手順は judge-log-lines (judgeLogLines)。前の手順が保存した行数 (since) から後の行だけを数えるので、
// 数えない手順 (checks が空) を行数のバックアップに使う。
// 画面共有の音: ミュートを外してから、入力の音量メーターを読むまでの猶予と、読む長さ (InputVolumeMeters は 50 ms ごとのメッセージ)。
// しきい値は、無音 (ミュートの間は -inf) と区別できる大きさにした値。
export const SCREEN_AUDIO_SETTLE_MS = 5000;
export const INPUT_LEVEL_SECONDS = 10;
export const INPUT_LEVEL_THRESHOLD_DBFS = -60;
export const SCREEN_AUDIO_LEVEL_LABEL = "screen-audio";
// 共有の途中の解像度の変化: 1 つの録画 (配信なし) の中で Chrome のウィンドウを縮め、元の大きさへ戻す。縮め先の大きさ (物理ピクセル。
// 1280x720 を目安に 16:9 のまま縮めた値)。縮めた後に送信側が確保し直す猶予と、戻した後の猶予。
export const RESCHANGE_RECORDING_LABEL = "reschange";
export const RESCHANGE_BROWSER_PROCESS = "chrome";
export const RESCHANGE_SMALL_WIDTH = 800;
export const RESCHANGE_SMALL_HEIGHT = 450;
export const RESCHANGE_LEAD_MS = 5000;
export const RESCHANGE_SETTLE_MS = 20000;
export const RESCHANGE_TAIL_MS = 10000;
// 解像度の変化の録画 (frames.py の結果) で許す、いちばん長い静止 (ms)。
export const RESCHANGE_MAX_FREEZE_MS = 1000;
// エンコードの設定: [適用] の後に送信側へ届くまでの猶予と、共有を始め直した後に送信側が確保し直すまでの猶予。
export const ENCODE_APPLY_SETTLE_MS = UI_APPLY_SETTLE_MS;
export const ENCODE_SENDER_SETTLE_MS = 20000;
// 自分の枠: 自分の枠に映すソースを作るシーンとソース (別のシーンに置く = 録画に出ない。ソースは色のソース)。共有の前後の読みの猶予。
export const SELF_FRAME_SCENE_NAME = "self-frame-test";
export const SELF_FRAME_SOURCE_NAME = "self-test";
export const SELF_FRAME_SOURCE_KIND = "color_source_v3";
export const SELF_FRAME_SETTLE_MS = 10000;
// ドックの自分の行のチェックを押してから、枠が変わって記録に出るまでの猶予。
export const SELF_FRAME_DOCK_SETTLE_MS = 5000;
// 自分の枠の人の行 (メインのアカウント)。行のチェックの読み上げの名前は、人の見えている名前。
// スピーカーミュートと最小化の間に受け取りが続くかを読むまでの待ち、Vesktop が抜けた後・戻った後の猶予。
export const MUTE_MINIMIZE_HOLD_MS = 60000;
export const VESKTOP_LEFT_SETTLE_MS = 20000;
export const VESKTOP_REJOIN_SETTLE_MS = 30000;
// ログの行のパターン (judge-log-lines の checks の pattern。正規表現の元の文字列)。製品のログの文字列は dock_poll.cpp・pipeline.cpp・
// 送信側の src-vencord/encode/limits.ts の stageLogLine と同じ。
export const LOG_PATTERN_LIMITS_SENT = "Quality limits sent to Vesktop \\(.*encoder=software latency=quality\\)";
export const LOG_PATTERN_SENDER_CHOICE = "video stream \\d+: stage=\\S+ format=\\S+ preference=prefer-software latencyMode=quality";
export const LOG_PATTERN_SELF_ON = "Self frame turned on: dock";
// 自動 (枠に載せる人の選び方) で、メインアカウントが画面共有を始めてチェックが付いた行 (dock_poll.cpp の step_self_frame)。
export const LOG_PATTERN_SELF_ON_SHARE = "Self frame turned on: screen share";
export const LOG_PATTERN_SELF_OFF = "Self frame turned off: dock";
// 外れた理由を問わないパターン (共有を止めても外れないことを見る。dock_poll.cpp の step_self_frame と set_self_frame)。
export const LOG_PATTERN_SELF_OFF_ANY = "Self frame turned off";
export const LOG_PATTERN_CANVAS_COUNT = "Composite layout canvas=\\S+ count=(\\d+)";
export const LOG_PATTERN_CANVAS_SELF = "Composite layout canvas=\\S+ count=\\d+ self=";
export const LOG_PATTERN_SCREEN_AUDIO_PLACED = "Stats mixer_track screen_audio slot=\\d+ placed_frames=([1-9]\\d*)";

// ログの行を数える 1 ステップ。label はこのステップの結果の名前 (後のステップの since と delta.from が参照する)、opts.since は行数を保存した前のステップの label、
// opts.checks は judgeLogLines の checks (空なら行数のバックアップだけ)。
function judgeLogLinesStep(label, what, opts) {
  const o = opts || {};
  return step("judge-log-lines", what, { label, ...(o.since ? { since: o.since } : {}), checks: o.checks || [] });
}

// バージョンの表示の後に置く、画面共有の音の確認: 声をみんなミュートにする -> ミュートを外す前のログの行数を保存する -> 画面共有の音のミュートを外す ->
// 入力の音量メーターを読む -> ログに画面共有の音を混ぜた行があるか -> ミュートに戻す -> 声のミュートを戻す。共有の音が届く Chrome (実機の実行では Chrome のアカウント、
// リハーサルではマーカーの音を乗せたテスト用の画面)。声を切るのは、合成のソースの入力のメーターが声の音も含むため (Chrome のテスト用のマイクの 1 kHz が
// 混ざると、画面共有の音が届かなくても peak がしきい値を超える。callAvSteps と同じ)。
// ほかの人の画面共有の音もミュートにする (メーターは合成のソースの全部の音なので、ほかの人の共有の音が届くと、確かめる人の音が
// 届かなくても peak がしきい値を超える。callAvSteps と同じ)。
function screenAudioSteps(args) {
  const account = args.rehearsal ? REHEARSAL_AV_SCREEN_ACCOUNT : callAccounts(args).chrome;
  const everyone = callVoicePersons(args);
  const others = callScreenSharers(args).filter((p) => p !== account);
  return [
    peopleTabStep("画面共有の音: 行のミュートを押す前"),
    ...voiceIsolateSteps(everyone, null, "画面共有の音: 声をミュートにして、メーターに声の音を混ぜない"),
    ...others.map((p) => rowMuteStep("screen", p, "on", "画面共有の音: ほかの人の画面共有の音をメーターに混ぜない", { absentOk: true })),
    judgeLogLinesStep("sa-before", "画面共有の音: ミュートを外す前の記録の行数を保存する"),
    rowMuteStep("screen", account, "off", "画面共有の音: ミュートを外して合成のソースの音に届くかを見る"),
    step("wait", "画面共有の音が合成のソースの音に混ざり始めるまでの猶予", { ms: SCREEN_AUDIO_SETTLE_MS }),
    step("measure-input-level", "合成のソースの入力の音量メーターを " + INPUT_LEVEL_SECONDS + " 秒読み、peak が " + INPUT_LEVEL_THRESHOLD_DBFS + " dBFS より大きいか (記録するだけ)", {
      inputName: COMPOSITE_INPUT_NAME,
      seconds: INPUT_LEVEL_SECONDS,
      thresholdDbfs: INPUT_LEVEL_THRESHOLD_DBFS,
      label: SCREEN_AUDIO_LEVEL_LABEL,
    }),
    step("collect-plugin-log", "プラグインの記録の回収 (画面共有の音のミュートを外した後)"),
    judgeLogLinesStep("sa-after", "画面共有の音: 記録に、画面共有の音を混ぜた行 (screen_audio の placed_frames が 1 以上) があるか", {
      since: "sa-before",
      checks: [{ id: "placed", pattern: LOG_PATTERN_SCREEN_AUDIO_PLACED, min: 1 }],
    }),
    rowMuteStep("screen", account, "on", "画面共有の音: ミュートに戻す"),
    ...voiceRestoreSteps(everyone, "画面共有の音: 声のミュートを戻す"),
  ];
}

// 共有の途中の解像度の変化を実行スクリプトが起こせる実行か (実機の実行だけ。リハーサルのテスト用の送信側には Chrome のウィンドウがない)。
export function resolutionChangePlanned(args) {
  return Boolean(args && args.step === "call-1" && args.recheck && !args.avOnly && !args.rehearsal);
}

// 共有の途中の解像度の変化: 録画を始め -> Chrome のウィンドウを縮める -> 猶予 -> 画像と配置 -> 元の大きさへ戻す -> 猶予 -> 録画を止める。録画の欠落・静止は OBS を閉じた後に frames.py が読む (callRecheckAnalysisSteps)。
// ウィンドウが見つからない (resize-window が ok:false) ときは失敗として記録し、先へ進む (実行計画は静的で、見つからなければ人に依頼する分岐は持てない)。
function resolutionChangeSteps() {
  return [
    step("record-start", "解像度の変化の録画を始める (配信なし)", { stream: false, label: RESCHANGE_RECORDING_LABEL }),
    step("wait", "録画の頭の落ち着き", { ms: RESCHANGE_LEAD_MS }),
    step("resize-window", "Chrome のウィンドウを " + RESCHANGE_SMALL_WIDTH + "x" + RESCHANGE_SMALL_HEIGHT + " に縮める (動かさない・前面に出さない)", {
      processName: RESCHANGE_BROWSER_PROCESS,
      mode: "set",
      width: RESCHANGE_SMALL_WIDTH,
      height: RESCHANGE_SMALL_HEIGHT,
      label: RESCHANGE_RECORDING_LABEL,
    }),
    step("wait", "共有の解像度が変わり、送信側が確保し直すまでの猶予", { ms: RESCHANGE_SETTLE_MS }),
    step("collect-plugin-log", "プラグインの記録の回収 (ウィンドウを縮めた後)"),
    step("screenshot-source", "解像度の変化: 合成のソースの絵を PNG に撮る (ウィンドウを縮めた後)", { source: { kind: "composite" }, label: "reschange-shrunk" }),
    compositeSnapshotStep("reschange-shrunk", "ウィンドウを縮めた後", { namesUnknown: true }),
    step("resize-window", "Chrome のウィンドウを元の大きさへ戻す", { processName: RESCHANGE_BROWSER_PROCESS, mode: "restore", label: RESCHANGE_RECORDING_LABEL }),
    step("wait", "元の大きさに戻った共有が枠に乗るまでの猶予", { ms: RESCHANGE_TAIL_MS }),
    step("record-stop", "解像度の変化の録画を終了する", { stream: false, tailSeconds: 0, label: RESCHANGE_RECORDING_LABEL }),
  ];
}

// ドックの [表示] タブか設定ウィンドウの ComboBox の項目を選ぶ 1 ステップ (dialog-click.ps1 の -ControlType ComboBox -Select)。comboTextId は入力欄のラベルのテキスト、
// select はその ComboBox の中の項目のテキストで、ini の id (selectTextId) か、利用者の名前のようなテキストそのまま (selectText)。
function comboSelectStep(what, comboTextId, select) {
  const where = comboTextId === TEXT_ID_LOOK_SELF_FRAME_SOURCE ? DOCK_WINDOW : SETTINGS_WINDOW;
  return uiClickStep(what, where, comboTextId, { controlType: "ComboBox", ...select });
}

// エンコードの設定 (映像エンコーダ「ソフトウェア」と優先する項目「高品質」): 設定ウィンドウの [映像の受信] で選んで [適用] -> ログに
// 送信側へ送った行 -> Firefox のアカウントの共有を始め直してもらう -> 送信側の確保の行。
function encodeSettingsSteps(args) {
  return [
    judgeLogLinesStep("encode-before", "符号化の設定: 替える前の記録の行数を保存する"),
    ...settingsPageSteps(TEXT_ID_SETTINGS_PAGE_INCOMING_VIDEO, "符号化の設定を替える"),
    comboSelectStep("[" + TEXT_ID_SETTINGS_VIDEO_ENCODER + "] で [" + TEXT_ID_SETTINGS_VIDEO_ENCODER_SOFTWARE + "] を選ぶ", TEXT_ID_SETTINGS_VIDEO_ENCODER, {
      selectTextId: TEXT_ID_SETTINGS_VIDEO_ENCODER_SOFTWARE,
    }),
    comboSelectStep("[" + TEXT_ID_SETTINGS_ENCODE_PRIORITY + "] で [" + TEXT_ID_SETTINGS_ENCODE_PRIORITY_QUALITY + "] を選ぶ", TEXT_ID_SETTINGS_ENCODE_PRIORITY, {
      selectTextId: TEXT_ID_SETTINGS_ENCODE_PRIORITY_QUALITY,
    }),
    step("screenshot-window", "選んだ後の設定のウィンドウを PNG に撮る (符号化の設定)", { target: "first", shotLabel: "encode-chosen" }),
    settingsButtonStep(OBS_TEXT_APPLY, "符号化の設定を反映する"),
    step("wait", "反映されて送信側へ送られるまでの猶予", { ms: ENCODE_APPLY_SETTLE_MS }),
    settingsButtonStep(OBS_TEXT_OK, "設定のウィンドウを閉じる"),
    step("collect-plugin-log", "プラグインの記録の回収 (符号化の設定を反映した後)"),
    judgeLogLinesStep("encode-limits", "符号化の設定: 記録に、ソフトウェア・高品質を送信側へ送った行があるか", {
      since: "encode-before",
      checks: [{ id: "limits-sent", pattern: LOG_PATTERN_LIMITS_SENT, min: 1 }],
    }),
    humanStep(encodeRestartShareMessage(args.voiceChannel, callAccounts(args)), "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHARE_S),
    step("wait", "共有が始まり直して、送信側が符号化の設定で確保し直すまでの猶予", { ms: ENCODE_SENDER_SETTLE_MS }),
    step("collect-plugin-log", "プラグインの記録の回収 (共有を始め直した後)"),
    args.rehearsal
      ? rehearsalSkipStep("encode-choice", "符号化の設定: 送信側がソフトウェア・高品質で確保し直したか")
      : judgeLogLinesStep("encode-choice", "符号化の設定: 記録に、送信側がソフトウェア・高品質で確保した行があるか", {
          since: "encode-before",
          checks: [{ id: "sender-choice", pattern: LOG_PATTERN_SENDER_CHOICE, min: 1 }],
        }),
  ];
}

// 自分の枠 (ほかの人の行と同じ動き)。call-1 は枠に載せる人の選び方を「自動」にする (buildCallOneSteps) ので、確かめる動きは docs/spec.md の
// 「自分の枠」の自動のときの決まり: メインアカウントが画面共有している間はチェックが付く (自分の枠が出る)、共有を止めても外れない、
// ドックで外すと消え、付けると出る。並び:
//   自分の枠に映すソースを作って選ぶ -> 共有の前の読み (self-before) -> メインアカウントに共有を始めてもらう (自動でチェックが付き、枠が 1 つ増えて
//   self= が入る。self-share) -> 共有を止めてもらう (チェックも自分の枠も残る。self-stopped) -> ドックで自分の行のチェックを外す (消えて元の数。self-off)
//   -> 付ける (出て 1 つ増える。self-during) -> 外す (元の数。self-after)。
// 枠の数は「Composite layout」の行の count で読む (配置の行は変わったときだけ出るので、変わらないことは「新しい行がない」で読む。メインアカウントの
// 共有そのものは、OBS のソースを選んでいる間は枠に載らない)。行を押す前に、ドックの [参加者] タブを開く ([表示] タブで映像を選んだ後なので、
// 開かないと行のチェックが見つからない)。
// リハーサルは、テスト用の送信側の声だけの人をメインアカウントにする (mainAccountFor)。その人は共有を始めも止めもしないので、共有による
// 付け外し (self-share・self-stopped) と、共有で付いたチェックを外す読み (self-off) は判定せず (REHEARSAL_NOT_JUDGED)、行数の保存だけを通す。
// ドックのチェックの付け外し (self-during・self-after) は判定する。
function selfFrameSteps(args) {
  const main = mainAccountFor(args);
  if (main === null) return [rehearsalSkipStep("self-frame", "自分の枠", { reason: REHEARSAL_NO_MAIN_REASON })];
  const reads = (label, note) => [
    compositeSnapshotStep(label, note, { namesUnknown: !args.rehearsal }),
    dockTextsStep(label),
    step("screenshot-window", "自分の枠: " + note + "のドックを PNG に撮る", { target: "first", shotLabel: label }),
  ];
  const selfRowCheck = (desired, what) => [
    peopleTabStep("自分の枠: 自分の行のチェックを押す前"),
    step("dock-click", "参加者の行「" + main + "」のチェックを" + (desired === "off" ? "外す" : "付ける") + " (自分の枠: " + what + ")", {
      target: "first",
      name: main,
      index: 0,
      desired,
    }),
  ];
  // リハーサルで判定しない読み: 次の読みの since のために行数だけを保存し、判定しないことを skip で残す (judgeLinesSummary は part の分を外す)。
  const judgeRead = (label, what, opts) =>
    args.rehearsal && opts.notInRehearsal
      ? [judgeLogLinesStep(label, what + " (行数の保存だけ)", { since: opts.since }), rehearsalSkipStep("self-frame", "自分の枠: " + what, { part: label, reasonKey: "self-frame-share" })]
      : [judgeLogLinesStep(label, what, { since: opts.since, checks: opts.checks })];
  const unchanged = { id: "layout-unchanged", pattern: LOG_PATTERN_CANVAS_COUNT, min: 0, max: 0 };
  const countIs = (delta) => ({ id: "canvas-count", pattern: LOG_PATTERN_CANVAS_COUNT, min: 1, group: 1, delta: { from: "self-before", equals: delta } });
  const selfIn = { id: "canvas-self", pattern: LOG_PATTERN_CANVAS_SELF, min: 1 };
  return [
    step("obsws-request", "自分の枠: 録画に出ない別のシーンを作る", { request: "CreateScene", requestData: { sceneName: SELF_FRAME_SCENE_NAME }, label: "self-frame-scene" }),
    step("obsws-request", "自分の枠: そのシーンに映すソース (色のソース) を作る", {
      request: "CreateInput",
      requestData: { sceneName: SELF_FRAME_SCENE_NAME, inputName: SELF_FRAME_SOURCE_NAME, inputKind: SELF_FRAME_SOURCE_KIND, inputSettings: {}, sceneItemEnabled: true },
      label: "self-frame-source",
    }),
    ...displayTabSteps("自分の枠に映す映像を選ぶ"),
    comboSelectStep("[" + TEXT_ID_LOOK_SELF_FRAME_SOURCE + "] で作ったソースを選ぶ", TEXT_ID_LOOK_SELF_FRAME_SOURCE, { selectText: SELF_FRAME_SOURCE_NAME }),
    peopleTabStep("自分の枠: 映像を選んだ後"),
    step("wait", "保存されて自分の枠が準備されるまでの猶予", { ms: CALL_ACCOUNT_CHANGE_SETTLE_MS }),
    ...reads("self-before", "共有の前"),
    judgeLogLinesStep("self-before", "自分の枠: 共有の前の枠の数を記録から読む", { checks: [{ id: "canvas-count", pattern: LOG_PATTERN_CANVAS_COUNT, min: 1, group: 1 }] }),
    humanStep(selfFrameShareStartMessage(args.voiceChannel, callAccounts(args)), "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHARE_S),
    step("wait", "共有の始まりが自分の枠に届くまでの猶予", { ms: SELF_FRAME_SETTLE_MS }),
    step("collect-plugin-log", "プラグインの記録の回収 (" + main + " の共有中)"),
    ...reads("self-share", "共有を始めた後 (自動なので自分の枠が出る)"),
    ...judgeRead("self-share", "自動では、共有を始めるとチェックが付いて自分の枠が点灯し、枠が 1 つ増えて self= が入る", {
      since: "self-before",
      notInRehearsal: true,
      checks: [{ id: "self-on-share", pattern: LOG_PATTERN_SELF_ON_SHARE, min: 1 }, countIs(1), selfIn],
    }),
    humanStep(selfFrameShareStopMessage(args.voiceChannel, callAccounts(args)), "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", "共有の終わりが自分の枠に届くまでの猶予", { ms: SELF_FRAME_SETTLE_MS }),
    step("collect-plugin-log", "プラグインの記録の回収 (" + main + " の共有を止めた後)"),
    ...reads("self-stopped", "共有を止めた後 (チェックも自分の枠も残る)"),
    ...judgeRead("self-stopped", "共有を止めても、チェックは外れず、自分の枠は出たまま、枠の数も変わらない", {
      since: "self-share",
      notInRehearsal: true,
      checks: [{ id: "self-not-off", pattern: LOG_PATTERN_SELF_OFF_ANY, min: 0, max: 0 }, unchanged],
    }),
    ...selfRowCheck("off", "共有で付いたチェックを外す"),
    step("wait", "チェックが自分の枠に届くまでの猶予", { ms: SELF_FRAME_DOCK_SETTLE_MS }),
    step("collect-plugin-log", "プラグインの記録の回収 (自分の行のチェックを外した後)"),
    ...reads("self-off", "共有で付いたチェックを外した後"),
    ...judgeRead("self-off", "チェックを外すと自分の枠が消え、枠の数が元に戻る", {
      since: "self-stopped",
      notInRehearsal: true,
      checks: [{ id: "self-off", pattern: LOG_PATTERN_SELF_OFF, min: 1 }, countIs(0)],
    }),
    ...selfRowCheck("on", "チェックを付ける"),
    step("wait", "チェックが自分の枠に届くまでの猶予", { ms: SELF_FRAME_DOCK_SETTLE_MS }),
    step("collect-plugin-log", "プラグインの記録の回収 (自分の行のチェックを付けた後)"),
    ...reads("self-during", "チェックを付けた後"),
    step("screenshot-source", "自分の枠: 合成のソースの絵を PNG に撮る (チェックを付けた後)", { source: { kind: "composite" }, label: "self-during" }),
    ...judgeRead("self-during", "チェックを付けると自分の枠が点灯し、枠が 1 つ増えて self= が入る", {
      since: "self-off",
      checks: [{ id: "self-on", pattern: LOG_PATTERN_SELF_ON, min: 1 }, countIs(1), selfIn],
    }),
    ...selfRowCheck("off", "チェックを外す"),
    step("wait", "チェックが自分の枠に届くまでの猶予", { ms: SELF_FRAME_DOCK_SETTLE_MS }),
    step("collect-plugin-log", "プラグインの記録の回収 (自分の行のチェックを外した後)"),
    ...reads("self-after", "チェックを外した後"),
    ...judgeRead("self-after", "チェックを外すと自分の枠が消え、枠の数が元に戻る", {
      since: "self-during",
      checks: [{ id: "self-off", pattern: LOG_PATTERN_SELF_OFF, min: 1 }, countIs(0)],
    }),
  ];
}
// 自分の枠の判定の読みの名前 (judge-log-lines の label。judge-lines の self-frame の行がまとめる)。
export const SELF_FRAME_READ_LABELS = ["self-share", "self-stopped", "self-off", "self-during", "self-after"];

// スピーカーミュートと最小化の間も受け取りが続くこと -> 受け手の Vesktop が通話から抜けても接続が保たれ、戻ると映り直すこと。
function speakerMuteAndRejoinSteps(args) {
  const people = [callAccounts(args).chrome, callAccounts(args).firefox];
  const sharing = args.rehearsal ? [harnessPersonName("screen", 1)] : people;
  return [
    step("count-connections", "Vesktop の抜け戻りの前の送信部の接続の数を保存する", { label: VESKTOP_LEAVE_BASELINE_LABEL }),
    humanStep(speakerMuteMinimizeMessage(args.voiceChannel, callAccounts(args)), "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", "スピーカーミュートと最小化の間に、受け取りが続くかを読むまでの待ち", { ms: MUTE_MINIMIZE_HOLD_MS }),
    step("collect-plugin-log", "プラグインの記録の回収 (スピーカーミュートと最小化の間)"),
    dockTextsStep(RECEIVE_CONTINUES_LABEL),
    step("measure-video-rate", "プラグインの記録から、届いている映像の数 (stats video received_hz の直近 " + VIDEO_RATE_WINDOW_LINES + " 行) を読む (" + RECEIVE_CONTINUES_LABEL + ")", {
      label: RECEIVE_CONTINUES_LABEL,
      windowLines: VIDEO_RATE_WINDOW_LINES,
    }),
    noticeJudgeStep(false, TEXT_ID_NO_VIDEO, RECEIVE_CONTINUES_LABEL, false, "スピーカーミュートと最小化の間は「映像が止まっています」のメッセージが出ない"),
    humanStep(speakerUnmuteLeaveMessage(args.voiceChannel, callAccounts(args)), "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHARE_S),
    step("wait", "受け手の Vesktop が通話から抜けたことがドックに届くまでの猶予", { ms: VESKTOP_LEFT_SETTLE_MS }),
    step("screenshot-window", "受け手の Vesktop が抜けた間のドックを PNG に撮る", { target: "first", shotLabel: VESKTOP_LEFT_LABEL }),
    dockTextsStep(VESKTOP_LEFT_LABEL),
    noticeJudgeStep(true, TEXT_ID_SUMMARY_CONNECTED, VESKTOP_LEFT_LABEL, false, "受け手の Vesktop が抜けても、ドックは「Vesktopと接続済み」のまま通話に参加していない状態を示す"),
    connectionsKeptStep(VESKTOP_LEFT_LABEL, VESKTOP_LEAVE_BASELINE_LABEL, "受け手の Vesktop が通話から抜けても、送信部の接続が切れていない"),
    humanStep(vesktopRejoinMessage(args.voiceChannel, callAccounts(args)), "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S),
    step("wait", "受け手の Vesktop が通話に参加し直して、映像が映り直すまでの猶予", { ms: VESKTOP_REJOIN_SETTLE_MS }),
    step("collect-plugin-log", "プラグインの記録の回収 (受け手の Vesktop が戻った後)"),
    dockTextsStep(VESKTOP_REJOIN_LABEL),
    step("measure-video-rate", "プラグインの記録から、届いている映像の数 (stats video received_hz の直近 " + VIDEO_RATE_WINDOW_LINES + " 行) を読む (" + VESKTOP_REJOIN_LABEL + ")", {
      label: VESKTOP_REJOIN_LABEL,
      windowLines: VIDEO_RATE_WINDOW_LINES,
    }),
    step("screenshot-window", "受け手の Vesktop が戻った後のドックを PNG に撮る (参加者の行と枠のチェック)", { target: "first", shotLabel: VESKTOP_REJOIN_LABEL }),
    step("judge-rt", "受け手の Vesktop が戻ると、共有している人の行 (" + sharing.join("・") + ") がドックに並ぶ", {
      judge: "named-in-texts",
      inputs: { texts: { dialog: VESKTOP_REJOIN_LABEL }, names: { value: sharing } },
    }),
    // リハーサルは人の操作 (スピーカーミュート・最小化・抜けて戻る) が起きないので、上の読みは通すが、judge-lines の 2 行は判定しない
    // (起きていないことの読みが合格に見えないようにする。REHEARSAL_NOT_JUDGED)。
    ...(args.rehearsal
      ? [rehearsalSkipStep("receive-continues", "スピーカーミュートと最小化の間も受け取りが続く"), rehearsalSkipStep("rejoin", "受け手の Vesktop の抜け戻り")]
      : []),
  ];
}
export const VESKTOP_LEAVE_BASELINE_LABEL = "before-vesktop-leave";
export const RECEIVE_CONTINUES_LABEL = "mute-minimized";
export const VESKTOP_LEFT_LABEL = "vesktop-left";
export const VESKTOP_REJOIN_LABEL = "vesktop-rejoined";

// --recheck の本体: 参加後の画像 -> バージョンの表示 -> 画面共有の音 -> 遅れと映像と音のずれの測定 (recheckMeasureSteps) ->
// 共有の途中の解像度の変化 (実機の実行だけ) -> エンコードの設定 -> 別アカウント方式のメッセージ (--broadcaster-id があるとき。スピーカーミュートの段階は
// 入れない) -> 自分の枠 -> スピーカーミュートと最小化・受け手の Vesktop の抜け戻り。メインの録画と配信は実行しないので、録画は配信なしの録画
// (遅延・A/V・解像度の変化) だけになる。
// 遅れ (--latency-person) と A/V (--av-check) を測る間は、cordmux の録画を続ける (recheckMeasureSteps)。
//   - A/V: 画面共有の映像と音のずれを、配信の画面 (OBS の録画 av1) と cordmux の録画の両方で測る。遅延 (callLatencySteps) と
//     同じ回に測り、「映像のバッファ + 描く刻みの半分 - 遅延 = 音の遅れ」の関係が成り立つかを読む材料にする (値を補正に使わない)。
//   - 枠の人: 合成のソースの枠に誰が載っているかは、OBS の記録に名前がない (枠の番号とストリームの番号だけ)。人とストリームの対応は
//     cordmux の録画の録画ログにある (personScreenStreams) ので、遅れと A/V の OBS の録画の区間を cordmux の録画で覆い、その区間の対応から、
//     遅れを読む人とテスト動画を共有している人の枠を自動で見つける (judge-latency と judge-av-sync)。
// テスト動画の共有は参加の文 (callJoinMessage の recheck) で頼んでいるので、A/V の録画の前にもう一度は頼まない。
function recheckMeasureSteps(args) {
  if (!args.latencyPerson && !args.avCheck) return [];
  const avAccount = args.rehearsal ? REHEARSAL_AV_SCREEN_ACCOUNT : callAccounts(args).chrome;
  const steps = [
    step("dock-click", "ドックの [" + TEXT_ID_RECORD_START + "] を押して cordmux の録画を始める (遅れと A/V の測定の区間を含め、録画ログから枠の人を引くため)", {
      target: "first",
      textId: TEXT_ID_RECORD_START,
      desired: "on",
      gate: true,
    }),
  ];
  if (args.latencyPerson) steps.push(...callLatencySteps(args, callScreens(args)));
  if (args.avCheck) steps.push(...callAvSteps(args, "av1", null, callScreens(args), avAccount));
  steps.push(
    step("dock-click", "ドックの [" + TEXT_ID_RECORD_STOP + "] を押して cordmux の録画を終える", {
      target: "first",
      textId: TEXT_ID_RECORD_STOP,
      namePrefix: true,
      desired: "off",
      gate: true,
    }),
    step("wait-finalize", "録画ログ (ledger.jsonl) に録画ファイルの作成の行が出そろうのを待つ", {
      recordDirRel: RECORD_DIR_REL,
      timeoutMs: RECORD_FINALIZE_WAIT_TIMEOUT_MS,
      pollMs: RECORD_FINALIZE_POLL_MS,
      gate: true,
    }),
    step("collect-record-session", "cordmux の録画のフォルダ (録画ログ・区間・黒・完成) の一覧を結果に残す", { recordDirRel: RECORD_DIR_REL }),
  );
  if (args.avCheck) {
    steps.push(
      avSyncJudgeStep("av1", avAccount),
      step("judge-record-sync", "cordmux の録画の、" + avAccount + " の画面の完成に sync.py を実行する (録画の中の映像と音のずれ)", {
        person: avAccount,
        recordDirRel: RECORD_DIR_REL,
      }),
    );
  }
  return steps;
}

function callRecheckBodySteps(args) {
  const steps = [...callJoinedPictureSteps(args)];
  steps.push(...callVersionCheckSteps(args));
  steps.push(...screenAudioSteps(args));
  steps.push(...recheckMeasureSteps(args));
  if (resolutionChangePlanned(args)) steps.push(...resolutionChangeSteps());
  steps.push(...encodeSettingsSteps(args));
  if (args.broadcasterId !== null && args.broadcasterId !== undefined) {
    steps.push(...callBroadcasterSteps({ ...args, skipSpeakerMute: true }, !args.rehearsal));
    steps.push(...selfFrameSteps(args), ...speakerMuteAndRejoinSteps(args));
  }
  return steps;
}

// --av-only の本体: 参加後の画像 -> 遅れと映像と音のずれの測定だけ (recheckMeasureSteps)。締めは callClosingSteps (人は抜けて Vesktop を終了し、
// アンインストールは実行スクリプトが押す。callUninstallSteps)。
function callAvOnlyBodySteps(args) {
  return [...callJoinedPictureSteps(args), ...recheckMeasureSteps(args)];
}

function callBodySteps(args) {
  const minutes = typeof args.callMinutes === "number" ? args.callMinutes : DEFAULT_CALL_MINUTES;
  const poGate = !args.rehearsal;  const screens = callScreens(args);
  const steps = [];
  steps.push(...callJoinedPictureSteps(args));
  // --extra-checks の (a): 参加と共有を依頼する前に保存した数から、送信部の接続が切れていないかを見る (Vesktop が共有の選択でクラッシュすると、
  // 送信部の接続が切れて再起動する = close と hello が増える)。
  if (args.extraChecks) {
    steps.push(connectionsKeptStep("after-join", JOIN_BASELINE_LABEL, "参加と共有の後も送信部の接続が切れていない (Vesktop が共有でクラッシュしていない)"));
    steps.push(step("count-connections", "メインの前の送信部の接続の数を保存し直す (メインの間に切れたかを分けて読むため)", { label: MAIN_BASELINE_LABEL }));
  }
  steps.push(...callAc31Steps());
  // --extra-checks の (b): バージョンの表示 (録画と配信の前。設定ウィンドウの操作は配信中の操作に数えない位置)。
  if (args.extraChecks) steps.push(...callVersionCheckSteps(args));
  steps.push(step("stream-sink-start", "配信の受信側 (ffmpeg -listen rtmp) を起動する (実行の最後まで開けたまま)", {}));
  // 2 回目の A/V (av2) の 90 分の起点は、このメインの録画の開始。
  const mainStartIndex = steps.length;
  steps.push(step("record-start", "録画と配信を始める (配信と録画を同時に実行した状態で動作することを確認する)", { stream: true, label: "main" }));
  steps.push(step("wait", "定常状態になるまでの 15 秒", { ms: OBS_STATS_FIRST_WAIT_MS }));
  steps.push(step("obs-stats", "GetStats を取る (1 回目)", { label: "first" }));
  steps.push(step("wait", minutes + " 分の定常状態 (測定する区間)", { ms: minutes * 60 * 1000 }));
  steps.push(step("obs-stats", "GetStats を取る (2 回目)", { label: "last" }));
  steps.push(
    step("judge-output-skipped", "2 回の差分から配信用エンコードの欠落の割合を出す (しきい値 " + OUTPUT_SKIPPED_RATIO_THRESHOLD * 100 + "%)", {
      gate: true,
    }),
  );
  // --extra-checks の (f): 定常状態の終わり(録画と配信の最中)に、映像のバッファの統計の表を読む。
  if (args.extraChecks) steps.push(...incomingVideoSteps());
  steps.push(step("record-stop", "メインの録画を終了する (配信はこの後も実行の最後まで続ける)", { stream: false, tailSeconds: 0, label: "main" }));
  // --extra-checks の (a): 定常の間も送信部の接続が切れていない (負荷で送信部がクラッシュしていない)。参加者の行を押す操作より前に読む。
  if (args.extraChecks) steps.push(connectionsKeptStep("after-main", MAIN_BASELINE_LABEL, "定常の間も送信部の接続が切れていない"));
  const ac30Person = args.rehearsal ? harnessPersonName("screen", 1) : callAccounts(args).firefox;
  steps.push(...callAc30Steps(ac30Person, !args.rehearsal));
  // 配置の判定は、オプションを問わず実行する (記録するだけ)。
  steps.push(step("judge-composite-layout", "外した人 (" + ac30Person + ") のタイルが消えて後ろが詰まり、戻すと元の並びと位置に戻ったか (自分の枠は左上、画面共有は表示名の順、2 つ以上なら 4 等分の決まった位置。配置の行 3 つ)", { person: ac30Person, labels: AC30_ITEM_LABELS }));
  // 声を 1 人ずつ鳴らして録り、区間ごとの音量を後で読む (callAc32Steps)。
  steps.push(...callAc32Steps(args));
  // 操作の数は**メインの区間だけ**を数える (配信はこの後も続くので、ここで数える。judgeOperationCount は最初の
  // record-start からここまでの操作を「配信中」と見る)。この後の段階の操作 (欄への入力・録画のラベル) は数えない。
  steps.push(step("count-operations", "メインの区間で触った操作の数を数える (付け外し・音量・ミュート以外が 0 か)", { gate: true }));

  // --latency-person: cordmux が足す遅延を測る区間。メインの後 (定常状態になってから) で、配信者がまだ共有している間
  // (別アカウント方式の段階で配信者は抜けて、共有せずに戻る) に置く。操作の数を数えた後なので、置く・外す操作は数えない。
  if (args.latencyPerson) steps.push(...callLatencySteps(args, screens));

  // 別アカウント方式 (--broadcaster-id があるときだけ)。
  if (args.broadcasterId !== null && args.broadcasterId !== undefined) steps.push(...callBroadcasterSteps(args, poGate));

  // 短い項目。テスト動画の音を運ぶ枠の表示名は、実機の実行では Chrome のアカウント、リハーサルではマーカーの音を乗せたテスト用の画面。
  const avAccount = args.rehearsal ? REHEARSAL_AV_SCREEN_ACCOUNT : callAccounts(args).chrome;
  steps.push(...callAvSteps(args, "av1", "Chrome (" + callAccounts(args).chrome + ") で『テスト動画』のタブを、音声も含めて共有してください (最初から共有したままなら、そのまま伝えてください)", screens, avAccount));
  // --extra-checks: av1 の sync.py に合否のしきい値を当てる (実行中に結果が見える。記録するだけ)。
  if (args.extraChecks) steps.push(avSyncJudgeStep("av1"));
  steps.push(...callMidRecordingSteps());
  // --extra-checks の (c): 置き換えの後と戻した後に、声の枠ごとの短い録画を挟む (callVoiceSourceStepsWithCheck)。
  steps.push(...(args.extraChecks ? callVoiceSourceStepsWithCheck(args) : callVoiceSourceSteps(true, args)));
  // 連続運転 (--skip-continuous で省く)。
  if (!args.skipContinuous) steps.push(...callContinuousSteps(args));
  // 2 回目の A/V はメインの録画 (main) の開始から CALL_AV_SECOND_GAP_MINUTES 以上あと (傾きを出すため)。足りない分は連続運転を続ける。
  // 連続運転を省いた場合は待たない (90 分をあけるためだけに利用者を待たせない。傾きはメイン・av1・av2 の 3 点で読む)。
  if (!args.skipAv2) {
    const sinceMain = plannedSeconds(steps.slice(mainStartIndex));
    const gapSeconds = CALL_AV_SECOND_GAP_MINUTES * 60 - sinceMain;
    if (gapSeconds > 0 && !args.skipContinuous) {
      steps.push(step("wait", "2 回目の A/V まで連続運転を続ける (メインの録画の開始から " + CALL_AV_SECOND_GAP_MINUTES + " 分あける)", { ms: gapSeconds * 1000 }));
    }
    // リハーサル: テスト用の送信側が寿命で先に終了していないかを av2 の前に確認する (終了していれば理由つきで失敗にする。実機の実行にはないステップ)。
    if (args.rehearsal) steps.push(step("harness-alive", "テスト用の送信側がまだ動いているか (av2 の前。寿命で先に終了していれば失敗)", { gate: true }));
    steps.push(...callAvSteps(args, "av2", "Chrome (" + callAccounts(args).chrome + ") で『テスト動画』のタブを、音声も含めてもう一度共有してください (まだ共有したままなら、そのまま伝えてください)", screens, avAccount));
    if (args.extraChecks) steps.push(avSyncJudgeStep("av2"));
    // --extra-checks のときは av2 - av1 にしきい値 (AV_DRIFT_DELTA_LIMIT_MS) を当てる。それ以外のときは差を出すだけ。
    steps.push(
      step("judge-av-drift", "av1 と av2 の A/V のずれの差を区画ごとに出す (傾きの材料。" + (args.extraChecks ? "差のしきい値 " +AV_DRIFT_DELTA_LIMIT_MS + " ms 未満。記録するだけ" : "判定はしない") + ")", {
        first: "av1",
        second: "av2",
        ...(args.extraChecks ? { deltaLimitMs: AV_DRIFT_DELTA_LIMIT_MS } : {}),
      }),
    );
  }
  return steps;
}

// 締め: 配信を止めてから、シーンコレクションの往復と診断情報のエクスポート、ログの回収、アンインストールの判定。
// **メインの録画とシーンコレクションの往復の録画の判定ツールはここに置かない** (callAnalysisSteps。OBS を閉じた後)。
// 判定ツールの実行には長い時間がかかり、アンインストールの前に置くと、その後の Vesktop を閉じる操作の最中に制限時間に
// 達して、アンインストールの判定ができなくなる。判定ツールは録画のファイルを読むだけで送信部も利用者の操作も要らないので、
// 利用者の操作を全部済ませてから実行する。送信部が要る judge-connection-held と診断情報のエクスポートと、
// シーンコレクションの往復の録画そのものは、Vesktop を閉じてもらう前に置いたままにする。
function callClosingSteps(args) {
  const steps = [];
  // --recheck は配信も受信側もシーンコレクションの往復も書き出しも実行しない。
  if (!args.recheck) {
    steps.push(step("stream-stop", "配信を止める (実行の最後)", {}));
    steps.push(step("stream-sink-stop", "配信の受信側を閉じる", {}));
    steps.push(...callAc35Steps());
    // --skip-export-bundle: 保存ダイアログへキーで場所を入力する書き出しを省く (診断情報のエクスポートのステップで確認している)。
    if (!args.skipExportBundle) steps.push(...exportBundleSteps());
  }
  steps.push(step("collect-plugin-log", "プラグインの記録の回収"));
  const uninstall = callUninstallSteps(args);
  // Vesktop を終了してもらった直後 (読み込んでいた Vencord が設定を書き終えた後、
  // アンインストールと利用者のふだんの Vencord の起動より前) に、[Vesktopに追加] の前にコピーした Vencord の設定とキーを比べる。
  if (vencordSettingsCopied(args)) {
    const quit = uninstall.findIndex((s) => s.kind === "human" && s.quitVesktop === true);
    uninstall.splice(quit + 1, 0, vencordSettingsCompareStep());
  }
  steps.push(...uninstall);
  steps.push(step("wait", "state.json とバックアップが書き終わるまでの猶予", { ms: CALL_GATE_WRITE_SETTLE_MS }));
  steps.push(step("read-state-json", "アンインストールの後の state.json を読む", { appDataRel: VESKTOP_STATE_JSON_REAL_REL, label: "after-uninstall" }));
  steps.push(
    step("judge-vencord-dir-restored", "アンインストールで vencordDir がバックアップの previous に戻ったか (戻しは cleanup.ps1 -CallWindow ではなくアンインストールで済ませる)", {
      backupPath: vencordDirBackupPath(args),
      gate: true,
    }),
  );
  // アンインストールが Vencord の設定を壊していないかを、アンインストールの後にも比べる。
  if (vencordSettingsCopied(args)) steps.push(vencordSettingsRestoredStep());
  steps.push(humanStep(VESKTOP_START_AFTER_UNINSTALL_MESSAGE, "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S));
  return steps;
}

// 締めの後の判定ツール (メインの録画とシーンコレクションの往復の録画を読むだけ。callClosingSteps と同じ理由で後に置く)。OBS を閉じた後に実行する。
function callAnalysisSteps(args) {
  const screens = callScreens(args);
  return [
    step("collect-recording", "録画ファイルの回収 (定常状態のメイン)", { label: "main" }),
    step("judge", "frames.py で欠落・重複・静止を見る (2x2 の区画ごと)", { tool: "frames.py", cells: screens, recording: "main" }),
    step("judge", "sync.py で A/V のずれを見る (2x2 の区画ごと)", { tool: "sync.py", cells: screens, recording: "main" }),
    step("judge", "smooth.py でフレーム間隔の粗さを見る (3 条件)", { tool: "smooth.py", ncells: screens, recording: "main" }),
    step("collect-recording", "録画ファイルの回収 (シーンコレクションの切り替えを戻した後)", { label: "ac35" }),
    step("judge", "frames.py で、シーンコレクションを戻した後に枠のソースへ映像が再び乗り欠落がないかを見る", {
      tool: "frames.py",
      cells: screens,
      recording: "ac35",
      label: "ac35-after-switch-back",
    }),
    // --extra-checks の (c)(d)(e)。どれもファイルを読むだけで、利用者の操作も送信部も要らない。
    ...(args.extraChecks ? callExtraAnalysisSteps(args) : []),
    // OBS のログ (閉じた後なので書き終わっている) から、表示の遅延の測定値と音の待ち時間の増加を読む。オプションを問わず実行する。
    step("judge-display-delay", "表示の遅れの測りが動作している: OBS の記録の video_latency の shown_n が 0 でない (合成の子に付けた測定のフィルタが付いた証拠)", {}),
    step("judge-audio-buffering", "OBS の全体の音の待ち時間: 記録の「adding N milliseconds of audio buffering」の行を時刻とソースの名前ごとに数える (しきい値は決めていない。記録するだけ)", {}),
  ];
}

// 遅延の録画に latency.py を実行する (ツールが無ければ「ツールがない」と記録して先へ進む)。--latency-person が無ければステップごと省く。
// 遅れを読む人の枠は、遅れの録画の始めと終わりに読んだ枠の割り当て (OBS の記録の Screen slot N carries stream M) と、その区間を覆う
// cordmux の録画の録画ログ (人とストリームの対応) から引く (recheckMeasureSteps。personScreenStreams と personCellOf)。録画ログが区間を
// 覆わないとき (--recheck でない call-1) は、テスト用の送信側なら名前 (harness-screen-<ストリームの番号>) で引き、それもできなければ
// 判定せず (handRead)、枠の PNG で選んで latency.py を手で実行する。リハーサルは比べる Vesktop のウィンドウがないので判定しない。
function judgeLatencySteps(args) {
  if (!args.latencyPerson) return [];
  if (args.rehearsal) return [rehearsalSkipStep("latency", "cordmux が足す遅れ")];
  return [
    step("judge-latency", "cordmux が足す遅れ: 遅れの録画の、" + latencyPersonFor(args) + " の枠と Vesktop のウィンドウで同じ帯の番号が出るフレームの差を latency.py で読む (目標 " + LATENCY_TARGET_DETECT_MS + " / " + LATENCY_TARGET_ACCEPT_MS + " ms。記録するだけ)", {
      recording: LATENCY_RECORDING_LABEL,
      person: latencyPersonFor(args),
      cells: callScreens(args),
      tool: LATENCY_TOOL,
    }),
  ];
}

// OBS のログから、ストリームごとの整列のずれの傾き・送信側のフォールバックの段階・アイコンを送らなかった行・接続と切断の数を数える。
// リハーサルは短く、傾きを読む長さ続くストリームがないので、そのときは判定しない (shortRunNotJudged。長く続いたストリームがあれば判定する)。
function judgeCallLogStep(args) {
  return step("judge-call-log", "OBS の記録から、ストリームごとの整列のずれの傾き・送信側のフォールバックの段階・アイコンを送らなかった行・接続と切断の数を数える", {
    maxDriftPercent: ALIGN_DRIFT_MAX_PERCENT,
    minSpanSeconds: ALIGN_DRIFT_MIN_SPAN_S,
    ...(args && args.rehearsal ? { shortRunNotJudged: REHEARSAL_NOT_JUDGED["align-drift"] } : {}),
  });
}

// --recheck で実行した項目だけを並べる judge-lines の項目 (JUDGE_LINES の item)。実行していない項目は表に出さない (null の行も失敗にならないが、
// 実行していない項目が並ぶと読み違えるため)。
export const RECHECK_JUDGE_ITEMS = [
  "add-to-vesktop",
  "vesktop-connects",
  "self-account",
  "latency",
  "roster-labels",
  "version-row",
  "screen-audio",
  "format-change",
  "encode-limits",
  "encode-choice",
  "self-frame",
  "receive-continues",
  "rejoin",
  "align-drift",
];
// --recheck の judge-lines の項目。--av-check なら A/V の 2 行 (合成のソースと cordmux の録画) を足す。--av-only は、実行した項目
// (追加・接続・遅れ・A/V・整列のずれの傾き) だけを並べる。
export function recheckJudgeItems(args) {
  const av = args && args.avCheck ? ["av-sync-av1", "record-sync"] : [];
  if (args && args.avOnly) return ["add-to-vesktop", "vesktop-connects", ...(args.latencyPerson ? ["latency"] : []), ...av, "align-drift"];
  return [...RECHECK_JUDGE_ITEMS, ...av];
}

// 共有の途中の解像度の変化の録画を frames.py で読むステップ (実機の実行だけ。resolutionChangePlanned)。区画は合成の枠。
function judgeResolutionChangeSteps(args) {
  if (!resolutionChangePlanned(args)) return [];
  return [
    step("collect-recording", "録画ファイルの回収 (共有の途中の解像度の変化)", { label: RESCHANGE_RECORDING_LABEL }),
    step("judge", "frames.py で、共有の解像度が変わっても欠落・重複・静止がないかを見る (2x2 の区画ごと。記録するだけ)", {
      tool: "frames.py",
      cells: callScreens(args),
      recording: RESCHANGE_RECORDING_LABEL,
      label: "reschange-frames",
    }),
  ];
}

// --recheck の判定 (OBS を閉じた後)。録画とログを読むものだけ。
function callRecheckAnalysisSteps(args) {
  return [
    ...judgeResolutionChangeSteps(args),
    ...judgeLatencySteps(args),
    judgeCallLogStep(args),
    step("judge-lines", "合否のしきい値を当てた判定を 1 つの表にまとめる (--recheck で実行した項目だけ。記録するだけ)", { items: recheckJudgeItems(args) }),
  ];
}

// --extra-checks の判定 (OBS を閉じた後)。どれも gate にしない (実行を失敗にはせず、結果 JSON に数と判定を残す)。
// しきい値を当てる判定は judge-lines が最後に 1 つの表にまとめる (JUDGE_LINES)。
function callExtraAnalysisSteps(args) {
  const avAccount = args.rehearsal ? REHEARSAL_AV_SCREEN_ACCOUNT : callAccounts(args).chrome;
  const latency = judgeLatencySteps(args);
  return [
    // メインの sync.py (区画ごと) にしきい値を当てる。
    avSyncJudgeStep("main"),
    // (d) 途中で始めた cordmux の録画のうち、テスト動画を共有している人の画面の完成した録画 (録画ログの person と finalize の行で選ぶ) に
    // sync.py を実行する。区画はなし (その人の共有の全面。テスト動画は表示域いっぱいに引き伸ばしてある = share/sync-video.html)。
    // しきい値もこのステップの中で当てる (記録するだけ)。
    step("judge-record-sync", "途中で始めた cordmux の録画の、" + avAccount + " の画面の完成に sync.py を実行する (録画の中の映像と音のずれ。しきい値はメインと同じ)", {
      person: avAccount,
      recordDirRel: RECORD_DIR_REL,
    }),
    // (c) 使う声の確認の録画に tone_scan.py (1 kHz) を実行する。--windows の区間で読む (voiceSourceScanArgs)。
    // 声を 1 人ずつ鳴らした録画の区間ごとの音量は、callAc32Steps のマーカーから区間を組み、tone_scan.py --windows で読む。
    step("judge-voice-source", "使う声の確認の録画 " + VOICE_SOURCE_CHECKS.length + " 本で、1 kHz が声を借りる人の枠へ移って戻ったかを数える", {
      hz: VOICE_SOURCE_TONE_HZ,
      checks: VOICE_SOURCE_CHECKS.map((c) => ({ label: c.label, expectTone: c.expectTone })),
      recordSeconds: VOICE_SOURCE_CHECK_RECORD_MS / 1000,
      marginSeconds: VOICE_SOURCE_SCAN_MARGIN_S,
    }),
    step("judge-voice-volume", "声の音量: " + VOICE_VOLUME_PCT + "% にした後の録画の音量が、100% の録画 (" + VOICE_VOLUME_BEFORE_LABEL + ") の +" + Math.round(20 * Math.log10(VOICE_VOLUME_PCT / 100) * 100) / 100 + " dB に収まるかを読む (記録するだけ)", {
      before: VOICE_VOLUME_BEFORE_LABEL,
      after: VOICE_VOLUME_AFTER_LABEL,
      pct: VOICE_VOLUME_PCT,
      toleranceDb: VOICE_VOLUME_GAIN_TOLERANCE_DB,
      hz: VOICE_SOURCE_TONE_HZ,
      recordSeconds: VOICE_SOURCE_CHECK_RECORD_MS / 1000,
      marginSeconds: VOICE_SOURCE_SCAN_MARGIN_S,
    }),
    step("judge-ac32", "声を 1 人ずつ鳴らした録画の区間ごとの音量で、ミュートは無音、外すと基準に戻るかを読む (記録するだけ)", {
      recording: "ac32",
      voices: callVoicePersons(args).length,
      hz: AC32_TONE_HZ,
      marginSeconds: AC32_WINDOW_MARGIN_S,
    }),
    ...latency,
    // (e) OBS のログの stats 行と送信側の行。
    judgeCallLogStep(args),
    // しきい値を当てた判定を 1 つの表にまとめる (実行中に止める判定と、記録するだけの判定を分けて書く)。
    step("judge-lines", "合否のしきい値を当てた判定を 1 つの表にまとめる (JUDGE_LINES。どれも記録するだけ)", {}),
  ];
}

// 通話に参加する案内の文 (利用者に見せる)。4 つのアカウント (CALL_ACCOUNT_ROLES) 全部に、ボイスチャンネルに入ることを頼む (Vesktop のアカウントも
// 入らないと、cordmux は何も受け取らない。11-13 の 1 回目は文が Vesktop に入ることを頼んでおらず、入る前に次の手順へ進んだ)。
// マイク: 4 つとも同じ PC のマイクなので、スピーカーから出た音 (テスト動画の音を含む) を拾うと、声として配信の音に混ざる。
//   - --recheck (--av-only を含む): 4 つともミュートにする。声の音を使う確認がないので、Chrome をテスト用のマイクの音つきで起動する
//     (launch-chrome-fake-mic.ps1) 必要もない。Chrome は、実行スクリプトを動かす側が実行を始める前にテスト動画のページを開いておく。
//   - それ以外の call-1: Chrome だけミュートを解除する (使う声の確認と声のミュートの往復は、Chrome のテスト用のマイクの 1 kHz を声として使う。
//     Chrome は launch-chrome-fake-mic.ps1 で起動しておく)。ほかの 3 つはミュートにする。
// Vesktop の共有は vesktopShare (--vesktop-share) のときだけ依頼する。
// ボイスチャンネルの名前は `--voice-channel` の値 (`voiceChannel`)。名前をコードに書かないため、渡されていなければ名前を挙げずに書く。
// recheck が真のとき: Chrome の共有にテスト動画のタブを音声付きで選んでもらう 1 文 (画面共有の音と、映像と音のずれを確かめるため。
// 後の A/V の録画の前にもう一度は頼まない) と、連絡が途絶えたときの案内を添える。
// 返事は要らない: 実行スクリプトが OBS の記録で、全員が入って共有を始めたことを見て進む (callJoinStep の until call-ready)。
export function callJoinMessage({ vesktopShare, voiceChannel, recheck, avOnly, accounts = DEFAULT_CALL_ACCOUNTS } = { vesktopShare: VESKTOP_SHARE_IN_CALL_ONE, voiceChannel: null, recheck: false, avOnly: false, accounts: DEFAULT_CALL_ACCOUNTS }) {
  const channel = voiceChannel ? "ボイスチャンネル「" + voiceChannel + "」" : "指定のボイスチャンネル";
  const base = callJoinBaseMessage(channel, vesktopShare, Boolean(recheck), accounts);
  const ready = "\n全員が入って画面共有を始めると、こちらで確かめて次へ進みます (返事は必要ありません)。";
  return recheck
    ? base +
        "\nChrome (" + accounts.chrome + ") の画面共有は、先に「テスト動画」のタブで「再生する (音が出ます)」のボタンを押し、そのタブを選んで「タブの音声も共有する」のチェックを付けてください " +
        (avOnly ? "(映像と音のずれを確かめるためです)。" : "(画面共有の音が届くかと、映像と音のずれを確かめるためです)。") +
        ready +
        "\n" +
        CALL_CONTACT_LOST_GUIDANCE
    : base + ready;
}

function callJoinBaseMessage(channel, vesktopShare, recheck, accounts) {
  const sharers = "Desktop版のDiscord (" + accounts.main + ")、Firefox (" + accounts.firefox + ")、Chrome (" + accounts.chrome + ")";
  return (
    "Desktop版のDiscord (" + accounts.main + ")、Firefox (" + accounts.firefox + ")、こちらが先に開いておいたChrome (" + accounts.chrome + ")、Vesktop (" +
    accounts.receiver + ") の4つで、" + channel + "に入ってください。" +
    (recheck
      ? "マイクは4つともミュートにしてください (スピーカーから出たテスト動画の音をマイクが拾うと、配信の音に混ざるためです)。"
      : "マイクは、Chrome (" + accounts.chrome + ") だけミュートを解除し (テスト用の音はそこから送ります)、ほかの3つはミュートにしてください " +
        "(スピーカーから出た音をマイクが拾うと、配信の音に混ざるためです)。") +
    "\n" + sharers + " の3つで画面共有を始めてください。" +
    (vesktopShare ? "Vesktop (" + accounts.receiver + ") でも、画面共有を1つ始めてください。" : "Vesktop (" + accounts.receiver + ") では画面共有を始めないでください。")
  );
}

// 参加と共有の依頼。返事を待たず、OBS の記録の最後の Facts の行で、通話の人数が 4 つのアカウント以上、画面共有の人数が共有を頼んだ人数以上に
// なったら進む (until call-ready)。人が返事を書いた通知ファイルでも進む (どの until でも同じ) が、その場合は全員がそろう前に進みうるので、
// 実行スクリプトを動かす側は、この手順では通知ファイルを置かない (README の「実際の通話」)。
function callJoinStep(args) {
  return humanStep(
    callJoinMessage({ vesktopShare: args.vesktopShare || VESKTOP_SHARE_IN_CALL_ONE, voiceChannel: args.voiceChannel, recheck: Boolean(args.recheck), avOnly: Boolean(args.avOnly), accounts: callAccounts(args) }),
    "call-ready",
    HUMAN_JOIN_TIMEOUT_MS,
    HUMAN_ALLOWANCE_JOIN_S,
    { roster: CALL_ACCOUNT_ROLES.length, sharing: callScreenSharers(args).length },
  );
}

// --recheck: 接続の後に、OBS のログの Facts の行に self=yes が出るまで待つ (最長 SELF_ACCOUNT_WAIT_TIMEOUT_MS)。出なければ失敗 (gate)。
// stopOnFail は付けない (実行は続ける)。baseline は保存した hello の数の名前 (wait-connection と同じ。無ければ 0 から数える)。
function selfAccountWaitStep(baseline) {
  return step("wait-self-account", "送信側が自分のアカウントを届けたか: OBS の記録の Facts の行に self=yes が出るまで待つ (最長 " + SELF_ACCOUNT_WAIT_TIMEOUT_MS / 1000 + " 秒)", {
    timeoutMs: SELF_ACCOUNT_WAIT_TIMEOUT_MS,
    pollMs: SELF_ACCOUNT_POLL_MS,
    ...(baseline ? { baseline } : {}),
    gate: true,
  });
}

// ---- 通話の本体 (--step call-1) の組み立て ------------------------------------------------
//
// 配信と録画の同時実行、定常状態の測定、画面共有の操作の順に並べ、そのあとに短い項目と連続運転を置く (2 時間 30 分)。
// **[Vesktop に追加] と [cordmux のシーンを作成] は record-start より前**に置く (操作の数を数えるのは配信中の
// 操作だけ。count-operations はメインの直後に置いて、メインの区間だけを数える)。配信 (受信側) は実行の最後まで
// 続け、シーンコレクションの往復とアンインストールは配信を止めた後に置く: OBS の UI は出力中の切り替えを禁じており、
// obs-websocket 経由で同じ制限が掛かるかは測定していない。
// ハードウェアのデコードの 10 分はこの実行では省く: OBS を再起動すると配信の受信側を閉じることになり
// 「受信側は最後まで開けたまま」と両立せず、締めのアンインストールの後は送信部がないので 2 回目の呼び出しは接続できない。
// 欠落の割合の既定はソフトウェアのデコードの値で決めるので、判定には影響しない。
function buildCallOneSteps(args) {
  const steps = [];
  // 実行スクリプトが用意したもの (一時の Vencord のビルド・ブラウザのプロファイル・共有の中身の
  // 一時フォルダ) を **ステップの一番先に** 書く: 途中で抜けた場合でも cleanup.ps1 -CallWindow が
  // 消す対象を知っている状態にする。1 つも渡されていなければステップごと省く。
  const artifacts = callWindowArtifactsFromArgs(args);
  if (artifacts !== null) {
    steps.push(
      step("write-call-window-artifacts", "実行スクリプトが用意したものの保存先を " + CALL_WINDOW_ARTIFACTS_REL + " へ書く (後始末で消す対象)", {
        path: CALL_WINDOW_ARTIFACTS_REL,
        artifacts,
      }),
    );
  }
  // 枠の割り当ては「自動 (共有を始めた人を上限まで)」にする。手動のまま参加者一覧の行を座標で
  // 押す方式は、行の並び (一覧に載る人と順番) が実機の参加者一覧とテスト用の送信側で違い、押しても枠に届かない。自動なら
  // 共有を始めた人が枠に入る (製品の割り当ては共有の開始のときに 1 回で、周期では動かないので、枠を外す・戻す
  // 押下はそのまま動作する)。record と同じく、呼び出しの引数より実行計画を優先する。
  const callArgs = { ...args, dockAssignMode: "auto" };
  steps.push(...seedAndPlaceSteps(callArgs), ...launchAndConnectSteps(args));

  if (args.rehearsal) {
    // リハーサル: 実機の Vesktop の代わりにテスト用の送信側を使う (--step record と同じ手順。画面 4 + 声 4、マーカーの音は画面の 1 つ目)。
    // [Vesktop に追加] は実行スクリプトが押す (absent の偽の state.json に製品が書き、バックアップができる = アンインストールの判定の材料)。
    const body = rehearseSteps([...callOneBodySteps(args), ...callClosingSteps(args)]);
    const harnessSeconds = Math.ceil(plannedSeconds(body) * REHEARSAL_HARNESS_LIFETIME_FACTOR) + REHEARSAL_HARNESS_MARGIN_S;
    steps.push(step("wait", "ドックがメッセージを描くまでの猶予 (押す前)", { ms: SELECT_FOUR_SETTLE_WAIT_MS }));
    steps.push(step("dock-click", "リハーサル: 実行スクリプトが [" + TEXT_ID_ADD_TO_VESKTOP + "] を押す (偽の state.json に製品が書く)", {
      target: "first",
      textId: TEXT_ID_ADD_TO_VESKTOP,
      index: 0,
      invokeTimeoutMs: ROUNDTRIP_INVOKE_TIMEOUT_MS,
      gate: true,
    }));
    steps.push(step("wait", "state.json とバックアップが書き終わるまでの猶予", { ms: CALL_GATE_WRITE_SETTLE_MS }));
    steps.push(
      step("harness-start", "テスト用の送信側を product モードで画面 " + callScreens(args) + " 本 + 声 " + callVoices(args) + " 本、" + harnessSeconds + " 秒 (実機の Vesktop の代わり。実行計画の見込み + 余裕)", {
        mode: "product",
        nScreen: callScreens(args),
        nVoice: callVoices(args),
        seconds: harnessSeconds,
        hw: args.hw,
        warmup: args.warmup,
        syncTone: DEFAULT_SYNC_TONE,
      }),
    );
    steps.push(step("wait-connection", "テスト用の送信側が接続を確立するまで待つ", { timeoutMs: REHEARSAL_CONNECT_WAIT_TIMEOUT_MS, gate: true }));
    // テスト用の送信側は Vesktop のアカウントではなく、自分のアカウントを送らない。実機の実行と同じ位置に、判定しないことの記録を置く
    // (REHEARSAL_NOT_JUDGED。送信側が Vesktop のアカウントを読むかは、実際の通話でしか確かめられない)。
    if (args.recheck && !args.avOnly) steps.push(rehearsalSkipStep("self-account", "送信側が自分のアカウントを届けたか"));
    // --extra-checks の (a): 参加と共有の前の接続の数を保存する (実機の実行と同じ位置)。
    if (args.extraChecks) steps.push(step("count-connections", "参加と共有の前の送信部の接続の数を保存する (共有で落ちていないかを後で比べる)", { label: JOIN_BASELINE_LABEL }));
    steps.push(...rehearseSteps([humanStep("(リハーサル) 通話に参加して共有を始める操作の代わりに待つ", "go-file", HUMAN_JOIN_TIMEOUT_MS, HUMAN_ALLOWANCE_JOIN_S)]));
    steps.push(...body);
    steps.push(step("harness-stop", "テスト用の送信側を止める (子プロセスごと)", {}));
  } else {
    if (args.startWithAdd) {
      // --start-with-add: 開始ゲートを実行せず、ここで [Vesktopに追加] -> 判定 -> バックアップ -> hello の数
      // -> Vesktop の起動 -> 増えた hello を待つ (call-hold の先頭と同じ並び。Vesktop の終了は [Vesktopに追加] の依頼と 1 つ)。待ちの上限は 20 分
      // (繋がった時点で先へ進む。20 分たっても繋がらなければ実行を打ち切る)。
      // [Vesktopに追加] の前 (利用者の Vesktop がテスト用にインストールした Vencord を読む前) に Vencord の設定ファイルをコピーする (読むだけ・コピーするだけ)。
      if (vencordSettingsCopied(args)) steps.push(vencordSettingsCopyStep());
      // [Vesktopに追加] が state.json に届かない・起動し直した Vesktop が接続しない、の
      // 2 つは後の答えが 1 つも取れなくなる前提なので、失敗なら実行を止めて締めの後始末 (finally の vencordDir の戻し) へ進む
      // (stopOnFail。止めないと、接続していない Vesktop のまま利用者に参加と共有を依頼し続け、1 時間を使う)。
      steps.push(...addToVesktopSteps(args).map((s) => (s.kind === "human" && s.until === "state-json-points-to-placed" ? { ...s, stopOnFail: true } : s)));
      // 追加の依頼で Vesktop を終了してもらっているので、ここで起動してもらう。--recheck は、ログインした状態の画面に戻ったことの
      // 返事と、その後の案内も含む文にする (vesktopStartAfterAddMessage)。
      steps.push(humanStep(args.recheck ? vesktopStartAfterAddMessage(callAccounts(args)) : VESKTOP_START_AFTER_ADD_MESSAGE, "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_REOPEN_S));
      steps.push(
        step("wait-connection", "起動し直した実機の Vesktop の送信部が接続してくるのを待つ (保存した数より増えたときだけ通す。20 分たっても接続しなければ実行を打ち切る)", {
          timeoutMs: CALL_CONNECT_WAIT_TIMEOUT_MS,
          baseline: ADD_TO_VESKTOP_BASELINE_LABEL,
          gate: true,
          stopOnFail: true,
        }),
      );
      // --recheck: 接続の直後に、送信側が自分のアカウントを届けたかを見る (止めない。以後の確認は自分のアカウントが届かなくても測れる)。
      // --av-only は映像と音のずれだけを確かめるので見ない。
      if (args.recheck && !args.avOnly) steps.push(selfAccountWaitStep(ADD_TO_VESKTOP_BASELINE_LABEL));
    } else {
      // **利用者が Vesktop を開き直すのはこの待ちの前**。call-gate-d の
      // Vesktop は package.json を退けた状態で起動しているので、package.json も必要な場合は送信部が
      // 読み込まれていない。マーカーを出すのは、初期設定が data/vencord を置き直し、OBS が待ち受けを開いた後 = ここ。
      steps.push(humanStep(VESKTOP_START_FOR_CALL_ONE_MESSAGE, "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_REOPEN_S));
      steps.push(
        step("wait-connection", "開き直した実機の Vesktop の送信部が接続してくるのを待つ (20 分たっても接続しなければ実行を打ち切る)", {
          timeoutMs: CALL_CONNECT_WAIT_TIMEOUT_MS,
          gate: true,
        }),
      );
    }
    // --extra-checks の (a): 参加と共有の前の接続の数を保存する (callBodySteps の after-join・after-main がこれと比べる)。
    if (args.extraChecks) steps.push(step("count-connections", "参加と共有の前の送信部の接続の数を保存する (共有で落ちていないかを後で比べる)", { label: JOIN_BASELINE_LABEL }));
    steps.push(callJoinStep(args));
    steps.push(...callOneBodySteps(args));
    steps.push(...callClosingSteps(args));
  }
  steps.push(...closeSteps("first"));
  // 録画を読むだけの判定ツールは OBS を閉じた後 (理由は callClosingSteps のコメント)。
  steps.push(...(args.recheck ? callRecheckAnalysisSteps(args) : callAnalysisSteps(args)));
  steps.push(...finishSteps(args, "call-1"));
  // 30 / 60 / 90 分の位置の obs-stats。リハーサルは待ちを縮めた分だけ位置も縮める (手順を一度通すため)。
  const marks = args.rehearsal ? CALL_STATS_MARK_MINUTES.map((m) => m * REHEARSAL_TIME_FACTOR) : CALL_STATS_MARK_MINUTES;
  return insertStatsAtMinutes(steps, marks);
}

// call-1 の本体 (参加の後から締めの前まで)。--recheck は短い実行、--av-only はその中の遅れと映像と音のずれの測定だけ。
function callOneBodySteps(args) {
  if (!args.recheck) return callBodySteps(args);
  return args.avOnly ? callAvOnlyBodySteps(args) : callRecheckBodySteps(args);
}

// テスト用の送信側 (harness の Electron) の起動の引数。userData (Electron が設定やキャッシュを書く場所) を userDataDir に向ける。
// 向けないと %APPDATA%\cordmux-harness に書く。--real-profile のときは APPDATA を差し替えないので、環境変数の差し替えには頼れない。
// userDataDir は、cleanup.ps1 が実行の後に消す場所の下に置く。
export const HARNESS_USER_DATA_DIR_NAME = "cordmux-harness";
export function harnessElectronArgs(userDataDir) {
  return ["--user-data-dir=" + userDataDir, "."];
}

// ---- 通話の判定 (純粋関数。run-window.mjs が呼ぶ) ------------------------------------

// アンインストールの後: state.json の vencordDir がバックアップの previous に戻っている (previous が null なら鍵がない)。
export function judgeVencordDirRestored({ stateText, backupText }) {
  const backup = parseObjectOrNull(backupText);
  if (!backup) return result(false, "the backup (vencord_dir_backup.json) is not readable", { previous: null, vencordDir: null });
  const state = parseObjectOrNull(stateText);
  if (!state) return result(false, "state.json is not readable", { previous: backup.previous ?? null, vencordDir: null });
  const hasKey = Object.prototype.hasOwnProperty.call(state, "vencordDir");
  const vencordDir = hasKey ? state.vencordDir : null;
  const previous = Object.prototype.hasOwnProperty.call(backup, "previous") ? backup.previous : null;
  if (previous === null) {
    return result(!hasKey, "vencordDir is still present (" + String(vencordDir) + ") although the backup's previous is null", { previous, vencordDir });
  }
  const ok = typeof vencordDir === "string" && normalizePathForCompare(vencordDir) === normalizePathForCompare(previous);
  return result(ok, "vencordDir is " + String(vencordDir) + ", the backup's previous is " + String(previous), { previous, vencordDir });
}

// 読んだ文のどれにも、名前が 1 つも出ていない (案内が消えたことの判定)。文を 1 つも読めていなければ失敗。
export function judgeNotNamedInTexts({ texts, names }) {
  const list = Array.isArray(texts) ? texts.map(String) : [];
  const found = (names || []).filter((n) => list.some((t) => t.includes(n)));
  return result(list.length > 0 && found.length === 0, list.length === 0 ? "no text was read from the dock" : "still named: " + found.join(", "), { found });
}

// av1 と av2 (sync.py の結果。judge の記録そのもの) から区画ごとの A/V の平均の差を出す。判定はしない (傾きは人が出す)。
// ok は「両方の値が揃った区画が 1 つ以上ある」。
// deltaLimitMs を渡すと (--extra-checks のとき)、両方に値のある区画ごとに |差| < deltaLimitMs を当て
// (withinLimit)、ok は「値の揃った区画が 1 つ以上あり、どれもしきい値の内」。渡さなければ差を出すだけ。
export function judgeAvDrift({ first, second, deltaLimitMs }) {
  const plain = judgeAvDriftCells({ first, second });
  if (typeof deltaLimitMs !== "number") return plain;
  const cells = plain.cells.map((c) => ({ ...c, withinLimit: c.deltaMs === null ? null : Math.abs(c.deltaMs) < deltaLimitMs }));
  const over = cells.filter((c) => c.withinLimit === false).map((c) => c.cell);
  const ok = plain.measuredCells > 0 && over.length === 0;
  const reason = plain.measuredCells === 0 ? plain.reason : "av2 - av1 is " + deltaLimitMs + " ms or more on cell " + over.join(", ");
  return result(ok, reason, { cells, measuredCells: plain.measuredCells, deltaLimitMs });
}

function judgeAvDriftCells({ first, second }) {
  const cells = (judged) => {
    if (!judged) return [];
    const list = Array.isArray(judged.perCell) ? judged.perCell : judged.data ? [{ cell: 0, data: judged.data }] : [];
    return list.map((c) => {
      const m = c && c.data && c.data.audio_minus_video_ms;
      const mean = m && typeof m.mean === "number" && Number.isFinite(m.mean) ? m.mean : null;
      const paired = c && c.data && typeof c.data.paired === "number" ? c.data.paired : null;
      return { cell: c.cell, mean, paired };
    });
  };
  const a = cells(first);
  const b = cells(second);
  const out = [];
  for (const ca of a) {
    const cb = b.find((x) => x.cell === ca.cell);
    const both = ca.mean !== null && cb && cb.mean !== null;
    out.push({ cell: ca.cell, firstMeanMs: ca.mean, secondMeanMs: cb ? cb.mean : null, firstPaired: ca.paired, secondPaired: cb ? cb.paired : null, deltaMs: both ? cb.mean - ca.mean : null });
  }
  const measured = out.filter((c) => c.deltaMs !== null);
  return result(measured.length > 0, !first || !second ? "av1 or av2 was not judged" : "no cell has a paired A/V value in both recordings", { cells: out, measuredCells: measured.length });
}

// ---- 判定の純粋関数 (run-window.mjs が呼ぶ) ------------------------------------------

// 判定ツールに渡す区画を決める。captures は同じ録画について取った screenCellRegions の結果の並び (録画の始めと終わり)。
// 区画は取れたものの**重なり** (始めにはまだ大きさがない・途中で共有の大きさが変わった、のどちらでも、両方の時点で
// 枠が占めていた矩形だけを見る)。1 つも取れなかった区画は、仮定した区画 (枠が 2 つ以上なら 960x540 の 4 象限、1 つなら
// 画面全体) にして regionSource に "assumed-quadrant" / "full-frame" と書く (仮定したことを regionSource に残す)。
export function cellRegionsForJudge({ captures, cells }) {
  const usable = (captures || []).filter((c) => c && Array.isArray(c.cells));
  const canvas = (usable.find((c) => c.canvas) || {}).canvas || { width: 1920, height: 1080 };
  const n = Math.max(1, Number(cells) || 1);
  // 区画の出どころ (シーンの項目か、合成のソースの配置の行か)。区画に書いて、判定ツールの引数の側が読み分ける。
  const origin = (usable.find((c) => typeof c.regionSource === "string") || {}).regionSource || "composite-layout";
  const out = [];
  for (let i = 0; i < n; i += 1) {
    const found = usable.map((c) => c.cells.find((r) => r.cell === i)).filter(Boolean);
    if (found.length === 0) {
      if (n <= 1) {
        out.push({ cell: i, x: 0, y: 0, w: canvas.width, h: canvas.height, bitCell: BIT_CELL_SOURCE_PX, regionSource: "full-frame" });
      } else {
        const qw = Math.floor(canvas.width / 2);
        const qh = Math.floor(canvas.height / 2);
        out.push({ cell: i, x: (i % 2) * qw, y: Math.floor(i / 2) * qh, w: qw, h: qh, bitCell: Math.round(BIT_CELL_SOURCE_PX / 2), regionSource: "assumed-quadrant" });
      }
      continue;
    }
    const x0 = Math.max(...found.map((r) => r.x));
    const y0 = Math.max(...found.map((r) => r.y));
    const x1 = Math.min(...found.map((r) => r.x + r.w));
    const y1 = Math.min(...found.map((r) => r.y + r.h));
    const last = found[found.length - 1];
    const changed = found.some((r) => r.x !== last.x || r.y !== last.y || r.w !== last.w || r.h !== last.h);
    if (x1 - x0 < 1 || y1 - y0 < 1) {
      out.push({ cell: i, x: last.x, y: last.y, w: last.w, h: last.h, bitCell: last.bitCell, regionSource: origin + "-last (no overlap between captures)", changed, captures: found.length });
      continue;
    }
    out.push({ cell: i, x: x0, y: y0, w: x1 - x0, h: y1 - y0, bitCell: Math.min(...found.map((r) => r.bitCell)), regionSource: origin, changed, captures: found.length });
  }
  return out;
}

// 判定ツールの引数 (区画ごとの呼び出しの並び)。frames.py は区画の矩形 (時計つきの帯を探す範囲) と帯のセルの一辺 (区画の
// 左上を帯の左上として読む)、sync.py は白を見る矩形、smooth.py は 1 回の呼び出しで全部の区画 (仮定した区画しか無ければ
// 枠の数だけ)。cells が 1 つで画面全体なら引数なし (frames.py は画面全体を探す)。
// frames.py は帯を区画の中だけで探す (bit_strip.py)。左上だけを渡して右下まで探すと、区画の外の隣の枠まで探して、
// 隣の人の帯を拾うため。
export function judgeCellArgs(tool, regions) {
  const list = regions || [];
  if (tool === "smooth.py") {
    // 1 つでも枠の実際の位置が取れていれば全部の区画を渡す (取れなかった区画は仮定した象限のまま渡る)。全部を
    // 仮定した区画に戻すと、枠 3 つのうち 1 つだけ大きさが取れない場合に、取れた 2 つまで 960x540 で読むことになる。
    const known = list.some((r) => String(r.regionSource || "").startsWith("scene-items") || String(r.regionSource || "").startsWith("composite-layout"));
    if (!known) return [[String(Math.max(1, list.length))]];
    const cell = Math.min(...list.map((r) => r.bitCell));
    return [[String(list.length), "--regions", list.map((r) => [r.x, r.y, r.w, r.h].join(",")).join(";"), "--cell", String(cell)]];
  }
  return list.map((r) => {
    if (r.regionSource === "full-frame") return [];
    if (tool === "frames.py") return ["--x", String(r.x), "--y", String(r.y), "--w", String(r.w), "--h", String(r.h), "--cell", String(r.bitCell)];
    return ["--x", String(r.x), "--y", String(r.y), "--w", String(r.w), "--h", String(r.h)];
  });
}

// プラグインの記録の `[cordmux] stats video received_hz=` の行から直近 windowLines 行を読み、映像が止まっているか
// (どれも stoppedHz 未満) を出す。行が足りなければ framesStopped は null (分からない)。
export function parseVideoRateSamples(lines, windowLines = VIDEO_RATE_WINDOW_LINES, stoppedHz = VIDEO_STOPPED_HZ) {
  const samples = [];
  for (const raw of lines || []) {
    const line = String(raw);
    const m = /\[cordmux\] [Ss]tats video received_hz=([0-9.]+)/.exec(line);
    if (!m) continue;
    const colon = line.indexOf(": [cordmux]");
    samples.push({ at: colon > 0 ? line.slice(0, colon) : null, receivedHz: Number(m[1]) });
  }
  const recent = samples.slice(-Math.max(1, windowLines));
  const framesStopped = recent.length < windowLines ? null : recent.every((s) => s.receivedHz < stoppedHz);
  return { samples: recent, framesStopped, windowLines, stoppedHz };
}

// 案内 (names の文言) がドックの文 (texts) に出ていることと、映像が止まっていること (videoRate) が一致するか。
// 映像が止まらず案内も出ない場合も一致 (合格) とする。前提 (ミュートで映像が止まる) が崩れても、案内の有無が映像と一致していれば合格にする。
export function judgeNoticeIffFramesStopped({ texts, names, videoRate }) {
  const list = Array.isArray(texts) ? texts.map(String) : [];
  const wanted = (names || []).map(String).filter(Boolean);
  const framesStopped = videoRate && typeof videoRate.framesStopped === "boolean" ? videoRate.framesStopped : null;
  const receivedHz = videoRate && Array.isArray(videoRate.samples) ? videoRate.samples.map((s) => s.receivedHz) : [];
  const noticePresent = list.length > 0 && wanted.length > 0 ? wanted.some((n) => list.some((t) => t.includes(n))) : null;
  const extra = { noticePresent, framesStopped, receivedHz };
  if (list.length === 0) return result(false, "no text was read from the dock", extra);
  if (wanted.length === 0) return result(false, "the notice text is unknown", extra);
  if (framesStopped === null) return result(false, "no video stats line was read from the OBS log", extra);
  const ok = noticePresent === framesStopped;
  return result(ok, noticePresent ? "the notice is shown although video still arrives (" + receivedHz.join(", ") + " Hz)" : "video stopped (" + receivedHz.join(", ") + " Hz) but the notice is not shown", extra);
}

// call-1 の OBS の録画を、cleanup.ps1 -KeepRecordings が残す場所へ移すための、from と to の組み合わせ。recordings は label -> 録画のパス。
// 行き先は `<windowOutDir>/recordings/obs-<tag>/<label>-<元の名前>` (区切りは / で組む。Node の API を呼ばない)。
export function obsRecordingMoves({ recordings, windowOutDir, tag }) {
  const dir = String(windowOutDir || "").replace(/[\\/]+$/, "") + "/" + OBS_RECORDINGS_KEEP_DIR_PREFIX + String(tag || "untagged");
  const moves = [];
  for (const [label, from] of Object.entries(recordings || {})) {
    if (typeof from !== "string" || !from) continue;
    const base = from.split(/[\\/]/).pop();
    moves.push({ label, from, to: dir + "/" + label + "-" + base });
  }
  return { dir, moves };
}

// ---- --extra-checks の純粋関数 -----------------

// (a) 送信部の接続が、保存した時点 (count-connections) から切れていないか。before / now は { hello, close } (countConnectionEvents の
// 数)。close が増えた (切れた) か hello が増えた (切れて接続し直した = Vesktop がクラッシュして再起動した・送信部が作り直された) なら失敗。
// 保存した数が無ければ判断できないので、失敗にする。
export function judgeConnectionsKept({ before, now }) {
  const valid = (c) => c && typeof c.hello === "number" && typeof c.close === "number";
  if (!valid(before)) return result(false, "no baseline (count-connections did not run)", { before: before || null, now: now || null, closes: null, hellos: null });
  if (!valid(now)) return result(false, "the OBS log could not be read", { before, now: now || null, closes: null, hellos: null });
  const closes = now.close - before.close;
  const hellos = now.hello - before.hello;
  const reason = closes > 0 ? "the connection closed " + closes + " time(s) since the baseline" : "the sender connected again " + hellos + " time(s) since the baseline";
  return result(closes === 0 && hellos === 0, reason, { before, now, closes, hellos });
}

// (b) [バージョン情報] の「バージョン」の表の 1 行: 行の名前 (ini の値。names の 1 つ目) の隣のテキストが値。値が空か「-」か、読めない値の
// 読み上げの名前 (unavailable の 1 つ目 = 「取得できません」) か、数字を含まないなら失敗。texts は dialog-click.ps1 -ListOnly のテキスト。
// ラベルと値の順に頼らない。labels (表のラベルの全部) にあるテキストは値と見なさず、reference (いつもバージョンのテキストが出る行 =
// cordmux (OBS)。表の 1 行目) の前後でラベルと値のどちらが先か (orientation) を確認してから読む。「ラベル、値、ラベル、値」と「値、ラベル、値、ラベル」は
// 表の中ほどでは同じ並びに見え、違うのは表の端だけなので、1 行目のラベルの前後で決める: 後ろだけがバージョンのテキスト (数字を含みラベルでない) なら
// ラベルが先 (前は表の見出し)、前だけがバージョンのテキストなら値が先。両方ともバージョンのテキストか、どちらも違えば決められない (失敗)。reference を渡さない
// 呼び出しは「ラベルの次が値」と仮定する (orientation に "label-first (assumed)" と書く)。読めた全部の行の値も rows に返す
// (リハーサルで cordmux (OBS) の行のバージョンのテキストを見る)。値が読めない行は、製品が値の読み上げの名前を unavailable にする
// (settings_window.cpp の update_about) ので、UI Automation で読むテキストは「-」ではなくその文字列になる。
export function judgeVersionRowShown({ texts, names, labels, reference, unavailable }) {
  const list = Array.isArray(texts) ? texts.map((t) => String(t).trim()) : [];
  const label = (names || [])[0] ? String(names[0]).trim() : null;
  if (list.length === 0) return result(false, "no text was read from the settings window", { value: null, orientation: null });
  if (!label) return result(false, "the row name is unknown", { value: null, orientation: null });
  const refLabel = (reference || [])[0] ? String(reference[0]).trim() : null;
  const known = new Set([...(labels || []), label, refLabel].map((x) => String(x || "").trim()).filter(Boolean));
  const unavailableText = (unavailable || [])[0] ? String(unavailable[0]).trim() : null;
  const isValue = (t) => typeof t === "string" && !known.has(t);
  const readable = (t) => typeof t === "string" && t !== "" && t !== "-" && t !== unavailableText && /[0-9]/.test(t);
  let orientation = "label-first (assumed)";
  if (refLabel) {
    const ref = list.indexOf(refLabel);
    if (ref < 0) return result(false, "the reference row " + refLabel + " is not shown", { value: null, orientation: null });
    const versionAfter = isValue(list[ref + 1]) && readable(list[ref + 1]);
    const versionBefore = ref > 0 && isValue(list[ref - 1]) && readable(list[ref - 1]);
    if (versionAfter && !versionBefore) orientation = "label-first";
    else if (versionBefore && !versionAfter) orientation = "value-first";
    else return result(false, "the order of labels and values could not be confirmed with the row " + refLabel, { value: null, orientation: null });
  }
  const offset = orientation === "value-first" ? -1 : 1;
  const valueOf = (name) => {
    const i = list.indexOf(name);
    if (i < 0) return null;
    const v = list[i + offset];
    return isValue(v) ? v : "";
  };
  const rows = {};
  for (const name of known) {
    const v = valueOf(name);
    if (v !== null) rows[name] = v;
  }
  const value = valueOf(label);
  if (value === null) return result(false, "the row " + label + " is not shown", { value: null, orientation, rows });
  return result(readable(value), "the value of the row " + label + " is " + JSON.stringify(value), { value, orientation, rows });
}

// (c) 使う声の確認。checks は VOICE_SOURCE_CHECKS の並びに、録った音 (tone_scan.py の rms と tone_share) と、鳴らす声の枠が
// 見つかったか (targetFound。ドックの行の押下では見ないので null) を足した物。1 本ごとに「1 kHz が聞こえた (heard)」を true / false / null (決められない)
// にし、expectTone と合うかを見る。置き換えた後に画面共有している人の枠で 1 kHz が聞こえることが芯なので、そこが決められなければ合格にしない。
// 枠が見つからない録画は null (例: 戻した後に画面共有している人の声の枠がない = 画面共有している人のマイクがミュートで、その人の声のストリームが来ていない)。
export function judgeVoiceSourceSwitch({ checks }) {
  const parts = (checks || []).map((c) => {
    const base = { label: c && c.label, expectTone: c && c.expectTone, targetFound: c ? c.targetFound ?? null : null, rms: c ? c.rms ?? null : null, toneShare: c ? c.toneShare ?? null : null };
    if (!c || c.targetFound === false) return { ...base, heard: null, matches: null, note: "the voice frame was not found" };
    if (typeof c.rms !== "number" || typeof c.toneShare !== "number") return { ...base, heard: null, matches: null, note: c.error || "the recording was not scanned" };
    const heard = c.toneShare >= VOICE_SOURCE_HEARD_TONE_SHARE && c.rms >= TONE_AUDIBLE_RMS ? true : c.toneShare < VOICE_SOURCE_QUIET_TONE_SHARE || c.rms < TONE_SILENT_RMS ? false : null;
    return { ...base, heard, matches: heard === null ? null : heard === Boolean(c.expectTone), note: heard === null ? "between heard and quiet" : null };
  });
  const core = parts.find((p) => p.label === VOICE_SOURCE_CHECKS[0].label);
  const mismatched = parts.filter((p) => p.matches === false).map((p) => p.label);
  const ok = mismatched.length === 0 && Boolean(core) && core.matches === true;
  const reason = mismatched.length > 0 ? "not as expected: " + mismatched.join(", ") : "the owner's voice frame after the switch could not be judged";
  return result(ok, reason, { parts });
}
// 声の音量: 100% の録画と pct% にした後の録画の、同じ区間の実効値 (tone_scan.py --windows の rms) と 1 kHz の成分の割合から、
// 利得が pct / 100 倍 (dB で 20 log10) になったかを判定する。before と after は { rms, toneShare }。基準が鳴っていない・読めていないときは
// 判定しない (ok は null)。クリップしうる大きさのときは、測った比を実際の比の下限としてだけ使う (下の説明)。
export function judgeVoiceVolume({ before, after, pct = VOICE_VOLUME_PCT, toleranceDb = VOICE_VOLUME_GAIN_TOLERANCE_DB } = {}) {
  const round2 = (x) => Math.round(x * 100) / 100;
  const num = (v) => (typeof v === "number" && Number.isFinite(v) ? v : null);
  const b = { rms: num(before && before.rms), toneShare: num(before && before.toneShare) };
  const a = { rms: num(after && after.rms), toneShare: num(after && after.toneShare) };
  const expectedDb = round2(20 * Math.log10(pct / 100));
  const base = { pct, expectedDb, toleranceDb, before: b, after: a };
  if (b.rms === null || a.rms === null) return result(null, "a recording was not scanned (before or after)", { ...base, decidable: false });
  if (b.rms < TONE_AUDIBLE_RMS || b.toneShare === null || b.toneShare < VOICE_SOURCE_HEARD_TONE_SHARE) {
    return result(null, "the tone was not heard at 100% (nothing to compare with)", { ...base, decidable: false });
  }
  if (b.rms * (pct / 100) * Math.SQRT2 >= 1) {
    // クリップしうる大きさ: クリップすると波の実効値は元より小さくなる (波を頂点で抑える単調な操作) ので、測った比は実際の比の下限。
    // 下限が期待の幅の下端以上なら「少なくともそれだけ上がった」は言える。上限は言えない (幅の上端は確認しない)。
    // 下端に届かないときは、クリップして低く出ただけかもしれないので判定しない。
    const lowerBoundDb = a.rms > 0 ? round2(20 * Math.log10(a.rms / b.rms)) : null;
    if (lowerBoundDb !== null && lowerBoundDb >= expectedDb - toleranceDb) {
      return result(true, "", { ...base, decidable: true, clipped: true, gainDb: lowerBoundDb, gainIsLowerBound: true, upperChecked: false, errorDb: null, note: "the louder recording may clip, so the measured gain " + lowerBoundDb + " dB is a lower bound of the true gain; it is at least the lower edge of the range, and the upper edge was not checked" });
    }
    return result(null, "the louder recording may clip, so the ratio would read low", { ...base, decidable: false, clipped: true, gainDb: lowerBoundDb, gainIsLowerBound: true });
  }
  if (a.rms <= 0) return result(false, "the recording after the change is silent", { ...base, decidable: true, gainDb: null, errorDb: null });
  const gainDb = round2(20 * Math.log10(a.rms / b.rms));
  const errorDb = round2(gainDb - expectedDb);
  return result(Math.abs(errorDb) <= toleranceDb, "the gain is " + gainDb + " dB, " + expectedDb + " dB expected (tolerance " + toleranceDb + " dB)", { ...base, decidable: true, gainDb, errorDb });
}

// 実際のアバターの並び (--extra-checks の (g)): 画面共有していない参加者は、cordmux のソースの画像に名前ラベルの並び (アバターつき) で
// 出る。並びの 1 参加者ごとの名前ラベルができるたびに DEBUG の行が出る (src-obs/sources/composite_decor.cpp の
// 「Composite roster label ready: index=%zu ...」)。共有を止めてもらう前の行の数を保存し、止めた後に増えた行の番号を数える。
// テスト用の送信側はアバターを送らないので、実際の Discord のアバターで並ぶのは実際の通話だけ。
export const ROSTER_BASELINE_LABEL = "before-roster";
export const ROSTER_SHOT_LABEL = "roster-real";
// 共有を止めてから並びの名前ラベルが描かれるまでの猶予 (名前ラベルは別の担当が非同期に描く。ドックの周期より十分長い値)。
export const ROSTER_LABEL_SETTLE_MS = 10000;
// 実際の通話で並ぶと期待する人数は rosterExpectedPeople (docs/spec.md の並びの決まりから出す)。メインアカウントは、自分の枠が出ていなければ
// 並びに出る (並びを撮るのは自分の枠のチェックを外した後)。並びから外れるのは、別アカウント方式のサブアカウント (Vesktop のアカウント) だけ
// (dock/build_observation.cpp の composite_roster_people)。
const ROSTER_LABEL_READY_RE = /\[cordmux\] Composite roster label ready: index=(\d+)/;
export function rosterLabelReadyFromLog(lines) {
  const out = [];
  for (const raw of lines || []) {
    const m = ROSTER_LABEL_READY_RE.exec(String(raw));
    if (m) out.push({ index: Number(m[1]), t: obsLogSeconds(String(raw)) });
  }
  return out;
}

// 並んだ人の数は、合成のソースが並べた数が変わるたびに書く行 (src-obs/sources/composite_decor.cpp の
// 「Composite roster shows N participant(s)」。名前と userId は書かない) の最後の値で数える。名前ラベルの行
// (Composite roster label ready) の index は描き直しの処理の番号で、並びの人数ではない (3 人並んでいても index は 0 と 1 だけのことがある)
// ので、人数には使わない。baselineCount (共有を止めてもらう前の名前ラベルの行の数) は、止めた後に名前ラベルが描かれたかの記録にだけ使う。
// 成功の条件は、人数の行があり、最後の値が期待の人数と等しいこと。
const ROSTER_SHOWS_RE = /\[cordmux\] Composite roster shows (\d+) participant\(s\)\s*$/;

export function rosterShownFromLog(lines) {
  let last = null;
  for (const raw of lines || []) {
    const m = ROSTER_SHOWS_RE.exec(String(raw));
    if (m) last = { people: Number(m[1]), t: obsLogSeconds(String(raw)) };
  }
  return last;
}

export function judgeRosterLabels({ lines, baselineCount, expectedPeople } = {}) {
  const all = rosterLabelReadyFromLog(lines);
  const base = Math.max(0, Number(baselineCount) || 0);
  const added = all.slice(base);
  const shown = rosterShownFromLog(lines);
  const people = shown ? shown.people : null;
  const expectedKnown = Number.isInteger(expectedPeople);
  const complete = expectedKnown && people === expectedPeople;
  const reason = !expectedKnown
    ? "the expected number of people was not given"
    : shown === null
      ? "no roster count line in the OBS log (the plugin may be older than the line), so the number of people cannot be told; read the picture"
      : complete
        ? ""
        : "the roster shows " + people + " people, expected " + expectedPeople;
  return result(complete, reason, {
    baselineCount: base,
    totalLines: all.length,
    addedLines: added.length,
    people,
    peopleAt: shown ? shown.t : null,
    expectedPeople: expectedKnown ? expectedPeople : null,
    complete,
  });
}

// 送信側が自分のアカウントを届けたか (--recheck)。受信側は facts を受け取るたびに DEBUG の行を 1 行書く
// (src-obs/pipeline/pipeline.cpp の handle_facts の「Facts: roster=.. sharing=.. warnings=.. stages=.. self=yes|-」。userId は書かない)。
// 送信側が自分のアカウントを読めていなければ self=-、届けていれば self=yes。Vesktop のログインが済んでいれば、接続の後の数秒で最初の
// yes が出る見込み (送信側の周期的な確認は 30 秒ごと)。待つ上限はこの値。
export const SELF_ACCOUNT_WAIT_TIMEOUT_MS = 120000;
export const SELF_ACCOUNT_POLL_MS = 1000;
const FACTS_SELF_RE = /\[cordmux\] Facts: .*\bself=(yes|-)(?:\s|$)/;

// afterHello は count-connections が保存した hello の数 (wait-connection の baseline と同じ)。それより後の最初の接続の行から後の Facts の行だけを
// 見る。接続より前の self=yes (前の接続の名残) は数えず、yesBeforeConnection に数だけ残す。
export function judgeSelfAccount({ lines, afterHello = 0 } = {}) {
  const list = lines || [];
  const need = Math.max(0, Number(afterHello) || 0) + 1;
  let seen = 0;
  let helloAt = -1;
  for (let i = 0; i < list.length; i += 1) {
    if (!endsWithAny(list[i], HELLO_MARKS)) continue;
    seen += 1;
    if (seen === need) {
      helloAt = i;
      break;
    }
  }
  let yesBeforeConnection = 0;
  let yes = 0;
  let dash = 0;
  let firstYesAfterSec = null;
  const helloT = helloAt >= 0 ? obsLogSeconds(String(list[helloAt])) : null;
  for (let i = 0; i < list.length; i += 1) {
    const m = FACTS_SELF_RE.exec(String(list[i]));
    if (!m) continue;
    if (helloAt < 0 || i < helloAt) {
      if (m[1] === "yes") yesBeforeConnection += 1;
      continue;
    }
    if (m[1] === "yes") {
      yes += 1;
      if (firstYesAfterSec === null) {
        const t = obsLogSeconds(String(list[i]));
        if (t !== null && helloT !== null) firstYesAfterSec = Math.round((t - helloT + (t < helloT ? 86400 : 0)) * 1000) / 1000;
      }
    } else dash += 1;
  }
  const reason =
    helloAt < 0
      ? "no connection line after the baseline"
      : "no Facts line with self=yes after the connection (" + dash + " lines with self=-)";
  return result(yes > 0, reason, { connected: helloAt >= 0, afterHello: need - 1, factsYes: yes, factsDash: dash, firstYesAfterSec, yesBeforeConnection });
}

// ログの行を正規表現で数える (judge-log-lines。--recheck の解像度の変化・エンコードの設定・自分の枠・画面共有の音)。lines は数える範囲の行
// (since の手順が保存した行数より後。切るのは実行スクリプト)。checks の 1 つ:
//   id        結果の名前
//   pattern   正規表現の元の文字列 (flags は任意)
//   min / max 合った行の数の下限 (既定 1) と上限 (無ければ上限なし)
//   group     最後に合った行のこの番号の群を値 (数に読めれば数) として last に残す
//   distinct  { key, value, min }: key の群が同じ行どうしで、value の群の種類が min 以上になる key があること
//   delta     { from, equals, check }: last が、from の label の judge-log-lines の結果 (previous[from]) の check (既定は同じ id) の last に
//             equals を足した値であること
// previous は label -> その label の結果 (checks の並びを持つ)。判定が全部合えば ok。
export function judgeLogLines({ lines, checks, previous } = {}) {
  const list = Array.isArray(lines) ? lines.map(String) : [];
  const prior = previous || {};
  const results = (checks || []).map((spec) => {
    let re = null;
    try {
      re = new RegExp(spec.pattern, spec.flags || "");
    } catch (e) {
      return { id: spec.id, ok: false, count: 0, last: null, reason: "invalid pattern: " + e.message };
    }
    const hits = [];
    for (const line of list) {
      const m = re.exec(line);
      if (m) hits.push(m);
    }
    const count = hits.length;
    const min = spec.min === undefined ? 1 : Number(spec.min);
    const problems = [];
    if (count < min) problems.push("matched " + count + " line(s), expected at least " + min);
    if (spec.max !== undefined && count > Number(spec.max)) problems.push("matched " + count + " line(s), expected at most " + spec.max);
    let last = null;
    if (spec.group !== undefined && count > 0) {
      const raw = hits[count - 1][Number(spec.group)];
      last = raw !== undefined && /^-?\d+(\.\d+)?$/.test(raw) ? Number(raw) : raw ?? null;
    }
    let distinctMax = null;
    if (spec.distinct) {
      const byKey = new Map();
      for (const m of hits) {
        const key = m[Number(spec.distinct.key)];
        if (!byKey.has(key)) byKey.set(key, new Set());
        byKey.get(key).add(m[Number(spec.distinct.value)]);
      }
      distinctMax = 0;
      for (const set of byKey.values()) distinctMax = Math.max(distinctMax, set.size);
      if (distinctMax < Number(spec.distinct.min)) problems.push("at most " + distinctMax + " distinct value(s) per key, expected " + spec.distinct.min);
    }
    let expectedLast = null;
    if (spec.delta) {
      const from = prior[spec.delta.from];
      const fromCheck = from && Array.isArray(from.checks) ? from.checks.find((c) => c.id === (spec.delta.check || spec.id)) : null;
      const base = fromCheck ? fromCheck.last : null;
      if (typeof base !== "number" || typeof last !== "number") {
        problems.push("cannot compare with '" + spec.delta.from + "' (before: " + String(base) + ", now: " + String(last) + ")");
      } else {
        expectedLast = base + Number(spec.delta.equals);
        if (last !== expectedLast) problems.push("last value " + last + " is not " + expectedLast + " (" + base + " in '" + spec.delta.from + "' " + (spec.delta.equals >= 0 ? "+" : "") + spec.delta.equals + ")");
      }
    }
    return {
      id: spec.id,
      ok: problems.length === 0,
      count,
      last,
      ...(distinctMax !== null ? { distinctMax } : {}),
      ...(expectedLast !== null ? { expectedLast } : {}),
      reason: problems.length === 0 ? null : problems.join("; "),
    };
  });
  const bad = results.filter((r) => !r.ok);
  return result(bad.length === 0, bad.map((r) => r.id + ": " + r.reason).join(" | "), { checks: results, lineCount: list.length });
}

// obs-websocket の InputVolumeMeters のメッセージの並び (ObsWs#events の d) から、入力 inputName の peak (dBFS) の最大を読む。
// メッセージの inputs[].inputLevelsMul は チャンネルごとの [magnitude, peak, input peak] (倍率)。peak の倍率を 20 log10 で dBFS にする
// (倍率 0 は -Infinity なので null で返し、無音と読む)。samples は inputName が載っていたメッセージの数。
export function inputPeakFromMeterEvents(events, inputName) {
  let samples = 0;
  let maxMul = 0;
  for (const ev of events || []) {
    const data = ev && ev.eventType === "InputVolumeMeters" ? ev.eventData : null;
    const inputs = data && Array.isArray(data.inputs) ? data.inputs : [];
    const hit = inputs.find((x) => x && x.inputName === inputName);
    if (!hit) continue;
    samples += 1;
    for (const channel of Array.isArray(hit.inputLevelsMul) ? hit.inputLevelsMul : []) {
      const peak = Array.isArray(channel) ? Number(channel[1]) : NaN;
      if (Number.isFinite(peak) && peak > maxMul) maxMul = peak;
    }
  }
  return { samples, maxPeakDbfs: maxMul > 0 ? Math.round(20 * Math.log10(maxMul) * 100) / 100 : null };
}

// 入力の音量メーターの判定: peak の最大がしきい値 (thresholdDbfs) より大きいこと。メッセージが 1 つも読めなかった、無音だった、はそれぞれ理由つきの失敗。
export function judgeInputLevel({ events, inputName, thresholdDbfs } = {}) {
  const { samples, maxPeakDbfs } = inputPeakFromMeterEvents(events, inputName);
  const threshold = Number.isFinite(Number(thresholdDbfs)) ? Number(thresholdDbfs) : INPUT_LEVEL_THRESHOLD_DBFS;
  const extra = { inputName: inputName ?? null, samples, maxPeakDbfs, thresholdDbfs: threshold };
  if (samples === 0) return result(false, "no InputVolumeMeters event carried the input '" + inputName + "' (the input has no audio or the subscription is off)", extra);
  if (maxPeakDbfs === null) return result(false, "the input was silent (no peak above 0 in " + samples + " events)", extra);
  return result(maxPeakDbfs > threshold, "the peak " + maxPeakDbfs + " dBFS is not above " + threshold + " dBFS", extra);
}

// (d) 録画ログ (readLedger の形 = { person, finalize, ... }) から、person (人の見えている名前) の画面の録画ファイル名を選ぶ。
// 人は person の行の display_name で引き、その user_id の finalize の行 (kind が screen) の output_file。名簿のないテスト用の送信側は
// display_name が空で、user_id がそのまま見えている名前なので、user_id でも引く。
export function pickPersonScreenOutput({ rows, person }) {
  if (!rows) return { file: null, reason: "the ledger was not read" };
  const name = String(person || "");
  const ids = new Set((rows.person || []).filter((p) => p && p.display_name === name).map((p) => String(p.user_id)));
  ids.add(name);
  const outputs = (rows.finalize || []).filter((f) => f && ids.has(String(f.user_id)) && String(f.kind || "screen") === "screen" && f.output_file);
  if (outputs.length === 0) return { file: null, reason: "no screen output of " + name + " in the ledger" };
  return { file: String(outputs[outputs.length - 1].output_file), reason: null };
}

// (e) OBS のログの行を読む。行の先頭は OBS の「HH:MM:SS.mmm: 」(その日の時刻)。日付をまたいだら (12 時間より大きく戻ったら) 1 日足す。
function obsLogSeconds(line) {
  const m = /^(\d{2}):(\d{2}):(\d{2})\.(\d{3}): /.exec(String(line));
  return m ? Number(m[1]) * 3600 + Number(m[2]) * 60 + Number(m[3]) + Number(m[4]) / 1000 : null;
}

// ストリームごとの整列の stats 行 (src-obs/pipeline/pipeline.cpp の「Stats stream=%u align period_us=... offset_us=...」) を、ストリームごとの並びにする。
export function alignStatsFromLog(lines) {
  const byStream = new Map();
  let dayOffset = 0;
  let last = null;
  for (const raw of lines || []) {
    const line = String(raw);
    const m = /\[cordmux\] Stats stream=(\d+) align period_us=([0-9.]+) frames=(-?\d+) realign=(-?\d+) .*?\boffset_us=(-?\d+)/.exec(line);
    if (!m) continue;
    let t = obsLogSeconds(line);
    if (t === null) continue;
    if (last !== null && t + dayOffset < last - 12 * 3600) dayOffset += 24 * 3600;
    t += dayOffset;
    last = t;
    const id = Number(m[1]);
    if (!byStream.has(id)) byStream.set(id, []);
    byStream.get(id).push({ t, periodUs: Number(m[2]), frames: Number(m[3]), realign: Number(m[4]), offsetUs: Number(m[5]) });
  }
  return byStream;
}

// 1 ストリームの並びを、frames が減った所で区切る。frames が減るのは整列を作り直したとき (枠の付け外しで枠の
// 中身が替わるとデコードと整列を作り直す = src-obs/pipeline/pipeline.cpp の needs_screen_stream_recreate。参加者の行の
// チェックを外して戻すと起きる)。区切らずに 1 本の直線を当てると offset_us と frames が途中で段になり、一様に溜まる形でも
// 傾きが薄まり、realign の差も負になりうる。
export function alignSegments(samples) {
  const segments = [];
  let current = [];
  for (const s of samples || []) {
    if (current.length > 0 && s.frames < current[current.length - 1].frames) {
      segments.push(current);
      current = [];
    }
    current.push(s);
  }
  if (current.length > 0) segments.push(current);
  return segments;
}

// 1 ストリームの並びの傾き (最小二乗。offset_us の秒あたりの変化) と大きさ。slopePercent は時間に対する割合 (%)。
// 傾きを読むのは、フレームが進んでいた (frames が増えた) 行だけ (共有が止まっている間の行は同じ値が並び、傾きを薄める)。
// frames が減った所 (整列の作り直し) で区切り (alignSegments)、ALIGN_DRIFT_MIN_SPAN_S 以上続いた区切りごとに傾きを出して、
// 絶対値の最大で判定する。戻り値の上位の値 (slope*・spanSeconds) は判定に使った区切り (傾きの絶対値が最大の物) の値で、
// samples と realigns は区切りの和。区切りごとの値は segments。
export function alignDriftOf(samples, opts) {
  const o = opts || {};
  const maxPercent = typeof o.maxDriftPercent === "number" ? o.maxDriftPercent : ALIGN_DRIFT_MAX_PERCENT;
  const minSpan = typeof o.minSpanSeconds === "number" ? o.minSpanSeconds : ALIGN_DRIFT_MIN_SPAN_S;
  const segments = alignSegments(samples).map((seg) => alignSegmentDrift(seg, maxPercent, minSpan));
  const all = (samples || []).filter((s) => s && typeof s.offsetUs === "number");
  const periods = all.map((s) => s.periodUs).filter((v) => v > 0).sort((a, b) => a - b);
  const judged = segments.filter((s) => s.judged);
  const candidates = judged.length > 0 ? judged : segments.filter((s) => s.slopePercent !== null);
  const worst = candidates.reduce((a, b) => (a === null || Math.abs(b.slopePercent) > Math.abs(a.slopePercent) ? b : a), null);
  const longest = segments.reduce((a, b) => (a === null || b.spanSeconds > a.spanSeconds ? b : a), null);
  const shown = worst || longest;
  return {
    samples: segments.reduce((sum, s) => sum + s.samples, 0),
    spanSeconds: shown ? shown.spanSeconds : 0,
    medianPeriodMs: periods.length > 0 ? Math.round(periods[Math.floor(periods.length / 2)] / 100) / 10 : null,
    firstOffsetMs: all.length > 0 ? Math.round(all[0].offsetUs / 100) / 10 : null,
    lastOffsetMs: all.length > 0 ? Math.round(all[all.length - 1].offsetUs / 100) / 10 : null,
    maxAbsOffsetMs: all.length > 0 ? Math.round(Math.max(...all.map((s) => Math.abs(s.offsetUs))) / 100) / 10 : null,
    realigns: segments.length > 0 ? segments.reduce((sum, s) => sum + (s.realigns || 0), 0) : null,
    slopeMsPerMinute: worst ? worst.slopeMsPerMinute : null,
    slopePercent: worst ? worst.slopePercent : null,
    judged: judged.length > 0,
    ok: judged.length > 0 ? judged.every((s) => s.ok) : null,
    restarts: Math.max(0, segments.length - 1),
    segments,
  };
}

// 区切り 1 つ分の傾き (alignDriftOf の segments の要素)。
function alignSegmentDrift(samples, maxPercent, minSpan) {
  const list = (samples || []).filter((s, i, all) => i === 0 || s.frames > all[i - 1].frames);
  const n = list.length;
  const spanSeconds = n >= 2 ? list[n - 1].t - list[0].t : 0;
  const periods = list.map((s) => s.periodUs).filter((v) => v > 0).sort((a, b) => a - b);
  const medianPeriodMs = periods.length > 0 ? Math.round(periods[Math.floor(periods.length / 2)] / 100) / 10 : null;
  const offsets = list.map((s) => s.offsetUs);
  const out = {
    samples: n,
    spanSeconds: Math.round(spanSeconds),
    medianPeriodMs,
    firstOffsetMs: n > 0 ? Math.round(offsets[0] / 100) / 10 : null,
    lastOffsetMs: n > 0 ? Math.round(offsets[n - 1] / 100) / 10 : null,
    maxAbsOffsetMs: n > 0 ? Math.round(Math.max(...offsets.map((v) => Math.abs(v))) / 100) / 10 : null,
    realigns: n > 0 ? list[n - 1].realign - list[0].realign : null,
    slopeMsPerMinute: null,
    slopePercent: null,
    judged: false,
    ok: null,
  };
  if (n < 3 || spanSeconds <= 0) return out;
  const mt = list.reduce((a, s) => a + s.t, 0) / n;
  const mo = offsets.reduce((a, v) => a + v, 0) / n;
  let num = 0;
  let den = 0;
  for (const s of list) {
    num += (s.t - mt) * (s.offsetUs - mo);
    den += (s.t - mt) * (s.t - mt);
  }
  const usPerSecond = den > 0 ? num / den : 0;
  out.slopeMsPerMinute = Math.round(usPerSecond * 60 / 100) / 10;
  out.slopePercent = Math.round((usPerSecond / 1e6) * 100 * 10000) / 10000;
  out.judged = spanSeconds >= minSpan;
  out.ok = out.judged ? Math.abs(usPerSecond / 1e6) * 100 <= maxPercent : null;
  return out;
}

// 送信側の段階の行 (src-vencord/index.ts の「video stream N: stage=S format=WxH@F」。OBS のログでは [cordmux/js] の行) を、ストリームごとにまとめる。
export function senderStagesFromLog(lines) {
  const byStream = new Map();
  for (const raw of lines || []) {
    const m = /\[cordmux\/js\] \S+ video stream (\d+): stage=(\d+) format=(\S+)/.exec(String(raw));
    if (!m) continue;
    const id = Number(m[1]);
    const prev = byStream.get(id) || { stream: id, stages: [], lastFormat: null };
    const stage = Number(m[2]);
    if (!prev.stages.includes(stage)) prev.stages.push(stage);
    prev.lastFormat = m[3];
    byStream.set(id, prev);
  }
  return [...byStream.values()].map((s) => ({ ...s, maxStage: Math.max(...s.stages), degraded: Math.max(...s.stages) > 1 }));
}

// アイコンを送らなかった行 (送信側の「avatar (userId=...): 送らない (理由)」) を理由ごとに数える。**userId は出さない** (人の数だけ)。
// あわせて、受信側がデコードできずに捨てたアイコンの数 (decodeFailures)。受信側の「Profiles: people=N avatar_decode_failures=M」と
// 「Avatar: stored=... people_with_avatar=N avatar_decode_failures=M」の M は、1 通ごとの数ではなく、その接続の間に捨てた数の合計で
// (src-obs/pipeline/profiles_state.h の avatar_decode_failures)、接続が切れると 0 に戻る (pipeline.cpp の handle_disconnected が
// profiles を捨て、その後に PROFILES_RESET_MARK の行を書く)。そこで、接続ごとの最後の M を足して、ログ全体で捨てた数にする。
// 切れ目に CLOSE_MARKS を使わないのは、応答が止まって切れたときの行 (「Disconnected from Vesktop: it stopped responding」) が
// そのマーカーで終わらないためである。
// 送信側の行の日本語の語は文字列の定数に置き、正規表現は文字列から組む (test/lint/identifiers_english.py は正規表現の字句を読まず、
// 中の日本語を識別子と見なすため。ほかの行のマーカー = HELLO_MARKS と同じく文字列で持つ)。
const AVATAR_NOT_SENT_MARK = "送らない";
const AVATAR_NOT_SENT_RE = new RegExp("avatar \\(userId=([^)]*)\\): " + AVATAR_NOT_SENT_MARK + " \\(([^)]*)\\)");
const AVATAR_DECODE_FAILURES_RE = /\[cordmux\] (?:Profiles: people=\d+|Avatar: stored=\w+ people_with_avatar=\d+) avatar_decode_failures=(\d+)/;
const PROFILES_RESET_MARK = "[cordmux] Disconnected from Vesktop; cleared screen share decoding and routes";
export function avatarIssuesFromLog(lines) {
  const reasons = new Map();
  let decodeFailures = 0;
  let inConnection = 0; // 現在の接続の間に捨てた数 (最後に読んだ M)
  for (const raw of lines || []) {
    const line = String(raw);
    const a = AVATAR_NOT_SENT_RE.exec(line);
    if (a) {
      const entry = reasons.get(a[2]) || { reason: a[2], lines: 0, people: new Set() };
      entry.lines += 1;
      entry.people.add(a[1]);
      reasons.set(a[2], entry);
      continue;
    }
    const p = AVATAR_DECODE_FAILURES_RE.exec(line);
    if (p) {
      inConnection = Number(p[1]);
      continue;
    }
    if (line.includes(PROFILES_RESET_MARK)) {
      decodeFailures += inConnection;
      inConnection = 0;
    }
  }
  decodeFailures += inConnection;
  return { notSent: [...reasons.values()].map((r) => ({ reason: r.reason, lines: r.lines, people: r.people.size })), decodeFailures };
}

// (e) の判定: 整列の傾きが上限の内か (ALIGN_DRIFT_MIN_SPAN_S 以上続いたストリームだけ)。フォールバック・アイコン・接続は数を残すだけで、ok には入れない
// (どれも人が読む情報。フォールバックは負荷の結果、アイコンは見た目の結果で、合否のしきい値をここで決めない)。傾きを読めるストリームが 1 つも無ければ失敗。
// opts.shortRunNotJudged (理由の文。リハーサル) があれば、傾きを読める長さのストリームが 1 つも無いときは失敗にせず判定しない (ok null)。
export function judgeCallLog(lines, opts) {
  const streams = [...alignStatsFromLog(lines).entries()].sort((a, b) => a[0] - b[0]).map(([stream, samples]) => ({ stream, ...alignDriftOf(samples, opts) }));
  const judged = streams.filter((s) => s.judged);
  const over = judged.filter((s) => s.ok === false).map((s) => s.stream);
  const short = opts && typeof opts.shortRunNotJudged === "string" && judged.length === 0;
  const reason = judged.length === 0 ? "no stream had stats lines long enough to read the drift" : "the alignment drift is over the limit on stream " + over.join(", ");
  return result(short ? null : judged.length > 0 && over.length === 0, short ? reason + " (not judged: " + opts.shortRunNotJudged + ")" : reason, {
    ...(short ? { notJudged: opts.shortRunNotJudged } : {}),
    align: streams,
    stages: senderStagesFromLog(lines),
    avatars: avatarIssuesFromLog(lines),
    connections: countConnectionEvents(lines),
  });
}

// ---- 実行計画と録画の判定の純関数 (run-window.mjs が呼ぶ) -----------------------------------------------

// 実行計画の中でキーを送る手順 (KEY_SENDING_KINDS)。--dry-run の出力の keySendingSteps。
// 再確認の実際の実行 (--skip-export-bundle) では 0 であること。
export function keySendingSteps(steps) {
  return (steps || [])
    .filter((s) => s && KEY_SENDING_KINDS.includes(s.kind))
    .map((s) => ({ id: s.id || null, kind: s.kind, label: s.label }));
}

// sync.py の傾き (trend) をしきい値で判定する。見分けたいのは、ずれが時間とともに溜まって広がる形 (長い配信で限りなく大きくなる。欠陥) と、
// 一時的に上下して戻る段 (溜まらない)。sync.py は段を外した基準の組の傾き (trend.baseline) と段の並び (trend.steps) を出す
// (sync.py の「一時的な段と、時間とともに広がる傾きを分ける」)。判定は基準の傾きで行い、段は数と高さを記録する (段で欠陥にはしない)。
// 基準がない前の形の出力は、全部の組の傾きで判定する (fitOn: all-pairs)。verdict は
//   defect        |傾き| > AV_TREND_DEFECT_MS_PER_MIN かつ 95% の幅が 0 を含まない (0 と分けられる) = 時間とともに広がる。欠陥
//   undetermined  |傾き| はしきい値を超えたが 95% の幅が 0 を含むか幅が出せない (ずれが段のように留まり、独立な組が少ない)、
//                 または録画の終わりが段の途中 (戻るかが分からない) = この録画では分けられない。長い通話で確認する
//   long-call     |傾き| がしきい値の内 = この録画では広がりと言えない。長い通話で確認する項目として残す
//   not-judged    組が少ないか短い (AV_TREND_MIN_PAIRS / AV_TREND_MIN_SPAN_S)、または前の形の出力が幅を出していない
export function judgeAvTrend(trend) {
  const t = trend || {};
  const b = t.baseline && typeof t.baseline === "object" ? t.baseline : null;
  const fit = b || t;
  const num = (x) => (typeof x === "number" && Number.isFinite(x) ? x : null);
  const slope = num(fit.slope_ms_per_min);
  const ci = Array.isArray(fit.slope_ci95_ms_per_min) && fit.slope_ci95_ms_per_min.length === 2 && fit.slope_ci95_ms_per_min.every((x) => typeof x === "number") ? fit.slope_ci95_ms_per_min : null;
  const pairs = typeof fit.pairs === "number" ? fit.pairs : 0;
  const span = typeof fit.span_s === "number" ? fit.span_s : 0;
  const steps = Array.isArray(t.steps) ? t.steps : null;
  const endsOff = t.ends_off_baseline === true;
  const base = {
    slopeMsPerMin: slope,
    ci95MsPerMin: ci,
    pairs,
    spanSeconds: span,
    limitMsPerMin: AV_TREND_DEFECT_MS_PER_MIN,
    fitOn: b ? "baseline" : "all-pairs",
    ...(b ? { allPairsSlopeMsPerMin: num(t.slope_ms_per_min), lag1Autocorr: num(b.lag1_autocorr), effectivePairs: num(b.effective_pairs) } : {}),
    steps: steps ? steps.map((x) => ({ startS: x.start_s, endS: x.end_s, pairs: x.pairs, heightMs: x.height_ms, returned: x.returned })) : null,
    stepCount: steps ? steps.length : null,
    endsOffBaseline: endsOff,
  };
  if (slope === null || pairs < AV_TREND_MIN_PAIRS || span < AV_TREND_MIN_SPAN_S) {
    return { ...base, verdict: "not-judged", note: "fewer than " + AV_TREND_MIN_PAIRS + " pairs or shorter than " + AV_TREND_MIN_SPAN_S + " s" + (b ? " (outside the steps)" : "") };
  }
  if (!ci && !b) return { ...base, verdict: "not-judged", note: "sync.py gave no 95% interval of the slope" };
  const separated = ci !== null && (ci[0] > 0 || ci[1] < 0);
  const over = Math.abs(slope) > AV_TREND_DEFECT_MS_PER_MIN;
  if (over && separated) return { ...base, verdict: "defect", note: null };
  if (over) {
    return { ...base, verdict: "undetermined", note: (ci ? "over the line but the 95% interval includes 0" : "over the line but the offset stays in levels (too few independent pairs for an interval)") + "; confirm in a long call" };
  }
  if (endsOff) return { ...base, verdict: "undetermined", note: "the recording ended during a step, so whether it returns is not known; confirm in a long call" };
  return { ...base, verdict: "long-call", note: "within the line in this recording; confirm in a long call" };
}

// sync.py の 1 セルの結果をしきい値で判定する。audio_minus_video_ms は「音の時刻 - 映像の時刻」(負 = 音が先。sync.py の先頭)。
// 平均が -AV_SYNC_AUDIO_LEAD_LIMIT_MS より大きく AV_SYNC_AUDIO_LAG_LIMIT_MS より小さければしきい値の内。組が無ければ ok は null
// (そのセルにテスト動画がない = 判定しない)。傾きが defect なら ok は false。
export function judgeAvSyncLines(data) {
  const d = data || {};
  const paired = typeof d.paired === "number" ? d.paired : 0;
  const m = d.audio_minus_video_ms || {};
  const mean = typeof m.mean === "number" && Number.isFinite(m.mean) ? m.mean : null;
  const lines = { leadLimitMs: AV_SYNC_AUDIO_LEAD_LIMIT_MS, lagLimitMs: AV_SYNC_AUDIO_LAG_LIMIT_MS };
  const trend = judgeAvTrend(d.trend);
  if (paired <= 0 || mean === null) return { ok: null, verdict: "no-pairs", paired, meanMs: null, ...lines, trend };
  // 組が少ないセルの平均は、声が同時に鳴るなどで組めた数組の値に引きずられるので判定しない (傾きと同じ下限)。
  if (paired < AV_MIN_PAIRS) {
    return { ok: null, verdict: "too-few-pairs", note: "fewer than " + AV_MIN_PAIRS + " pairs; the mean was not judged", paired, meanMs: mean, ...lines, trend };
  }
  const within = mean > -AV_SYNC_AUDIO_LEAD_LIMIT_MS && mean < AV_SYNC_AUDIO_LAG_LIMIT_MS;
  return {
    ok: within && trend.verdict !== "defect",
    verdict: within ? "within" : "outside",
    direction: mean < 0 ? "audio-leads" : "audio-lags",
    paired,
    meanMs: mean,
    minMs: typeof m.min === "number" ? m.min : null,
    maxMs: typeof m.max === "number" ? m.max : null,
    ...lines,
    trend,
  };
}

// sync.py の判定結果 (judge の結果。セルごとの perCell か 1 つの data) のセルをしきい値で判定する。組のあるセルが 1 つも
// 無ければ失敗 (テスト動画を共有しているセルで組が取れていない = 測れていない)。
// opts.person を渡すと、その人の枠 (opts.cell。テスト動画を共有している人) だけを判定し、ほかのセルは判定しない (verdict other-person)。
// 合成のソースの音は全員の音を混ぜた 1 本なので、ほかの人の枠の白 (画面全体の共有にテスト動画が写る) とテスト動画の音の組は、
// 映像と音のずれではなく、別の経路の映像の差を測る。枠が分からなければ (opts.cell が null) 判定しない (ok null。理由は opts.cellReason)。
export function judgeAvSync(judged, opts) {
  const o = opts || {};
  if (!judged) return result(false, "the sync.py result was not found", { cells: [], judgedCells: 0 });
  if (judged.ok === false) return result(false, "sync.py did not run: " + String(judged.error || "unknown"), { cells: [], judgedCells: 0 });
  const list = Array.isArray(judged.perCell) ? judged.perCell : judged.data ? [{ cell: 0, data: judged.data }] : [];
  const person = o.person || null;
  const target = person !== null && Number.isInteger(o.cell) ? o.cell : null;
  if (person !== null && target === null) {
    return result(null, "the frame of " + person + " was not found: " + String(o.cellReason || "unknown"), { person, personCell: null, cells: [], judgedCells: 0 });
  }
  const cells = list.map((c) => {
    const cell = c && c.cell;
    if (target !== null && cell !== target) return { cell, ok: null, verdict: "other-person", paired: c && c.data && typeof c.data.paired === "number" ? c.data.paired : null };
    return { cell, ...judgeAvSyncLines(c && c.data) };
  });
  const decided = cells.filter((c) => c.ok !== null);
  const failed = decided.filter((c) => c.ok === false).map((c) => c.cell);
  // 組はあるが下限に届かないセルだけのときは、測れていないので成功でも失敗でもない (null)。
  const fewPairs = cells.filter((c) => c.verdict === "too-few-pairs").map((c) => c.cell);
  const reason = decided.length === 0 ? (fewPairs.length > 0 ? "too few pairs to judge on cell " + fewPairs.join(", ") : "no cell has paired events (the test video was not measured)") : "outside the line on cell " + failed.join(", ");
  const ok = decided.length === 0 && fewPairs.length > 0 ? null : decided.length > 0 && failed.length === 0;
  return result(ok, reason, { cells, judgedCells: decided.length, ...(person !== null ? { person, personCell: target } : {}) });
}

// ---- 合成のソースの実行 (--composite) の判定 (純関数。run-window.mjs が呼ぶ) --------------------------------------
//
// 合成のソース cordmux_composite は、シーンに 1 つ置くと、その中に自分の枠 (左上) と参加者ごとの画面共有を描く。画面共有の映像は
// 枠ごとの公開のソースには渡らないので、判定の枠の位置は OBS のログの配置の行から取る:
//   Composite layout canvas=<w>x<h> count=<n> <tile>=<x>,<y>,<w>,<h> ...
// <tile> は self か slot<N> (枠の番号)。並びは左上・右上・左下・右下。人の名前は行にないので、枠の番号と人の対応は
// 製品がストリームの経路の変わるたびに OBS のログへ書く行から引く (下の harnessPersonsFromSlotStreams)。

// テスト用の送信側の画面の共有の幅 (test/tools/harness/sender.ts の SCREEN_WIDTH。1920x1080 固定)。タイルの矩形の幅からセルの一辺
// (frames.py の --cell。時計つきの帯が見つからないときの前の帯の読みだけが使う) を出すのに使う。
export const COMPOSITE_SHARE_WIDTH_PX = 1920;
// タイルの矩形が 4 等分のセルの内側に収まっていればよいとみなす、セルからの縮み (px)。製品の tile_gap_px の既定は 0 だが、
// 設定を変えた場合でも失敗にしないための幅 (すき間は両側で 2 回引かれる)。
export const COMPOSITE_GAP_TOLERANCE_PX = 64;

// OBS のログの 1 行が配置の行なら読んで返す (そうでなければ null)。置けなかった画面共有の行 (Composite layout has no room ...) は
// canvas= を持たないので読まない。count とタイルの数が合わない行は壊れた行として読まない。
export function parseCompositeLayoutLine(line) {
  const text = String(line ?? "");
  const m = /Composite layout canvas=(\d+)x(\d+) count=(\d+)((?: (?:self|slot\d+)=-?\d+,-?\d+,\d+,\d+)*)\s*$/.exec(text);
  if (!m) return null;
  const tiles = [...m[4].matchAll(/ (self|slot(\d+))=(-?\d+),(-?\d+),(\d+),(\d+)/g)].map((t) => ({
    name: t[1],
    self: t[1] === "self",
    slot: t[2] ? Number(t[2]) : 0,
    x: Number(t[3]),
    y: Number(t[4]),
    w: Number(t[5]),
    h: Number(t[6]),
  }));
  const count = Number(m[3]);
  if (tiles.length !== count) return null;
  return { canvas: { width: Number(m[1]), height: Number(m[2]) }, count, tiles, line: text.trim() };
}

// OBS のログの行の並びの、最後の配置の行 (無ければ null)。録画の前の最後の行を取るので、呼ぶ側がログの読み終わった所までを渡す。
export function lastCompositeLayout(lines) {
  const list = lines || [];
  for (let i = list.length - 1; i >= 0; i -= 1) {
    const parsed = parseCompositeLayoutLine(list[i]);
    if (parsed) return parsed;
  }
  return null;
}

// 表示名の自然順 (src-obs/ledger/natural_order.h の natural_compare と同じ規則): 数字の並びは数の大小、そのほかは ASCII の
// 英字の大文字と小文字を同じとして文字の番号の順。すべて同じなら元の文字列の順。負なら a が前。
export function naturalCompare(a, b) {
  const x = Array.from(String(a));
  const y = Array.from(String(b));
  const isDigit = (c) => c >= "0" && c <= "9";
  const fold = (c) => (c >= "A" && c <= "Z" ? c.toLowerCase() : c);
  let i = 0;
  let j = 0;
  while (i < x.length && j < y.length) {
    if (isDigit(x[i]) && isDigit(y[j])) {
      let ie = i;
      while (ie < x.length && isDigit(x[ie])) ie += 1;
      let je = j;
      while (je < y.length && isDigit(y[je])) je += 1;
      let is = i;
      while (is + 1 < ie && x[is] === "0") is += 1;
      let js = j;
      while (js + 1 < je && y[js] === "0") js += 1;
      const da = x.slice(is, ie).join("");
      const db = y.slice(js, je).join("");
      if (da.length !== db.length) return da.length < db.length ? -1 : 1;
      if (da !== db) return da < db ? -1 : 1;
      i = ie;
      j = je;
      continue;
    }
    const ca = fold(x[i]);
    const cb = fold(y[j]);
    if (ca !== cb) return ca.codePointAt(0) < cb.codePointAt(0) ? -1 : 1;
    i += 1;
    j += 1;
  }
  if (i < x.length) return 1;
  if (j < y.length) return -1;
  const sa = String(a);
  const sb = String(b);
  return sa === sb ? 0 : sa < sb ? -1 : 1;
}

// 合成のソースの実行では枠の公開のソースがないので、枠の番号と人の対応は製品がストリームの経路の変わるたびに OBS のログへ書く行から引く:
//   Screen slot <N> carries stream <M>   (枠 N にストリーム M が載った・替わった)
//   Screen slot <N> carries no stream    (枠 N が空いた)
// 人の名前は行にない。テスト用の送信側は stream_id を送信側全体の通し番号にしていて (test/tools/harness/sender.ts。画面 1..nScreen、声は続けて)、
// 画面共有の人 harness-screen-i のストリームの番号は i なので、ストリーム M の人は harness-screen-M とコピーする。
export function parseScreenSlotLine(line) {
  const m = /Screen slot (\d+) carries (?:stream (\d+)|no stream)\s*$/.exec(String(line ?? ""));
  if (!m) return null;
  return { slot: Number(m[1]), stream: m[2] === undefined ? null : Number(m[2]) };
}

// 行の並び (呼ぶ側が時点までを渡す) から、枠ごとの最後の読み取り結果を返す。枠 -> ストリームの番号 (空いた枠は null)。
export function screenSlotStreamsFromLines(lines) {
  const out = {};
  for (const raw of lines || []) {
    const p = parseScreenSlotLine(raw);
    if (p) out[p.slot] = p.stream;
  }
  return out;
}

// 枠 -> ストリームの番号を、テスト用の送信側の人の名前 (harness-screen-<M>) にコピーする。空いた枠は入れない。
export function harnessPersonsFromSlotStreams(streams) {
  const out = {};
  for (const [slot, stream] of Object.entries(streams || {})) {
    if (Number.isInteger(stream) && stream > 0) out[slot] = harnessPersonName("screen", stream);
  }
  return out;
}

// ---- 枠に載っている人を、OBS の記録の枠の割り当てと cordmux の録画の録画ログから引く ----------------------------------------------
// 実際の通話の OBS の記録には、人の名前を書かない (枠の番号とストリームの番号だけ: 「Screen slot N carries stream M」)。人とストリームの
// 対応は cordmux の録画の録画ログにある (person の行の user_id と display_name、segment の行の user_id・kind・stream_id と時刻)。
// ストリームの番号は再接続で別の人に割り当てられうる (docs/architecture.md の「録画ログ」) ので、対応は、判定する OBS の録画の区間
// (fromMs から toMs。システム時刻の ms) に重なる segment の行だけから取る。テスト用の送信側は user_id がそのまま名前なので、名前も
// user_id として引く (pickPersonScreenOutput と同じ)。user_id は戻り値に入れない (結果 JSON に書かない)。
// segment の行の時刻: 始まりは first_pts_wall_us、長さは last_pts_us - first_pts_us (閉じていない行は今も続いているとみなす)。
export function personScreenStreams({ ledger, person, fromMs, toMs }) {
  if (!ledger) return { streams: [], reason: "no cordmux recording ledger" };
  const name = String(person || "");
  const ids = new Set((ledger.person || []).filter((p) => p && p.display_name === name).map((p) => String(p.user_id)));
  ids.add(name);
  const fromUs = Number(fromMs) * 1000;
  const toUs = Number(toMs) * 1000;
  const overlaps = (seg) => {
    const start = Number(seg.first_pts_wall_us);
    if (!Number.isFinite(start)) return false;
    const open = seg.close_reason === "not_closed" || !Number.isFinite(Number(seg.last_pts_us));
    const end = open ? Infinity : start + Math.max(0, Number(seg.last_pts_us) - Number(seg.first_pts_us));
    return start < toUs && end > fromUs;
  };
  const screens = (ledger.segment || []).filter((seg) => seg && String(seg.kind || "") === "screen" && Number.isInteger(Number(seg.stream_id)) && overlaps(seg));
  const mine = new Set(screens.filter((seg) => ids.has(String(seg.user_id))).map((seg) => Number(seg.stream_id)));
  // 同じ区間に別の人が同じストリームの番号を持っていたら、その番号では人を決められない。
  const others = new Set(screens.filter((seg) => !ids.has(String(seg.user_id))).map((seg) => Number(seg.stream_id)));
  const streams = [...mine].filter((id) => !others.has(id)).sort((a, b) => a - b);
  const reason =
    mine.size === 0
      ? "the cordmux recording has no screen segment of " + name + " during the recording"
      : streams.length === 0
        ? "the stream of " + name + " was also used by another person during the recording"
        : null;
  return { streams, reason };
}

// 録画の始めと終わりに読んだ枠 (captures の cells の stream) のうち、streams のどれかを運んでいた枠。始めと終わりの両方で同じ枠に
// 載っていたときだけ、その枠 (cell = 枠の番号 - 1) を返す (途中で枠を移った人の区画は 1 つに決まらない)。
export function personCellOf({ captures, streams }) {
  const wanted = new Set((streams || []).map(Number));
  const usable = (captures || []).filter((c) => c && Array.isArray(c.cells));
  if (usable.length === 0) return { cell: null, reason: "the frames were not read at the start and the end of the recording" };
  if (wanted.size === 0) return { cell: null, reason: "no stream to look for" };
  const seen = usable.map((c) => {
    const hit = c.cells.filter((x) => wanted.has(Number(x.stream)));
    return hit.length === 1 ? hit[0].cell : null;
  });
  if (seen.some((c) => c === null)) return { cell: null, reason: "the stream was not on exactly one frame at the " + usable.map((c, i) => (seen[i] === null ? c.when || "?" : null)).filter(Boolean).join(" and ") + " of the recording" };
  if (new Set(seen).size !== 1) return { cell: null, reason: "the person moved to another frame during the recording" };
  return { cell: seen[0], reason: null };
}

// 配置の行のタイルを、判定ツールのセル (screenCellRegions と同じ形) にする。cell は枠の番号 - 1 (人との対応は枠の番号で引く)。
// 矩形は OBS の基本の解像度 (配置の行の canvas) の上の値で、録画の上の値へは GetVideoSettings の output / base の比で補正する
// (screenCellRegions と同じ丸め)。sourceName は、枠ごとの公開のソースがないので常に null (人の枠は persons から引く)。自分の枠は
// cells に入れず self に置く。
// slotStreams (枠の番号 -> ストリームの番号。screenSlotStreamsFromLines) を渡すと、セルにストリームの番号 (stream) も入れる (personCellOf が引く)。
export function compositeCellRegions({ layout, videoSettings, persons, slotStreams }) {
  if (!layout) return { ok: false, regionSource: "composite-layout", canvas: null, cells: [], skipped: [], self: null, error: "no Composite layout line in the OBS log" };
  const vs = videoSettings || {};
  const baseW = layout.canvas.width > 0 ? layout.canvas.width : Number(vs.baseWidth) > 0 ? Number(vs.baseWidth) : 1920;
  const baseH = layout.canvas.height > 0 ? layout.canvas.height : Number(vs.baseHeight) > 0 ? Number(vs.baseHeight) : 1080;
  const outW = Number(vs.outputWidth) > 0 ? Number(vs.outputWidth) : baseW;
  const outH = Number(vs.outputHeight) > 0 ? Number(vs.outputHeight) : baseH;
  const sx = outW / baseW;
  const sy = outH / baseH;
  const cells = [];
  const skipped = [];
  let self = null;
  for (const tile of layout.tiles) {
    const x0 = Math.max(0, Math.ceil(tile.x * sx));
    const y0 = Math.max(0, Math.ceil(tile.y * sy));
    const x1 = Math.min(outW, Math.floor((tile.x + tile.w) * sx));
    const y1 = Math.min(outH, Math.floor((tile.y + tile.h) * sy));
    const rect = { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
    if (rect.w < 1 || rect.h < 1) {
      skipped.push({ cell: tile.self ? null : tile.slot - 1, sourceName: null, reason: "the tile is outside the canvas" });
      continue;
    }
    if (tile.self) {
      self = rect;
      continue;
    }
    const scale = tile.w / COMPOSITE_SHARE_WIDTH_PX;
    cells.push({
      cell: tile.slot - 1,
      sourceName: null,
      person: persons && typeof persons[tile.slot] === "string" ? persons[tile.slot] : null,
      stream: slotStreams && Number.isInteger(slotStreams[tile.slot]) ? slotStreams[tile.slot] : null,
      ...rect,
      bitCell: Math.max(1, Math.round(BIT_CELL_SOURCE_PX * scale * sx)),
      scaleX: scale,
      scaleY: scale,
      sourceWidth: COMPOSITE_SHARE_WIDTH_PX,
      sourceHeight: null,
    });
  }
  cells.sort((a, b) => a.cell - b.cell);
  return { ok: cells.length > 0, regionSource: "composite-layout", canvas: { width: outW, height: outH }, cells, skipped, self };
}

// キャンバスを 4 等分したセル cell (0 左上、1 右上、2 左下、3 右下)。奇数のときは左と上を切り捨て、残りを右と下に入れる
// (src-obs/sources/composite_layout.h の cell_rect)。
function compositeCellRect(canvas, cell) {
  const leftW = Math.floor(canvas.width / 2);
  const topH = Math.floor(canvas.height / 2);
  const right = cell % 2 === 1;
  const bottom = Math.floor(cell / 2) === 1;
  return { x: right ? leftW : 0, y: bottom ? topH : 0, w: right ? canvas.width - leftW : leftW, h: bottom ? canvas.height - topH : topH };
}

// 配置の 1 つ分の読み取り結果が、決まった置き方に合っているか。snapshot は { layout, persons } (layout は parseCompositeLayoutLine の
// 結果、persons は枠の番号 -> 人の名前)。返す物の names は画面共有のタイルの並び (左上から順の人の名前)。
// 見る項目: 自分の枠があれば先頭 (左上)、画面共有は表示名の自然順、2 つ以上ならタイル i がセル i の内側 (すき間の分だけ縮んでよい)、
// 1 つなら全面か 4 等分の左上。人の名前が引けないタイルがあれば並びは確認できない (失敗にする。何も出さずに通さない)。
export function judgeCompositePlacement(snapshot) {
  const layout = snapshot && snapshot.layout;
  if (!layout) return { ok: false, reason: "no Composite layout line was read", names: null, problems: ["no layout"] };
  const persons = (snapshot && snapshot.persons) || {};
  const problems = [];
  const tiles = layout.tiles;
  const selfIndex = tiles.findIndex((t) => t.self);
  if (selfIndex > 0) problems.push("the self tile is not the first tile");
  const shares = tiles.filter((t) => !t.self);
  const names = shares.map((t) => (typeof persons[t.slot] === "string" && persons[t.slot] !== "" ? persons[t.slot] : null));
  // ログに人の名前が 1 つもない (実際の通話。名前は「Screen slot N carries stream M」の M からテスト用の送信側の名前にコピーするだけ) ときは、
  // 名前の判定をしない (namesUnknown)。数と位置の判定だけが残る。
  const namesUnknown = Object.keys(persons).length === 0;
  if (!namesUnknown) {
    names.forEach((n, i) => {
      if (n === null) problems.push("the person of slot" + shares[i].slot + " is unknown");
    });
  }
  for (let i = 1; !namesUnknown && i < names.length; i += 1) {
    if (names[i - 1] !== null && names[i] !== null && naturalCompare(names[i - 1], names[i]) > 0) problems.push("the screen shares are not in display name order: " + names[i - 1] + " before " + names[i]);
  }
  if (layout.count >= 2) {
    tiles.forEach((t, i) => {
      if (i >= 4) {
        problems.push("more than 4 tiles");
        return;
      }
      const cell = compositeCellRect(layout.canvas, i);
      const inside = t.x >= cell.x && t.y >= cell.y && t.x + t.w <= cell.x + cell.w && t.y + t.h <= cell.y + cell.h;
      const shrink = Math.max(cell.w - t.w, cell.h - t.h);
      if (!inside || shrink > COMPOSITE_GAP_TOLERANCE_PX) problems.push("tile " + i + " (" + t.name + ") is not in cell " + i + ": " + [t.x, t.y, t.w, t.h].join(","));
    });
  } else if (layout.count === 1) {
    const t = tiles[0];
    const full = t.x === 0 && t.y === 0 && t.w === layout.canvas.width && t.h === layout.canvas.height;
    const cell = compositeCellRect(layout.canvas, 0);
    const quarter = t.x >= cell.x && t.y >= cell.y && t.x + t.w <= cell.x + cell.w && t.y + t.h <= cell.y + cell.h;
    if (!full && !quarter) problems.push("the single tile is neither full-canvas nor in the top-left cell: " + [t.x, t.y, t.w, t.h].join(","));
  }
  const mode = layout.count === 1 ? (tiles[0].w === layout.canvas.width && tiles[0].h === layout.canvas.height ? "single-full" : "single-quarter") : layout.count >= 2 ? "quarters" : "empty";
  return { ok: problems.length === 0, reason: problems.join("; ") || null, names, namesUnknown, count: layout.count, selfFirst: selfIndex <= 0, hasSelf: selfIndex >= 0, mode, problems };
}

// 枠の付け外しの判定: 外す前・外した後・戻した後の 3 つの読み取り結果 (snapshot) を判定する。
//   - 3 つとも置き方が合っている (judgeCompositePlacement)。
//   - 外す前は person が画面共有のタイルにいる。外した後は person がいなくなり、残りの人の並びは前のまま詰まる (タイルの数が 1 減る)。
//   - 戻した後は外す前と同じ並びで、参加者ごとの矩形も外す前と同じ。
// 自分の枠の有無は 3 つで変わらないことだけ見る (テスト用の送信側を使う実行では自分の枠のソースを選ばないので、自分の枠はないことが多い。あるかどうかは hasSelf に残す)。
export function judgeCompositeLayout({ before, afterRemove, afterRestore, person }) {
  if (!person) return result(false, "the person to remove is unknown", { person: null });
  const reads = { before, afterRemove, afterRestore };
  const placement = {};
  for (const key of Object.keys(reads)) placement[key] = judgeCompositePlacement(reads[key]);
  const unread = Object.keys(reads).filter((k) => !reads[k] || !reads[k].layout);
  if (unread.length > 0) return result(false, "the layout line was not read: " + unread.join(", "), { person, placement });
  const problems = [];
  for (const key of Object.keys(placement)) if (!placement[key].ok) problems.push(key + ": " + placement[key].reason);
  const b = placement.before.names || [];
  const r = placement.afterRemove.names || [];
  const s = placement.afterRestore.names || [];
  const namesUnknown = placement.before.namesUnknown === true;
  // 名前がなくても、読み取り結果にストリームの番号 (slotStreams) があれば、番号の並びで外した 1 ストリームと詰まり方と戻し方を確認する。
  const streamsKnown = namesUnknown && Object.values(reads).every((r) => r.slotStreams && typeof r.slotStreams === "object");
  const streamsOf = (read) => read.layout.tiles.filter((t) => !t.self).map((t) => (read.slotStreams ? read.slotStreams[t.slot] ?? null : null));
  const streams = streamsKnown ? { before: streamsOf(reads.before), afterRemove: streamsOf(reads.afterRemove), afterRestore: streamsOf(reads.afterRestore) } : null;
  if (streams) {
    for (const key of Object.keys(streams)) if (streams[key].some((x) => x === null)) problems.push(key + ": a tile has no stream number");
    const gone = streams.before.filter((x) => !streams.afterRemove.includes(x));
    const added = streams.afterRemove.filter((x) => !streams.before.includes(x));
    if (gone.length !== 1 || added.length > 0) problems.push("exactly one stream should have gone after removing: gone [" + gone.join(", ") + "], added [" + added.join(", ") + "]");
    const packedStreams = streams.before.filter((x) => streams.afterRemove.includes(x));
    if (JSON.stringify(packedStreams) !== JSON.stringify(streams.afterRemove)) problems.push("the remaining streams did not keep their order after removing: expected [" + packedStreams.join(", ") + "] got [" + streams.afterRemove.join(", ") + "]");
    // 枠の矩形が前と同じでも、同じ枠に別のストリームが載ることがあるので、並びそのものを比べる。
    if (JSON.stringify(streams.afterRestore) !== JSON.stringify(streams.before)) problems.push("the streams after restoring differ from before: [" + streams.afterRestore.join(", ") + "] vs [" + streams.before.join(", ") + "]");
  }
  if (!namesUnknown) {
    if (!b.includes(person)) problems.push(person + " was not in a tile before removing");
    if (r.includes(person)) problems.push(person + " was still in a tile after removing");
    const packed = b.filter((n) => n !== person);
    const packedOk = r.length === packed.length && r.every((n, i) => n === packed[i]);
    if (!packedOk) problems.push("the tiles did not close up after removing: expected [" + packed.join(", ") + "] got [" + r.join(", ") + "]");
  }
  if (reads.afterRemove.layout.count !== reads.before.layout.count - 1) problems.push("the tile count did not drop by one after removing (" + reads.before.layout.count + " -> " + reads.afterRemove.layout.count + ")");
  const sameOrder = namesUnknown || (s.length === b.length && s.every((n, i) => n === b[i]));
  if (!sameOrder) problems.push("the order after restoring differs from the order before: [" + s.join(", ") + "] vs [" + b.join(", ") + "]");
  const rectsOf = (read) => read.layout.tiles.map((t) => [t.name, t.x, t.y, t.w, t.h].join(","));
  const sameRects = JSON.stringify(rectsOf(reads.before)) === JSON.stringify(rectsOf(reads.afterRestore));
  if (!sameRects) problems.push("the tile rectangles after restoring differ from before");
  if (placement.before.hasSelf !== placement.afterRemove.hasSelf || placement.before.hasSelf !== placement.afterRestore.hasSelf) problems.push("the self tile appeared or disappeared");
  // 名前がログにない場合は、ストリームの番号が無ければ、数と位置の判定だけが通っても「判定できない」(null) にする (人が外れたことは
  // 確認していない)。番号があれば、構造 (外れた 1 ストリーム・詰まり・戻し) が通れば成功にし、表示名の順だけは判定しないと理由に書く。
  const okValue = problems.length > 0 ? false : namesUnknown && !streams ? null : true;
  const note = streams ? "names are not in the log; the display name order was not judged, and the streams, counts and rectangles were checked" : "names are not in the log; only counts and rectangles were checked";
  return result(okValue, problems.join("; ") || (namesUnknown ? note : ""), {
    person,
    namesUnknown,
    ...(namesUnknown ? { note } : {}),
    ...(streams ? { streams, nameOrderJudged: false } : {}),
    names: { before: b, afterRemove: r, afterRestore: s },
    counts: { before: reads.before.layout.count, afterRemove: reads.afterRemove.layout.count, afterRestore: reads.afterRestore.layout.count },
    hasSelf: placement.before.hasSelf,
    mode: placement.before.mode,
    problems,
  });
}

// OBS のログの映像の遅延の行 (src-obs/pipeline/pipeline.cpp の「Stats stream=N video_latency ... shown_avg_ms=.. shown_p95_ms=.. shown_n=..」)
// から、ストリームごとの表示の遅延の測定値を読む。shown_n は OBS が表示に選んだフレームの数 (合成の子に付けた測定のフィルタが受けたフレーム)。
export function videoLatencyShownFromLog(lines) {
  const byStream = new Map();
  for (const raw of lines || []) {
    const m = /Stats stream=(\d+) video_latency .*?\bhanded_n=(\d+) shown_avg_ms=([0-9.]+) shown_p95_ms=([0-9.]+) shown_n=(\d+)/.exec(String(raw));
    if (!m) continue;
    const id = Number(m[1]);
    if (!byStream.has(id)) byStream.set(id, { stream: id, lines: 0, handedN: 0, shownN: 0, shownLines: 0, shownAvgSumMs: 0, shownP95MaxMs: 0 });
    const e = byStream.get(id);
    const shownN = Number(m[5]);
    e.lines += 1;
    e.handedN += Number(m[2]);
    e.shownN += shownN;
    if (shownN > 0) {
      e.shownLines += 1;
      e.shownAvgSumMs += Number(m[3]) * shownN;
      e.shownP95MaxMs = Math.max(e.shownP95MaxMs, Number(m[4]));
    }
  }
  return [...byStream.values()]
    .sort((a, b) => a.stream - b.stream)
    .map((e) => ({ stream: e.stream, lines: e.lines, handedN: e.handedN, shownN: e.shownN, shownLines: e.shownLines, shownAvgMs: e.shownN > 0 ? Math.round((e.shownAvgSumMs / e.shownN) * 10) / 10 : null, shownP95MaxMs: e.shownN > 0 ? e.shownP95MaxMs : null }));
}

// 表示の遅延の測定が動作している: shown_n が 0 でない行がある (合成の子に測定のフィルタが付いた証拠)。フレームを渡したストリーム (handed_n が 0 でない)
// のうち shown_n が 1 つも無かったストリームは withoutShown に残す (ストリームが途中で入れ替わった場合に失敗にはしない)。
export function judgeDisplayDelay(streams) {
  const list = streams || [];
  const totalShown = list.reduce((n, e) => n + e.shownN, 0);
  const withoutShown = list.filter((e) => e.handedN > 0 && e.shownN === 0).map((e) => e.stream);
  const reason = list.length === 0 ? "no video_latency line in the OBS log" : "shown_n was 0 on every stream (the measuring filter on the tiles did not run)";
  return result(totalShown > 0, reason, { totalShown, streams: list, withoutShown });
}

// OBS のログの「adding N milliseconds of audio buffering, total audio buffering is now M milliseconds (source: 名前)」の行を読む。
// 名前は括弧を含みうる (例: harness-voice-5 (cordmux声 2)) ので、行末の「(source: 」から最後の「)」までを取る。
export function audioBufferingFromLog(lines) {
  const out = [];
  for (const raw of lines || []) {
    const line = String(raw);
    const m = /^(?:(\d{2}:\d{2}:\d{2}\.\d{3}): )?.*?adding (\d+) milliseconds of audio buffering, total audio buffering is now (\d+) milliseconds \(source: (.*)\)\s*$/.exec(line);
    if (!m) continue;
    out.push({ time: m[1] || null, addedMs: Number(m[2]), totalMs: Number(m[3]), source: m[4] });
  }
  return out;
}

// OBS の全体の音の遅延の増加。判定のしきい値はまだ決めていないので、数と中身を記録するだけ (ok は null)。ソースの名前ごとの回数と足した
// 合計も出す。
export function judgeAudioBuffering(entries) {
  const list = entries || [];
  const bySource = {};
  for (const e of list) {
    if (!bySource[e.source]) bySource[e.source] = { count: 0, addedMs: 0 };
    bySource[e.source].count += 1;
    bySource[e.source].addedMs += e.addedMs;
  }
  return { ok: null, reason: null, count: list.length, finalTotalMs: list.length > 0 ? list[list.length - 1].totalMs : 0, bySource, entries: list };
}

// 声のミュートと外し直しの判定: mark-time のマーカー (ac32MarkLabel。録画の始めからの ms) から、声の枠ごとの区間 (基準・ミュート・外し直し) を秒で組む。
// 両端は marginS ずつ削る。マーカーが足りない・削ると短すぎる区間は problems に書いて飛ばす。
export function ac32Windows({ marks, voices, marginS }) {
  const m = marks || {};
  const margin = typeof marginS === "number" ? marginS : AC32_WINDOW_MARGIN_S;
  const round3 = (x) => Math.round(x * 1000) / 1000;
  const windows = [];
  const problems = [];
  for (let v = 1; v <= Number(voices || 0); v += 1) {
    const edges = [...AC32_SEGMENT_NAMES, "end"].map((seg) => m[ac32MarkLabel(v, seg)]);
    AC32_SEGMENT_NAMES.forEach((segment, k) => {
      const label = "v" + v + "-" + segment;
      const a = edges[k];
      const b = edges[k + 1];
      if (typeof a !== "number" || typeof b !== "number") {
        problems.push(label + ": a mark is missing");
        return;
      }
      const start = a / 1000 + margin;
      const end = b / 1000 - margin;
      if (!(end - start >= 0.5)) {
        problems.push(label + ": shorter than 0.5 s after trimming");
        return;
      }
      windows.push({ label, slot: v, segment, start: round3(start), end: round3(end) });
    });
  }
  return { windows, problems };
}

// tone_scan.py --windows の引数の文字列 ("label=start:end,...")。
export function toneScanWindowsArg(windows) {
  return (windows || []).map((w) => w.label + "=" + w.start + ":" + w.end).join(",");
}

// 区間ごとの音量 (tone_scan.py の files[0].windows の rms) で、声ごとに「ミュートは無音、外し直すと基準に戻る」を判定する。
// 基準の区間が鳴っていない声は「判定できない」(音の出る声は 1 人だけの前提)。判定できる声が 1 つも無ければ ok は null (リハーサルの
// テスト用の送信側の声は無音 = 失敗にしない)。
export function judgeAc32({ windows, scanned }) {
  const byLabel = new Map((scanned || []).filter((w) => w && w.label).map((w) => [w.label, w]));
  const slots = [...new Set((windows || []).map((w) => w.slot))].sort((a, b) => a - b);
  const round2 = (x) => Math.round(x * 100) / 100;
  const parts = slots.map((slot) => {
    const rmsOf = (segment) => {
      const w = byLabel.get("v" + slot + "-" + segment);
      return w && typeof w.rms === "number" ? w.rms : null;
    };
    const rms = { base: rmsOf("base"), muted: rmsOf("muted"), restored: rmsOf("restored") };
    if (rms.base === null) return { slot, rms, decidable: false, ok: null, note: "the base window was not read" };
    if (rms.base < TONE_AUDIBLE_RMS) return { slot, rms, decidable: false, ok: null, note: "no sound in this voice (only one voice carries the test tone)" };
    const restoredDb = typeof rms.restored === "number" && rms.restored > 0 ? round2(20 * Math.log10(rms.restored / rms.base)) : null;
    const restoredOk = restoredDb !== null && Math.abs(restoredDb) <= AC32_GAIN_TOLERANCE_DB;
    const mutedOk = typeof rms.muted === "number" && rms.muted < TONE_SILENT_RMS;
    return { slot, rms, decidable: true, restoredDb, restoredOk, mutedOk, ok: restoredOk && mutedOk };
  });
  const decided = parts.filter((p) => p.decidable);
  const lines = { toleranceDb: AC32_GAIN_TOLERANCE_DB, silentRms: TONE_SILENT_RMS, audibleRms: TONE_AUDIBLE_RMS };
  if (decided.length === 0) return result(null, "no voice carried sound; the mute could not be judged by sound", { parts, soundingSlots: 0, ...lines });
  const failed = decided.filter((p) => !p.ok).map((p) => p.slot);
  return result(failed.length === 0, "not as expected on voice " + failed.join(", "), {
    parts,
    soundingSlots: decided.length,
    premise: decided.length === 1 ? "one voice carried sound (as assumed)" : "more than one voice carried sound (the premise is one)",
    ...lines,
  });
}

// 名前ラベルの [表示] の判定: 設定を変える前と、名前の [表示] を外した後の合成のソースの画像の名前ラベルの背景 (plate_measure.py の found と plate.width)。
// 外した後に背景が見つからないか、幅が前の 85% 以下に狭まれば、設定が画像に反映されている。前の画像で背景が見つからなければ「判定できない」(null。
// 背景を読み取れない映像だった)。名前を外してもアバターの背景が残るので、「消える」ではなく「狭まる」も成功とする。
export const AC31_NARROW_RATIO = 0.85;
export function judgeAc31({ before, after }) {
  const b = before || {};
  const a = after || {};
  if (b.found !== true) return result(null, "the plate was not found in the picture before the change; the name toggle could not be judged", { before: b, after: a });
  if (a.found === null || a.found === undefined) return result(null, "the picture after the change could not be measured", { before: b, after: a });
  const bw = b.plate && Number(b.plate.width);
  const aw = a.found && a.plate ? Number(a.plate.width) : 0;
  const narrowed = a.found === false || (bw > 0 && aw <= bw * AC31_NARROW_RATIO);
  return result(narrowed, "the name label plate did not narrow or disappear after turning the name off", { before: b, after: a, widthBefore: bw ?? null, widthAfter: a.found ? aw : null, ratio: AC31_NARROW_RATIO });
}
// select-four: 配置の行 (layout と persons) が、画面共有 count 個を 4 等分の決まった位置に置いているか。judgeCompositePlacement の
// 置き方の判定に、画面共有のタイルの数が count と合うことを足す (自分の枠のタイルは数えない)。
export function judgeCompositeCount({ layout, persons, count }) {
  const placed = judgeCompositePlacement({ layout, persons });
  if (!layout) return { ...placed, count: null };
  const shares = layout.tiles.filter((t) => !t.self).length;
  const problems = [...(placed.problems || [])];
  if (shares !== count) problems.push("the number of screen share tiles is " + shares + ", expected " + count);
  return { ...placed, ok: problems.length === 0, reason: problems.join("; ") || null, shares, problems };
}

// 使う声の確認で tone_scan.py の --windows に渡す文字列 (8 秒の録画の先頭と末尾を削った 1 区間 "all")。
export function voiceSourceScanWindow({ recordSeconds, marginSeconds } = {}) {
  const total = typeof recordSeconds === "number" ? recordSeconds : VOICE_SOURCE_CHECK_RECORD_MS / 1000;
  const margin = typeof marginSeconds === "number" ? marginSeconds : VOICE_SOURCE_SCAN_MARGIN_S;
  return "all=" + margin + ":" + (Math.round((total - margin) * 1000) / 1000);
}

// tone_scan.py の出力 (1 行の JSON) から、先頭のファイルの 1 つ目の区間の rms と tone_share を取り出す。tone_share は
// --windows のときの windows[] にだけある (ファイルごとの値にはない)。区間が無ければ「区間がない」を error に書く (黙って null にしない)。
export function toneWindowValues(scanOut) {
  const f = scanOut && Array.isArray(scanOut.files) && scanOut.files[0] ? scanOut.files[0] : null;
  if (!f) return { rms: null, toneShare: null, windowSeconds: null, error: "tone_scan.py gave no file entry" };
  if (f.error) return { rms: null, toneShare: null, windowSeconds: null, error: String(f.error) };
  const w = Array.isArray(f.windows) && f.windows[0] ? f.windows[0] : null;
  if (!w) return { rms: null, toneShare: null, windowSeconds: null, error: "tone_scan.py gave no window (was --windows passed?)" };
  return {
    rms: typeof w.rms === "number" ? w.rms : null,
    toneShare: typeof w.tone_share === "number" ? w.tone_share : null,
    windowSeconds: typeof w.duration_s === "number" ? w.duration_s : null,
    error: typeof w.rms === "number" && typeof w.tone_share === "number" ? null : "the window has no rms or tone_share (empty window)",
  };
}

// 空いた区画の四角。base は OBS の画面 (GetVideoSettings の base) の上 (置く位置と枠の大きさ)、output は録画の上
// (latency.py の --region-b)。区画の割り方は合成のソースの 4 等分 (列 = cell % 2、行 = cell / 2)。
export function latencyReferenceRect({ cell, videoSettings }) {
  const vs = videoSettings || {};
  const baseW = Number(vs.baseWidth) > 0 ? Number(vs.baseWidth) : 1920;
  const baseH = Number(vs.baseHeight) > 0 ? Number(vs.baseHeight) : 1080;
  const outW = Number(vs.outputWidth) > 0 ? Number(vs.outputWidth) : baseW;
  const outH = Number(vs.outputHeight) > 0 ? Number(vs.outputHeight) : baseH;
  const qw = baseW / 2;
  const qh = baseH / 2;
  const col = Number(cell) % 2;
  const row = Math.floor(Number(cell) / 2);
  const base = { x: col * qw, y: row * qh, w: qw, h: qh };
  const sx = outW / baseW;
  const sy = outH / baseH;
  const x0 = Math.ceil(base.x * sx);
  const y0 = Math.ceil(base.y * sy);
  const output = { x: x0, y: y0, w: Math.floor((base.x + qw) * sx) - x0, h: Math.floor((base.y + qh) * sy) - y0 };
  return { cell: Number(cell), base, output, canvas: { width: outW, height: outH } };
}

// ウィンドウキャプチャを空いた区画に収める置き方 (SetSceneItemTransform。縦横の比を保って区画の中に収め、真ん中に置く)。
export function latencyReferenceTransform(rect) {
  return {
    positionX: rect.base.x,
    positionY: rect.base.y,
    alignment: 5,
    boundsType: "OBS_BOUNDS_SCALE_INNER",
    boundsAlignment: 0,
    boundsWidth: rect.base.w,
    boundsHeight: rect.base.h,
  };
}

// 遅れの測定: ウィンドウキャプチャが Vesktop のウィンドウを捉えられなかったとき、画面キャプチャへ切り替えてよいかの判定。
// Vesktop のメインウィンドウ (window-rect.ps1 が物理のピクセルで読んだ四角と、最大化か・最小化かと、ウィンドウがある画面の四角) が、最大化されていて
// その画面の全体を覆うときだけ真にする。覆っていなければ画面キャプチャは利用者の別のウィンドウもキャプチャするので使わない。分からない場合 (ウィンドウがない・
// 四角が読めない・最小化) も偽にする (切り替えない)。最大化されたウィンドウの四角は画面より DWM の縁の分だけ外へはみ出す
// (実測: 画面 1920,0 の 1920x1080 に対しウィンドウ 1912,-8 の 1936x1096) ので、各辺のはみ出しは MONITOR_FILL_OVERHANG_PX まで許す (縁は DPI の倍率で太るので、実測の 8 の倍の余裕を見る)。
// それより大きくはみ出すウィンドウ (複数の画面にまたがるウィンドウ) は、この画面の全体をキャプチャしてもウィンドウの画像にならないので偽。
export const MONITOR_FILL_OVERHANG_PX = 16;
export function windowFillsMonitor(info) {
  const rectOf = (r) => (Array.isArray(r) && r.length === 4 && r.every((n) => Number.isFinite(n)) && r[2] > 0 && r[3] > 0 ? r : null);
  if (!info || info.found !== true) return { fills: false, reason: "no Vesktop window was found" };
  if (info.iconic === true) return { fills: false, reason: "the Vesktop window is minimized" };
  if (info.zoomed !== true) return { fills: false, reason: "the Vesktop window is not maximized" };
  const win = rectOf(info.rect);
  const mon = rectOf(info.monitor);
  if (!win || !mon) return { fills: false, reason: "the window or screen rectangle could not be read" };
  const over = [mon[0] - win[0], mon[1] - win[1], win[0] + win[2] - (mon[0] + mon[2]), win[1] + win[3] - (mon[1] + mon[3])];
  if (over.some((d) => d < 0)) return { fills: false, reason: "the maximized window does not cover the whole screen" };
  if (over.some((d) => d > MONITOR_FILL_OVERHANG_PX)) return { fills: false, reason: "the window extends past the screen" };
  return { fills: true, reason: null, monitor: { x: mon[0], y: mon[1], w: mon[2], h: mon[3] } };
}

// OBS の画面キャプチャの monitor_id の一覧の名前 ("<機種>: 1920x1080 @ 1920,0") から大きさと位置を読む。形が違えば null。
export function parseMonitorItemName(name) {
  const m = /(\d+)x(\d+)\s*@\s*(-?\d+)\s*,\s*(-?\d+)\s*$/.exec(String(name || ""));
  return m ? { w: Number(m[1]), h: Number(m[2]), x: Number(m[3]), y: Number(m[4]) } : null;
}

// 一覧 (GetInputPropertiesListPropertyItems の propertyItems) から、四角 (x, y, w, h) が同じ画面を 1 つだけ選ぶ。
// 位置が同じ物が 0 個か 2 個以上なら選ばない (別の画面をキャプチャしない)。monitor_id にはバックスラッシュが入るので、値は一覧のまま返す。
export function pickMonitorItem(items, monitor) {
  if (!monitor) return { item: null, reason: "no screen rectangle" };
  const hits = (Array.isArray(items) ? items : []).filter((it) => {
    const p = parseMonitorItemName(it && it.itemName);
    return p && p.x === monitor.x && p.y === monitor.y && p.w === monitor.w && p.h === monitor.h && typeof it.itemValue === "string" && it.itemValue !== "";
  });
  if (hits.length === 0) return { item: null, reason: "no screen in the capture list matches " + monitor.w + "x" + monitor.h + " @ " + monitor.x + "," + monitor.y };
  if (hits.length > 1) return { item: null, reason: "more than one screen in the capture list matches " + monitor.w + "x" + monitor.h + " @ " + monitor.x + "," + monitor.y };
  return { item: hits[0], reason: null };
}

// latency.py に渡す 2 つの区画。a = 遅れを読む人の cordmux の枠 (録画の始めと終わりに読んだ枠の四角 = cellRegionsForJudge。
// その人の枠は名前の %1 で探す)、b = Vesktop のウィンドウを置いた区画 (reference。置いたときに読んだ四角)。人の枠が見つからなければ
// regionA は null (「測れない」と理由を書く。ほかの人の枠と取り違えない)。
// resolvedCell (personCellOf で引いた枠) があればそれを使い、無ければセルの名前 (テスト用の送信側の名前) で探す。
export function latencyRegions({ captures, cells, person, personForm, reference, resolvedCell }) {
  const regions = cellRegionsForJudge({ captures, cells });
  let cell = Number.isInteger(resolvedCell) ? resolvedCell : null;
  if (cell === null) {
    for (const capture of captures || []) {
      for (const c of (capture && capture.cells) || []) {
        if (c.person === person || (personForm && personFromSlotName(c.sourceName, c.cell + 1, personForm) === person)) cell = c.cell;
      }
    }
  }
  const regionA = cell !== null ? regions.find((r) => r.cell === cell) || null : null;
  const regionB = reference || null;
  const reason = !regionB ? "the reference window was not placed" : regionA === null ? "the frame of " + person + " was not found in the latency recording" : null;
  return { regionA, regionB, cell, personFound: cell !== null, reason };
}

// 遅れの録画の始めと終わりに読んだ Main の枠ごとの名前と四角 (録画の座標)。judge-latency の
// 記録にコピーし、遅れを読む人の名前が枠に見つからなかったときに、テストを実行する人が枠の名前で領域 A を選んで手で読み直せるようにする。
export function latencyCellsSeen(captures) {
  const named = (list, pick) => (Array.isArray(list) ? list.map(pick) : []);
  return (captures || [])
    .filter((c) => c && typeof c === "object")
    .map((c) => ({
      when: c.when || null,
      ok: typeof c.ok === "boolean" ? c.ok : null,
      cells: named(c.cells, (x) => ({ cell: x.cell, sourceName: x.sourceName ?? null, x: x.x, y: x.y, w: x.w, h: x.h })),
      skipped: named(c.skipped, (x) => ({ cell: x.cell, sourceName: x.sourceName ?? null, reason: x.reason ?? null })),
    }));
}

// latency.py の呼び出しの形。a が cordmux の枠 (遅れを測られる側)、b が Vesktop のウィンドウ (基準 = Discord 自身の再生)。
// latency.py の出力はいつも 1 つの JSON で、--json のフラグを持たない (渡すと引数エラーになる)。
// --json を持つバージョンにも合うように、実行スクリプトは latency.py --help に --json があるときだけ足す (jsonFlag)。
export function latencyToolArgs({ recording, regionA, regionB, jsonFlag }) {
  const r = (x) => [x.x, x.y, x.w, x.h].join(",");
  return [recording, "--region-a", r(regionA), "--region-b", r(regionB), ...(jsonFlag ? ["--json"] : [])];
}

// latency.py の出力から、判定に使う遅れの値を取り出す。latency.py は judge.delay_ms (cordmux_delay_ms の平均。正 = cordmux の出力が
// Discord 自身の再生より遅い) で判定する (中央値は共有のフレームの間隔の刻みに張り付くので、平均を使う)。読めなければ
// cordmux_delay_ms.mean、それも無ければ median_ms。measured が false なら値はない (理由は judge.reason か error)。
export function latencyValueOf(data) {
  const d = data || {};
  const num = (x) => typeof x === "number" && Number.isFinite(x);
  if (d.measured === false) return { value: null, statistic: null, reason: (d.judge && d.judge.reason) || d.error || "latency.py could not measure" };
  if (d.judge && num(d.judge.delay_ms)) return { value: d.judge.delay_ms, statistic: d.judge.statistic || "mean", reason: null };
  if (d.cordmux_delay_ms && num(d.cordmux_delay_ms.mean)) return { value: d.cordmux_delay_ms.mean, statistic: "mean", reason: null };
  if (num(d.median_ms)) return { value: d.median_ms, statistic: "median", reason: null };
  return { value: null, statistic: null, reason: "latency.py gave no delay value (judge.delay_ms / cordmux_delay_ms.mean)" };
}

// latency.py の結果を目標と比べる。ウィンドウキャプチャ自身の遅れ (LATENCY_CAPTURE_BIAS_FRAMES フレーム) の分、測った差は
// 本当の遅れより小さく出るので、線は「測った値 + 偏り」(estimatedMs) と比べる (偏りは latency.py の capture_bias.approx_ms、
// 無ければ fps から)。ok は許容限 (90 ms) 未満か。検知限 (45 ms) 以上なら designQuestion (設計の問いとして記録する。実行中は
// 止めない)。境目は latency.py の over_detect / over_target (以上で超え) と同じ。負の値は区画の取り違えか帯の読み違いを疑う (note)。
export function judgeLatency({ data, fps }) {
  const picked = latencyValueOf(data);
  const fromTool = data && data.capture_bias && typeof data.capture_bias.approx_ms === "number" ? data.capture_bias.approx_ms : null;
  const frameMs = Number(fps) > 0 ? 1000 / Number(fps) : null;
  const captureBiasMs = fromTool !== null ? fromTool : frameMs !== null ? Math.round(LATENCY_CAPTURE_BIAS_FRAMES * frameMs * 10) / 10 : null;
  const targets = { detectMs: LATENCY_TARGET_DETECT_MS, acceptMs: LATENCY_TARGET_ACCEPT_MS };
  const bias = {
    captureBiasMs,
    captureBiasNote: "the window capture of Vesktop itself arrives about " + LATENCY_CAPTURE_BIAS_FRAMES + " frame late, so the measured value is smaller than the true delay by about that much",
  };
  const extra = {
    statistic: picked.statistic,
    pairs: data && typeof data.pairs === "number" ? data.pairs : null,
    toolLevel: data && data.judge ? data.judge.level ?? null : null,
    warnings: data && Array.isArray(data.warnings) ? data.warnings : [],
  };
  if (picked.value === null) return result(false, picked.reason, { measuredMs: null, estimatedMs: null, ...bias, targets, ...extra });
  const measured = picked.value;
  const estimatedMs = captureBiasMs !== null ? Math.round((measured + captureBiasMs) * 10) / 10 : measured;
  const withinDetectionTarget = estimatedMs < LATENCY_TARGET_DETECT_MS;
  const withinAcceptableTarget = estimatedMs < LATENCY_TARGET_ACCEPT_MS;
  return result(withinAcceptableTarget, "the estimated delay " + estimatedMs + " ms is at or over the acceptable target " + LATENCY_TARGET_ACCEPT_MS + " ms", {
    measuredMs: measured,
    estimatedMs,
    ...bias,
    withinDetectionTarget,
    withinAcceptableTarget,
    designQuestion: !withinDetectionTarget,
    note: measured < 0 ? "negative: cordmux showed the strip earlier than Vesktop; check the regions (a = cordmux frame, b = Vesktop window)" : null,
    targets,
    ...extra,
  });
}

// 2 つの JSON のキーのパスの違い (値は返さない)。キーのパスの中の 17-20 桁の数 (人の ID をキーにする設定) は伏せる。配列は中を
// 比べず、違えば changed とだけ書く。
export function settingsKeyDiff(before, after, maxEntries = 200) {
  const typeOf = (v) => (v === null ? "null" : Array.isArray(v) ? "array" : typeof v);
  const mask = (key) => String(key).replace(/[0-9]{17,20}/g, "<ID>");
  const changes = [];
  const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  const walk = (a, b, path) => {
    if (changes.length >= maxEntries) return;
    const ta = typeOf(a);
    const tb = typeOf(b);
    if (ta === "object" && tb === "object") {
      for (const k of [...new Set([...Object.keys(a), ...Object.keys(b)])].sort()) {
        if (changes.length >= maxEntries) return;
        const p = path.concat(mask(k));
        if (!own(a, k)) changes.push({ path: p.join("."), change: "added", type: typeOf(b[k]) });
        else if (!own(b, k)) changes.push({ path: p.join("."), change: "removed", type: typeOf(a[k]) });
        else walk(a[k], b[k], p);
      }
      return;
    }
    if (ta !== tb) changes.push({ path: path.join("."), change: "type", from: ta, to: tb });
    else if (JSON.stringify(a) !== JSON.stringify(b)) changes.push({ path: path.join("."), change: "changed", type: ta });
  };
  walk(before, after, []);
  return { changes, truncated: changes.length >= maxEntries };
}

// 実行スクリプトの finally で vencordDir を戻すか。戻すのは、実際の state.json を使う実行 (--real-profile) で、実行計画が締めで戻す場合
// (judge-vencord-dir-restored を持つ = call-1 と call-hold。開始ゲートの call-gate-a は書いた値を次のゲートへ残すので戻さない) が、
// 戻ったことを確認する前に終わったとき。バックアップが無ければ戻せない (製品が書いていない、またはアンインストールがバックアップを消した後) ので
// 確認だけ。リハーサルは偽の state.json で、後始末が偽の APPDATA ごと消すので戻さない。
//   skip         何もしない (理由つき)
//   call-window  cleanup.ps1 -CallWindow -RealProfile でバックアップの previous へ戻す
//   report-only  戻せないので、最後の確認 (vencordDirFinalCheck) だけを書く
export function finallyVencordRestorePlan({ realProfile, planHasRestoreJudge, restoredOk, backupExists }) {
  if (!realProfile) return { action: "skip", reason: "not the real profile (the fake APPDATA is removed by cleanup.ps1)" };
  if (!planHasRestoreJudge) return { action: "skip", reason: "this step keeps the written vencordDir on purpose (the start gates)" };
  if (restoredOk) return { action: "skip", reason: "the uninstall restored vencordDir (judge-vencord-dir-restored was ok)" };
  if (!backupExists) return { action: "report-only", reason: "no backup to restore from (the product did not write state.json, or the uninstall already removed the backup)" };
  return { action: "call-window", reason: "the run ended before the restore was confirmed; cleanup.ps1 -CallWindow restores vencordDir from the backup" };
}

// cleanup.ps1 のフラグ (run-window.mjs の runCleanup が使う。手順の cleanup と finally の両方)。callWindow は finally だけ。
export function cleanupFlags({ dryRun, obsDir, artifactsPath, realProfile, keepRecordings, watchProgramData, callWindow } = {}) {
  const flags = dryRun ? ["-DryRun", "-Json"] : ["-Json"];
  if (obsDir) flags.push("-ObsDir", obsDir);
  if (artifactsPath) flags.push("-ArtifactsPath", artifactsPath);
  if (realProfile) flags.push("-RealProfile");
  if (keepRecordings) flags.push("-KeepRecordings");
  if (watchProgramData) flags.push("-WatchProgramData");
  if (callWindow) flags.push("-CallWindow");
  return flags;
}

// 実行の最後の vencordDir と、実行の前のコピー (state-json-before.json) の vencordDir が同じか。キーの有無も比べる
// (前にキーが無ければ、現在もないことが「戻った」)。読めなければ same は null。
export function vencordDirFinalCheck({ stateText, beforeText }) {
  const before = parseObjectOrNull(beforeText);
  const state = parseObjectOrNull(stateText);
  if (!before) return { same: null, reason: "the copy taken before the run is not readable", vencordDirNow: null, vencordDirBefore: null };
  if (!state) return { same: null, reason: "state.json is not readable now", vencordDirNow: null, vencordDirBefore: null };
  const has = (o) => Object.prototype.hasOwnProperty.call(o, "vencordDir");
  const now = has(state) ? state.vencordDir : null;
  const was = has(before) ? before.vencordDir : null;
  const same = has(state) === has(before) && (!has(state) || normalizePathForCompare(now) === normalizePathForCompare(was));
  return { same, reason: same ? null : "vencordDir is not the value before the run", vencordDirNow: now, vencordDirBefore: was, keyNow: has(state), keyBefore: has(before) };
}

// 結果 JSON と記録に書く args (配信者の ID を REDACTED_BROADCASTER_ID に置き換える)。
export function argsForResult(args) {
  if (!args || args.broadcasterId === null || args.broadcasterId === undefined) return args;
  return { ...args, broadcasterId: REDACTED_BROADCASTER_ID };
}

// 配信者の ID が実際の ID の形 (17-20 桁の数だけ) か。記録の文字列の置き換えはこの形のとき
// だけ行う (リハーサルと dry-run のテスト用の 1 のような短い値で置き換えると、記録の中のほかの 1 を壊す)。
export function isDiscordIdShape(id) {
  return typeof id === "string" && /^[0-9]{17,20}$/.test(id);
}

// 記録 (結果 JSON の steps に積むものと、実行スクリプトの記録の行) の文字列の全部から配信者の ID を伏せる。実行スクリプトの record と
// log が手順を問わず通す。ID の入った Edit を持つウィンドウの [OK] (dialog-click) は、dialog-click.ps1 の Read-Edits が
// ウィンドウの Edit の値を全部読んで edits と texts に入れるので、特定の手順の記録だけを伏せる形では ID が結果 JSON と記録に残る。
// 実際の ID の形でなければ、または ID が含まれていなければ、data をそのまま (同じものを) 返す。
// 置き換えるのは文字列の値だけで、キー・数 (NaN と Infinity を含む)・undefined・素の object でないものは変えない (後の手順が
// state.results から読む値が、実際の実行でだけ変わらないようにするため)。
// JSON に変換して戻す方法は使わない: NaN と Infinity が null になり、undefined の欄が消える。実際の ID の形はリハーサルを通らないので、
// その違いが実機の実行で初めて出る。手順の種類ごとに伏せる所を足す方法も使わない: 特定の手順の記録だけを伏せる形と同じ漏れが再び起きる。
export function redactBroadcasterId(data, id) {
  if (!isDiscordIdShape(id) || data === null || data === undefined) return data;
  const fix = (s) => (s.includes(id) ? s.split(id).join(REDACTED_BROADCASTER_ID) : s);
  if (typeof data === "string") return fix(data);
  let text;
  try {
    text = JSON.stringify(data);
  } catch {
    return data;
  }
  if (typeof text !== "string" || !text.includes(id)) return data;
  const walk = (v) => {
    if (typeof v === "string") return fix(v);
    if (Array.isArray(v)) return v.map(walk);
    if (v && typeof v === "object" && Object.getPrototypeOf(v) === Object.prototype) {
      const out = {};
      for (const [k, x] of Object.entries(v)) out[k] = walk(x);
      return out;
    }
    return v;
  };
  return walk(data);
}

// 文字列の中の 17-20 桁の数を数える (数そのものは返さない。id-scan.mjs とテストが使う)。
export function countDiscordIdLike(text) {
  const m = String(text == null ? "" : text).match(/(?<![0-9])[0-9]{17,20}(?![0-9])/g);
  return m ? m.length : 0;
}

// 線と、外れたときの扱い (実行を止めるもの / 記録するだけのもの)。judge-lines が結果の表をこの並びで作る。
//   stop    後の答えが 1 つも取れなくなる前提。外れたら実行を止めて締めの後始末へ進む (stopOnFail。--start-with-add の実際の実行)
//   record  記録するだけ (設計の問いとして残す。gate にもしない = 終了コードを変えない)
//   gate    外れたら gateFailures に入る (終了コードが 3 になる)。実行は止めない
export const JUDGE_LINES = [
  { item: "add-to-vesktop", what: "[Vesktopに追加] が state.json に届く (10 分)", line: "vencordDir が配置物の data/vencord を指す", onFail: "stop" },
  { item: "vesktop-connects", what: "起動し直した Vesktop の送信部が繋ぐ (20 分)", line: "保存した数より hello が増える", onFail: "stop" },
  { item: "self-account", what: "送信側が自分のアカウントを届ける (--recheck)", line: "接続の後、" + SELF_ACCOUNT_WAIT_TIMEOUT_MS / 1000 + " 秒以内に OBS の記録の Facts の行に self=yes が出る", onFail: "gate" },
  { item: "av-sync-main", what: "OBS の出口の映像と音のずれ (本編)", line: "音が先 " + AV_SYNC_AUDIO_LEAD_LIMIT_MS + " ms 未満・音が遅れ " + AV_SYNC_AUDIO_LAG_LIMIT_MS + " ms 未満、時間とともに広がらない (一時的な段を除いた傾きが " + AV_TREND_DEFECT_MS_PER_MIN + " ms/分を超えて 0 と分けられれば欠陥。段は数と高さを記録する)", onFail: "record" },
  { item: "av-sync-av1", what: "OBS の出口の映像と音のずれ (av1。--recheck はテスト動画を共有している人の枠だけ)", line: "同上", onFail: "record" },
  { item: "av-sync-av2", what: "OBS の出口の映像と音のずれ (av2)", line: "同上", onFail: "record" },
  { item: "av-drift", what: "av2 - av1", line: "区画ごとの |差| < " + AV_DRIFT_DELTA_LIMIT_MS + " ms", onFail: "record" },
  { item: "record-sync", what: "途中で始めた cordmux の録画の中の映像と音のずれ", line: "本編と同じ", onFail: "record" },
  { item: "latency", what: "cordmux が足す遅れ (latency.py の平均にウィンドウキャプチャの 1 フレームを足した見積もり)", line: "許容限 " + LATENCY_TARGET_ACCEPT_MS + " ms 未満 (検知限 " + LATENCY_TARGET_DETECT_MS + " ms 以上は設計の問い)", onFail: "record" },
  { item: "ac30", what: "4 分割の付け外し", line: "自分の枠は左上、画面共有は表示名の順、2 つ以上なら 4 等分の決まった位置、外すと詰まり戻すと元に戻る", onFail: "record" },
  { item: "ac31", what: "名前ラベルの設定が絵に反映される", line: "名前の [表示] を外すと、合成のソースの絵の名前ラベルの板が狭まるか消える", onFail: "record" },
  { item: "ac32", what: "声のミュートの往復", line: "ミュートは無音、外し直すと基準の ±" + AC32_GAIN_TOLERANCE_DB + " dB に戻る", onFail: "record" },
  { item: "voice-source", what: "使う声の置き換え", line: "置き換えると声を借りる人の枠で 1 kHz、戻すと声を貸す人の枠で 1 kHz", onFail: "record" },
  { item: "voice-volume", what: "声の音量 " + VOICE_VOLUME_PCT + "%", line: "100% の録画に対して +" + (Math.round(20 * Math.log10(VOICE_VOLUME_PCT / 100) * 100) / 100) + " dB ±" + VOICE_VOLUME_GAIN_TOLERANCE_DB + " dB (切れうる大きさなら下限だけ見る)", onFail: "record" },
  { item: "roster-labels", what: "実際のアバターの並び (画面共有していない参加者)", line: "共有を止めた後に、並びの人数の行 (Composite roster shows N) の最後の値が、期待の人数 (通話の 4 つのアカウントから、別アカウント方式のサブアカウントを引いた数。メインアカウントも並ぶ) と等しい (絵も PNG で目で読む)", onFail: "record" },
  { item: "version-row", what: "Vesktop の版の表示", line: "Vesktop の行の値が数字を含む", onFail: "record" },
  { item: "screen-audio", what: "画面共有の音のミュートを外すと合成のソースの音に届く (--recheck)", line: "合成のソースの入力の peak が " + INPUT_LEVEL_THRESHOLD_DBFS + " dBFS より大きい。記録に screen_audio の placed_frames が 1 以上の行がある", onFail: "record" },
  { item: "format-change", what: "共有の途中の解像度の変化の録画 (--recheck)", line: "frames.py の結果で、全部の枠が crc_failures 0、unreadable_frames 0、max_freeze_ms " + RESCHANGE_MAX_FREEZE_MS + " 以下 (絵は PNG を目で読む)", onFail: "record" },
  { item: "encode-limits", what: "符号化の設定が送信側へ送られる (--recheck)", line: "記録に 'Quality limits sent to Vesktop' の行があり、encoder=software latency=quality", onFail: "record" },
  { item: "encode-choice", what: "送信側が符号化の設定で確保し直す (--recheck)", line: "記録に、送信側の stage の行が preference=prefer-software latencyMode=quality で出る", onFail: "record" },
  { item: "self-frame", what: "自分の枠 (--recheck。枠に載せる人の選び方は自動)", line: "メインアカウントが共有を始めると 'Self frame turned on: screen share'・枠が 1 つ増えて self= が入る・共有を止めても外れず枠も変わらない・自分の行のチェックを外すと 'Self frame turned off: dock' で枠が元の数・付けると 'Self frame turned on: dock' で 1 つ増えて self= が入る・外すと元の数", onFail: "record" },
  { item: "receive-continues", what: "スピーカーミュートと最小化の間も受け取りが続く (--recheck)", line: "stats video の received_hz が止まらない (直近 " + VIDEO_RATE_WINDOW_LINES + " 行のどれかが " + VIDEO_STOPPED_HZ + " 以上)", onFail: "record" },
  { item: "rejoin", what: "受け手の Vesktop の抜け戻り (--recheck)", line: "抜けても close と hello が増えず、戻ると映像が受け取られる", onFail: "record" },
  { item: "connections-kept", what: "Vesktop が共有と本編で落ちない", line: "close と hello が増えない", onFail: "record" },
  { item: "align-drift", what: "整列のずれの傾き", line: "|傾き| <= " + ALIGN_DRIFT_MAX_PERCENT + "% (10 分以上続いた区切り)", onFail: "record" },
  { item: "display-delay", what: "表示の遅れの測り", line: "OBS の記録の video_latency の shown_n が 0 でない", onFail: "record" },
  { item: "audio-buffering", what: "OBS の全体の音の待ち時間の増え", line: "線は決めていない (行の数と出どころを記録するだけ)", onFail: "record" },
];

// 結果 (state.results) から JUDGE_LINES の行ごとの答えを集める (judge-lines の手順)。ok は「答えの出た行に外れがない」。
// 行の ok が null (この実行にない・判定できない) は数えない。
// opts.items があれば、その項目 (JUDGE_LINES の item) だけを並べる (--recheck の RECHECK_JUDGE_ITEMS)。
export function judgeLinesSummary(results, opts) {
  const list = results || [];
  const only = opts && Array.isArray(opts.items) ? new Set(opts.items) : null;
  const last = (pred) => {
    for (let i = list.length - 1; i >= 0; i -= 1) if (list[i] && pred(list[i])) return list[i];
    return null;
  };
  const pick = (r, fields) => (r ? Object.fromEntries(fields.filter((f) => r[f] !== undefined).map((f) => [f, r[f]])) : null);
  // --recheck で足した行は、複数の結果を 1 行にまとめる。1 つでも外れなら失敗、全部が合格なら合格、どれかがない・判定できないなら判定できない
  // (ok が null)。何も無ければ実行していない行 (null)。
  const logOf = (label) => last((r) => r.name === "judge-log-lines" && r.label === label);
  const checksOf = (r) => (r && Array.isArray(r.checks) ? r.checks.map((c) => ({ id: c.id, ok: c.ok, count: c.count, last: c.last })) : null);
  const combine = (parts, detail) => {
    const have = parts.filter(Boolean);
    if (have.length === 0) return null;
    const bad = have.find((p) => p.ok === false);
    const ok = bad ? false : have.length === parts.length && have.every((p) => p.ok === true) ? true : null;
    return { ok, reason: bad ? bad.reason || bad.error || null : null, derived: true, detail };
  };
  const rateOf = (label) => {
    const r = last((x) => x.name === "measure-video-rate" && x.label === label);
    if (!r) return null;
    const receivedHz = Array.isArray(r.samples) ? r.samples.map((s) => s.receivedHz) : [];
    const stopped = r.framesStopped;
    return {
      ok: stopped === true ? false : stopped === false ? true : null,
      reason: stopped === true ? "video stopped (" + receivedHz.join(", ") + " Hz)" : stopped === null || stopped === undefined ? "too few stats video lines to decide" : null,
      receivedHz,
    };
  };
  const meter = last((r) => r.name === "measure-input-level" && r.label === SCREEN_AUDIO_LEVEL_LABEL);
  const placed = logOf("sa-after");
  // 解像度の変化の録画の frames.py の結果。全部の枠 (区画) が、CRC の失敗・読めないフレーム 0 で、フリーズが上限以下なら合格。
  // 数を読めない枠 (帯が見つからないなど) があれば、外れの材料がなくても合格にはしない (判定できない)。
  const reschangeFrames = (() => {
    const r = last((x) => x.name === "judge" && x.tool === "frames.py" && x.label === "reschange-frames");
    if (!r) return null;
    if (r.ok === false) return { ok: false, reason: r.error || "frames.py failed", derived: true, detail: null };
    const cells = Array.isArray(r.perCell) ? r.perCell.map((c) => ({ cell: c.cell, data: c.data })) : r.data ? [{ cell: 0, data: r.data }] : [];
    const isCount = (v) => typeof v === "number" && Number.isFinite(v);
    const detail = cells.map((c) => {
      const d = c.data || {};
      const known = isCount(d.crc_failures) && isCount(d.unreadable_frames) && isCount(d.max_freeze_ms);
      const bad = known && (d.crc_failures > 0 || d.unreadable_frames > 0 || d.max_freeze_ms > RESCHANGE_MAX_FREEZE_MS);
      return { cell: c.cell, crcFailures: d.crc_failures ?? null, unreadableFrames: d.unreadable_frames ?? null, maxFreezeMs: d.max_freeze_ms ?? null, barcodeFound: d.barcode_found ?? null, ok: known ? !bad : null };
    });
    const bad = detail.find((c) => c.ok === false);
    const ok = bad ? false : detail.length > 0 && detail.every((c) => c.ok === true) ? true : null;
    return { ok, reason: bad ? "cell " + bad.cell + ": crc_failures=" + bad.crcFailures + " unreadable_frames=" + bad.unreadableFrames + " max_freeze_ms=" + bad.maxFreezeMs : null, derived: true, detail };
  })();
  // 判定しない項目 (リハーサルの skip。REHEARSAL_NOT_JUDGED): part のない skip は行ごと判定しない、part のある skip はその読みを
  // まとめから外す。
  const skips = list.filter((r) => r && r.name === "skip" && typeof r.item === "string");
  const skippedWhole = new Map(skips.filter((r) => !r.part).map((r) => [r.item, r.reason || null]));
  const skippedParts = (item) => new Set(skips.filter((r) => r.item === item && r.part).map((r) => r.part));
  // 自分の枠: 読みの名前ごとの結果 (SELF_FRAME_READ_LABELS)。リハーサルで判定しない読みは外す。
  const selfFrameParts = SELF_FRAME_READ_LABELS.filter((label) => !skippedParts("self-frame").has(label));
  const selfFrameDetail = Object.fromEntries(SELF_FRAME_READ_LABELS.map((label) => [label, skippedParts("self-frame").has(label) ? "not judged" : checksOf(logOf(label))]));
  const keptLeft = last((r) => r.name === "judge-connections-kept" && r.label === VESKTOP_LEFT_LABEL);
  const rateMute = rateOf(RECEIVE_CONTINUES_LABEL);
  const rateBack = rateOf(VESKTOP_REJOIN_LABEL);
  const found = {
    "screen-audio": combine([meter, placed], { maxPeakDbfs: meter ? meter.maxPeakDbfs : null, samples: meter ? meter.samples : null, placed: checksOf(placed) }),
    "format-change": reschangeFrames,
    "encode-limits": combine([logOf("encode-limits")], { checks: checksOf(logOf("encode-limits")) }),
    "encode-choice": combine([logOf("encode-choice")], { checks: checksOf(logOf("encode-choice")) }),
    "self-frame": selfFrameParts.length > 0 ? combine(selfFrameParts.map(logOf), selfFrameDetail) : null,
    "receive-continues": combine([rateMute], { receivedHz: rateMute ? rateMute.receivedHz : null }),
    rejoin: combine([keptLeft, rateBack], { connectionsKept: keptLeft ? { ok: keptLeft.ok, closes: keptLeft.closes, hellos: keptLeft.hellos } : null, receivedHz: rateBack ? rateBack.receivedHz : null }),
    "add-to-vesktop": last((r) => r.name === "human" && r.until === "state-json-points-to-placed"),
    "vesktop-connects": last((r) => r.name === "wait-connection" && r.baseline === ADD_TO_VESKTOP_BASELINE_LABEL),
    "self-account": last((r) => r.name === "wait-self-account"),
    "av-sync-main": last((r) => r.name === "judge-av-sync" && r.recording === "main"),
    "av-sync-av1": last((r) => r.name === "judge-av-sync" && r.recording === "av1"),
    "av-sync-av2": last((r) => r.name === "judge-av-sync" && r.recording === "av2"),
    "av-drift": last((r) => r.name === "judge-av-drift"),
    "record-sync": last((r) => r.name === "judge-record-sync"),
    latency: last((r) => r.name === "judge-latency"),
    ac30: last((r) => r.name === "judge-composite-layout"),
    ac31: last((r) => r.name === "judge-ac31"),
    ac32: last((r) => r.name === "judge-ac32"),
    "voice-source": last((r) => r.name === "judge-voice-source"),
    "voice-volume": last((r) => r.name === "judge-voice-volume"),
    "roster-labels": last((r) => r.name === "judge-roster-labels"),
    "version-row": last((r) => r.name === "judge-rt" && r.judge === "version-row-shown"),
    "connections-kept": null,
    "align-drift": last((r) => r.name === "judge-call-log"),
    "display-delay": last((r) => r.name === "judge-display-delay"),
    "audio-buffering": last((r) => r.name === "judge-audio-buffering"),
  };
  const kept = list.filter((r) => r && r.name === "judge-connections-kept");
  const keptOk = kept.length === 0 ? null : kept.every((r) => r.ok === true);
  // A/V の行の傾きの読み (judgeAvTrend): 時間とともに広がるか (verdict と基準の傾き) と、一時的な段の数と高さを分けて残す。
  const trendDetail = (t) =>
    t
      ? {
          trend: t.verdict,
          slopeMsPerMin: t.slopeMsPerMin,
          ci95MsPerMin: t.ci95MsPerMin ?? null,
          fitOn: t.fitOn ?? null,
          steps: Array.isArray(t.steps) ? t.steps.map((x) => ({ startS: x.startS, endS: x.endS, heightMs: x.heightMs, returned: x.returned })) : null,
        }
      : { trend: null };
  const rows = JUDGE_LINES.filter((line) => !only || only.has(line.item)).map((line) => {
    // 判定しない行 (リハーサルの skip、または判定の側が判定しないとした行): 答えは null で、理由を残す。
    const notJudged = skippedWhole.has(line.item) ? skippedWhole.get(line.item) || "not judged in this run" : found[line.item] && typeof found[line.item].notJudged === "string" ? found[line.item].notJudged : null;
    if (notJudged !== null) {
      return { item: line.item, what: line.what, line: line.line, onFail: line.onFail, ok: null, notJudged, reason: "not judged: " + notJudged, detail: null };
    }
    const r = found[line.item];
    const ok = line.item === "connections-kept" ? keptOk : r ? (r.ok === undefined ? null : r.ok) : null;
    const detail =
      r && r.derived === true
        ? r.detail
        : line.item === "connections-kept"
        ? kept.map((x) => ({ label: x.label, ok: x.ok, closes: x.closes, hellos: x.hellos }))
        : line.item.startsWith("av-sync")
          ? r && Array.isArray(r.cells) ? r.cells.filter((c) => c.ok !== null).map((c) => ({ cell: c.cell, ...(r.person ? { person: r.person } : {}), meanMs: c.meanMs, paired: c.paired, ...trendDetail(c.trend) })) : null
          : line.item === "record-sync"
            ? r && r.lines ? { meanMs: r.lines.meanMs, paired: r.lines.paired, ...trendDetail(r.lines.trend) } : null
            : line.item === "align-drift"
              ? r && Array.isArray(r.align) ? r.align.map((a) => ({ stream: a.stream, judged: a.judged, ok: a.ok, slopePercent: a.slopePercent, restarts: a.restarts })) : null
              : pick(r, ["reason", "measuredMs", "estimatedMs", "designQuestion", "value", "orientation", "soundingSlots", "presentBefore", "goneAfterRemove", "backAfterRestore", "cells", "satisfiedBy", "timedOut", "names", "counts", "mode", "hasSelf", "totalShown", "withoutShown", "count", "handRead", "finalTotalMs", "bySource", "gainDb", "expectedDb", "errorDb", "indexes", "addedLines", "complete", "people", "contiguous", "expectedPeople", "connected", "factsYes", "factsDash", "firstYesAfterSec", "yesBeforeConnection", "waitedMs"]);
    return {
      item: line.item,
      what: line.what,
      line: line.line,
      onFail: line.onFail,
      ok,
      // 合格でない行は理由を残す (判定できない行 = ok null も、理由があれば。テスト動画の人の枠が見つからないなど)。
      reason: r && r.ok !== true ? r.reason || r.error || null : null,
      detail,
    };
  });
  const red = rows.filter((x) => x.ok === false);
  return result(red.length === 0, "outside the line: " + red.map((x) => x.item).join(", "), { rows, red: red.map((x) => x.item) });
}

// 実機の実行の期限の見積もり (分)。実行計画の見込み (plannedSeconds = wait / human の見込み / tail) に、判定ツールの読み
// (録画の長さ x 区画の数 x JUDGE_SECONDS_PER_RECORDED_SECOND + 1 回ごとの起動)、judge-record・条件待ち・
// 他の手順の分を足し、DEADLINE_MARGIN_FACTOR を掛け、人の操作の遅れ (humanOverrunMin) を足す。
// 録画の長さは、record-start から対の record-stop までの見込み (OBS の録画は 1 つずつなので、record-stop は直前の
// record-start と対。label を足していない手順の label は説明の文で始めと終わりの文字列が違うため、label では対にしない)。
// 判定が見る録画は recording の label、無ければ直前に止めた録画。
export function deadlineEstimate(steps, opts) {
  const o = opts || {};
  const humanOverrunMin = typeof o.humanOverrunMin === "number" ? o.humanOverrunMin : CALL_ONE_HUMAN_OVERRUN_MIN;
  const list = steps || [];
  const recordedSeconds = {};
  let openKey = null;
  let openAt = 0;
  let cumulative = 0;
  let lastRecording = null;
  let judgeSeconds = 0;
  let judgeRecordSeconds = 0;
  let conditionSeconds = 0;
  let otherSteps = 0;
  for (const s of list) {
    if (s.kind === "record-start") {
      openKey = s.label || "_last";
      openAt = cumulative;
    }
    cumulative += stepPlannedSeconds(s);
    if (s.kind === "record-stop") {
      const key = openKey || s.label || "_last";
      const seconds = openKey ? cumulative - openAt : 0;
      recordedSeconds[key] = seconds;
      if (s.label && s.label !== key) recordedSeconds[s.label] = seconds;
      lastRecording = key;
      openKey = null;
    }
    if (s.kind === "judge") {
      const recKey = s.recording || lastRecording;
      const seconds = recKey && recordedSeconds[recKey] !== undefined ? recordedSeconds[recKey] : 0;
      const passes = s.tool === "smooth.py" ? 1 : Math.max(1, Number(s.cells) || 1);
      const rate = JUDGE_SECONDS_PER_RECORDED_SECOND[s.tool] ?? Math.max(...Object.values(JUDGE_SECONDS_PER_RECORDED_SECOND));
      judgeSeconds += passes * (seconds * rate + JUDGE_PASS_OVERHEAD_S);
    } else if (s.kind === "judge-latency") {
      // latency.py の速さは測っていないので、判定ツールの中で一番遅い率で見込む。
      const seconds = s.recording && recordedSeconds[s.recording] !== undefined ? recordedSeconds[s.recording] : 0;
      judgeSeconds += seconds * Math.max(...Object.values(JUDGE_SECONDS_PER_RECORDED_SECOND)) + JUDGE_PASS_OVERHEAD_S;
    } else if (s.kind === "judge-record" || s.kind === "judge-record-sync" || s.kind === "judge-voice-source" || s.kind === "judge-voice-volume" || s.kind === "judge-ac32") judgeRecordSeconds += JUDGE_RECORD_ALLOWANCE_S;
    else if (s.kind === "wait-connection" || s.kind === "wait-finalize" || s.kind === "wait-self-account") conditionSeconds += CONDITION_WAIT_ALLOWANCE_S;
    else if (!["wait", "human", "record-stop"].includes(s.kind)) otherSteps += 1;
  }
  const planned = plannedSeconds(list);
  const otherSeconds = otherSteps * STEP_OVERHEAD_S;
  const totalSeconds = (planned + judgeSeconds + judgeRecordSeconds + conditionSeconds + otherSeconds) * DEADLINE_MARGIN_FACTOR + humanOverrunMin * 60;
  const toMin = (sec) => Math.round((sec / 60) * 10) / 10;
  return {
    minutes: Math.ceil(totalSeconds / 60),
    plannedMinutes: toMin(planned),
    judgeMinutes: toMin(judgeSeconds),
    judgeRecordMinutes: toMin(judgeRecordSeconds),
    conditionWaitMinutes: toMin(conditionSeconds),
    otherStepMinutes: toMin(otherSeconds),
    marginFactor: DEADLINE_MARGIN_FACTOR,
    humanOverrunMinutes: humanOverrunMin,
  };
}

// 使う期限 (分)。--deadline-min を渡していればそのまま。call-1 の実際の回は見積もり (deadlineEstimate) と
// CALL_ONE_DEADLINE_MIN (180) の大きいほう。ほかの step とリハーサルは parseArgs が決めた値のまま。
// 180 分の既定だと、締めの利用者の操作 (Vesktop を閉じる) の最中に期限に達し、アンインストールと締めの判定が実行されなくなるため。
export function deadlineMinFor(args, steps) {
  if (!args) return DEFAULT_DEADLINE_MIN;
  if (args.deadlineMinGiven) return args.deadlineMin;
  if (args.step === "call-1" && !args.rehearsal) return Math.max(CALL_ONE_DEADLINE_MIN, deadlineEstimate(steps).minutes);
  // call-hold: 開いたまま待つ分 (human の見込み = --hold-minutes) と締めの人の操作を含む見積もり。既定 120 分より
  // 短ければ 120 (期限に達すると締めのアンインストールが実行されず、利用者の Vesktop がテスト用にインストールしたファイルを指したまま残る)。
  if (args.step === "call-hold") return Math.max(DEFAULT_DEADLINE_MIN, deadlineEstimate(steps, deadlineEstimateOptsFor(args)).minutes);
  return args.deadlineMin;
}

// 見積もりの人の遅れ (分)。call-hold は CALL_HOLD_HUMAN_OVERRUN_MIN、ほかは deadlineEstimate の既定 (call-1 の値)。
// --dry-run の出力 (deadlineEstimate) と deadlineMinFor が同じ値で数えるため。
export function deadlineEstimateOptsFor(args) {
  return args && args.step === "call-hold" ? { humanOverrunMin: CALL_HOLD_HUMAN_OVERRUN_MIN } : {};
}

// ---- 録画の確認 (--step record) ------------------------------------------------
//
// **実機の実行では 2 つの録画が同時に走り、この 2 つが対の判定になる**:
// 黒の区間が cordmux の mov に有り、同じ時刻の OBS の録画にないことを
// 両方確認すれば、フラグの配線が意図どおりに適用されていることを
// 機械で確認できる。確認するのは「最初の実フレームより前に黒が出ていないこと」に絞る
// (共有の終わりの黒は正当に出うるので、そこで確認すると偽の失敗になる)。
//
// 順序で外せない点:
//   - wait-finalize を close-obs と後始末より前に置く (cleanup.ps1 は _work 配下の
//     ffmpeg.exe を終了させるので、録画ファイルの作成の途中で後始末に入ると出力が壊れる)。
//   - 空き容量の警告は録画中にしか出ないので OFF の前に置く。
//   - --assign-mode auto を渡す (手動の既定では共有を始めただけでは誰も枠に入らず、
//     OBS 側の録画が黒のままになって対の判定が成り立たない)。

// テスト用の送信側を 1 つ起動して、繋がるのを待つまで (繋ぎ直しのたびに同じ形を繰り返す)。
function recordHarnessSteps(args, index, seconds, extra) {
  const label =
    index === 1
      ? "テスト用の送信側を product モードで画面 " + RECORD_N_SCREEN + " 本 + 声 " + RECORD_N_VOICE + " 本、" + seconds + " 秒 (1 回目の接続)"
      : index > RECORD_RECONNECT_COUNT + 1
        ? "起動し直した OBS へもう 1 つつなぐ (録画が OFF に戻っていることの判定に中身を持たせるため。" + seconds + " 秒)"
        : "テスト用の送信側を起動し直す (" + (index - 1) + " 回目の繋ぎ直し。新しいプロセス = 新しいエンコーダ = 新しい SPS/PPS)";
  return [
    step("harness-start", label, {
      mode: "product",
      nScreen: RECORD_N_SCREEN,
      nVoice: RECORD_N_VOICE,
      seconds,
      hw: args.hw,
      warmup: args.warmup,
      syncTone: DEFAULT_SYNC_TONE,
      ...(extra ? { extra } : {}),
    }),
    step("wait-connection", "送信側が接続を確立するまで待つ", { timeoutMs: RECORD_CONNECT_WAIT_TIMEOUT_MS }),
  ];
}

function buildRecordSteps(args) {
  // --assign-mode auto を渡す。呼び出し行の指定より実行計画を優先する
  // (手動のままだと OBS 側の録画が黒のままになり、対の判定が成り立たない)。
  const recordArgs = { ...args, dockAssignMode: "auto" };
  const steps = commonSetupSteps(recordArgs);

  steps.push(wireCompositeStep("画面 " + RECORD_N_SCREEN + " 本を 4 等分、声 " + RECORD_N_VOICE + " 本は合成のソースの音"));
  // OBS 側の録画 (対の判定の相方)。テスト用の送信側より前に始める。
  steps.push(step("record-start", "OBS 側の録画を開始する (対の判定の相方。cordmux の録画とは別物)", { stream: false }));

  // 1 回目の接続: 送信中に解像度を 2 回変える。
  steps.push(
    ...recordHarnessSteps(recordArgs, 1, RECORD_HARNESS_FIRST_SECONDS, {
      resolutionSchedule: resolutionScheduleArg(RECORD_RESOLUTION_SCHEDULE),
    }),
  );
  steps.push(step("wait", "実映像が貯まるまでの猶予 (**ここで貯めることが「録画の途中での ON」の形そのもの**)", { ms: RECORD_SETTLE_BEFORE_ON_MS }));

  // 録画の途中での ON。dock-click.ps1 の UI Automation で押す。
  steps.push(
    step("dock-click", "ドックの [" + TEXT_ID_RECORD_START + "] を押してcordmuxの録画を始める (録画の途中での ON)", {
      target: "first",
      textId: TEXT_ID_RECORD_START,
      desired: "on",
    }),
  );
  steps.push(step("wait", "録画の素材が作られ、最初のキーフレームから区間が開くまでの猶予", { ms: RECORD_SETTLE_AFTER_ON_MS }));
  steps.push(step("screenshot-window", "録画を始めた直後のドックを PNG に撮る (ボタンのテキストと状態)", { target: "first", shotLabel: "record-on" }));

  // 1 回目の接続の送信側が自分で終わるのを待つ (**強制終了しない**: 送信と終了の経路で出る材料を得るため)。
  steps.push(step("harness-wait", "1 回目の接続の送信側が自分で終わるのを待つ (送信と終了の経路で出る材料を得るため。強制終了しない)", { seconds: RECORD_HARNESS_FIRST_SECONDS }));

  // 繋ぎ直し。
  for (let i = 2; i <= RECORD_RECONNECT_COUNT + 1; i += 1) {
    steps.push(step("wait", "前のプロセスが消えてから次を起こすまでの猶予", { ms: RECORD_RECONNECT_GAP_MS }));
    steps.push(...recordHarnessSteps(recordArgs, i, RECORD_HARNESS_RECONNECT_SECONDS, null));
    steps.push(step("harness-wait", (i - 1) + " 回目の繋ぎ直しの送信側が終わるのを待つ", { seconds: RECORD_HARNESS_RECONNECT_SECONDS }));
  }

  // 空き容量の警告は録画中にしか出ないので、OFF より前に撮る。
  steps.push(step("screenshot-window", "空き容量の警告が出ているドックを PNG に撮る (OFF より前)", { target: "first", shotLabel: "record-space" }));

  steps.push(
    step("dock-click", "ドックの [" + TEXT_ID_RECORD_STOP + "] を押してcordmuxの録画を終える", {
      target: "first",
      textId: TEXT_ID_RECORD_STOP,
      namePrefix: true,
      desired: "off",
    }),
  );

  // OBS 側の録画はここで止める (cordmux の録画ファイルの作成を待つ前。録画ファイルの作成は OBS の録画と無関係)。
  steps.push(step("record-stop", "OBS 側の録画を終了する", { stream: false, tailSeconds: 0 }));

  // **wait-finalize は close-obs と cleanup より前**。
  steps.push(
    step("wait-finalize", "録画ログ (ledger.jsonl) に録画ファイルの作成の行が出そろうのを待つ (cleanup が _work の ffmpeg.exe を止める前に終わらせる)", {
      recordDirRel: RECORD_DIR_REL,
      timeoutMs: RECORD_FINALIZE_WAIT_TIMEOUT_MS,
      pollMs: RECORD_FINALIZE_POLL_MS,
      gate: true,
    }),
  );
  steps.push(step("collect-recording", "OBS 側の録画ファイルの回収"));
  steps.push(step("collect-record-session", "cordmux の録画のフォルダ (録画ログ・区間・黒・完成) の一覧を結果に残す", { recordDirRel: RECORD_DIR_REL }));

  // 判定ツール 3 つ。**frames.py は使わない**。
  steps.push(step("judge-record", "ledger_check.py: 録画ログと ffprobe を整数で突き合わせる (長さ・フレーム数・サイズ・fps・音の有無)", { tool: "ledger_check.py", gate: true }));
  steps.push(step("judge-record", "au_hash.py: 送った AU と完成の AU の同一性 (区間ごとの framemd5)", { tool: "au_hash.py", gate: true }));
  steps.push(
    step("judge-record", "black_span.py: cordmux の完成の最初の実フレームより前に黒がないこと (対の判定の片側)", {
      tool: "black_span.py",
      mode: "no-leading-black",
      target: "cordmux",
      gate: true,
    }),
  );
  // 黒の区間が録画ログの切れ目と一致すること。**対の判定の cordmux 側
  // 「黒が有る」はここで確認する** (上の no-leading-black は「先頭に黒がない」しか確認しない)。
  // 録画ログの録画ファイルの作成の行の gaps を、その人の区間の行の fps でフレーム番号に変換して渡す
  // (組み立ては run-window.mjs の buildGapFrames)。
  steps.push(
    step("judge-record", "black_span.py: 黒の区間が録画ログの欠落と 1 対 1 で合うこと (対の判定の cordmux 側「黒が有る」)", {
      tool: "black_span.py",
      mode: "match-gaps",
      target: "cordmux",
      gate: true,
    }),
  );
  steps.push(
    step("judge-record", "black_span.py: 同じ時刻の OBS の録画に黒がないこと (対の判定の OBS 側「黒がない」)", {
      tool: "black_span.py",
      mode: "no-black",
      target: "obs",
      grid: "2x2",
      gate: true,
    }),
  );
  steps.push(step("collect-plugin-log", "プラグインのログ ([cordmux] 行) の回収"));

  // 同じ OBS の起動の中で 2 回目の録画をする。1 回目のセッションにいた人がまたいるので、セッションの始まりで前の
  // セッションの値 (人の行の表など) を残していると、ここで区間を開いたときに落ちる。1 回目の判定の後に置く
  // (判定のツールは、いちばん新しいセッションのフォルダーを読むため)。
  steps.push(
    step("harness-start", "テスト用の送信側をもう 1 つつなぐ (同じ OBS の起動の中での 2 回目の録画のため。" + RECORD_SECOND_SESSION_SECONDS + " 秒)", {
      mode: "product",
      nScreen: RECORD_N_SCREEN,
      nVoice: RECORD_N_VOICE,
      seconds: RECORD_SECOND_SESSION_SECONDS,
      hw: recordArgs.hw,
      warmup: recordArgs.warmup,
      syncTone: DEFAULT_SYNC_TONE,
    }),
    step("wait-connection", "送信側が接続を確立するまで待つ", { timeoutMs: RECORD_CONNECT_WAIT_TIMEOUT_MS }),
    step("dock-click", "ドックの [" + TEXT_ID_RECORD_START + "] を押して、同じ OBS の起動の中で 2 回目のcordmuxの録画を始める", {
      target: "first",
      textId: TEXT_ID_RECORD_START,
      desired: "on",
    }),
    step("wait", "2 回目の録画の区間が開くまでの猶予", { ms: RECORD_SETTLE_AFTER_ON_MS }),
    step("harness-wait", "2 回目の録画の送信側が終わるのを待つ", { seconds: RECORD_SECOND_SESSION_SECONDS }),
    step("dock-click", "ドックの [" + TEXT_ID_RECORD_STOP + "] を押して、2 回目のcordmuxの録画を終える", {
      target: "first",
      textId: TEXT_ID_RECORD_STOP,
      namePrefix: true,
      desired: "off",
    }),
    step("wait-finalize", "2 回目のcordmuxの録画ファイルの作成が終わるのを待つ", {
      recordDirRel: RECORD_DIR_REL,
      timeoutMs: RECORD_FINALIZE_WAIT_TIMEOUT_MS,
      pollMs: RECORD_FINALIZE_POLL_MS,
      gate: true,
    }),
    step("judge-record-second", "2 回目の録画のフォルダーが 1 回目と別にでき、録画ログに人の行と区間の行があること", {
      recordDirRel: RECORD_DIR_REL,
      gate: true,
    }),
  );
  steps.push(...closeSteps("first"));

  // 起動し直すと録画は OFF に戻っている (録画の状態を覚えない)。
  // **もう 1 つつないでから確認する**: 区間は最初のキーフレームが来るまで開かない
  // ので、送信側が居なければ録画が ON でも保存先は増えない。繋いだうえで増えないことを
  // 確認して初めて判定になる (繋がずに確認すると、この判定は必ず合格になって意味を持たない)。
  steps.push(...launchAndConnectSteps(recordArgs));
  steps.push(
    ...recordHarnessSteps(recordArgs, RECORD_RECONNECT_COUNT + 2, RECORD_AFTER_RESTART_SECONDS, null),
  );
  steps.push(step("wait", "ドックが引き直され、録画が ON なら区間が開くだけの猶予", { ms: RECORD_SETTLE_AFTER_ON_MS }));
  steps.push(step("screenshot-window", "起動し直した後のドックを PNG に撮る (録画は OFF に戻っている)", { target: "first", shotLabel: "record-after-restart" }));
  steps.push(step("judge-record-off", "送信側が繋がっているのに録画のフォルダが増えないことを確認する (settings に録画のキーがないことも確認する)", { recordDirRel: RECORD_DIR_REL, gate: true }));
  steps.push(step("harness-wait", "起動し直した後の送信側が終わるのを待つ", { seconds: RECORD_AFTER_RESTART_SECONDS }));
  steps.push(...closeSteps("first"));
  steps.push(...finishSteps(recordArgs, "record"));
  return steps;
}

// ---- WebCodecs の確認 (--step webcodecs-check) --------------------------------
//
// 次の 2 つを測る: (a) エンコーダを作った直後に 1 秒ぶんの黒をエンコードしたときの最初の実フレームの
// 遅れ、(b) 同じ設定の 2 つ目のインスタンスの SPS/PPS が 1 つ目とビット一致するか。
// **OBS は一切起動しない** (mic-check と同じ形。commonSetupSteps を使わない)。
// **実機の実行の最初に測る**: (b) が不一致なら ProRes の経路へ落ちる人が増え、作る範囲と容量の見積もりが
// 変わる。
function buildWebCodecsCheckSteps(args) {
  return [
    step(
      "webcodecs-check",
      "WebCodecs のエンコーダで (a) configure から最初の出力まで・黒 60 フレームのエンコードの所要・黒の最後から最初の実フレームまで、" +
        "(b) 同じ設定の 2 つを直列に確保した SPS/PPS の 16 進の比較 (ハード x ハード / ハード x ソフト / ソフト x ソフト / 同じインスタンスの 2 回目) を測る",
      { gate: true },
    ),
    ...finishSteps(args, "webcodecs-check"),
  ];
}

// ---- インストールの往復 (--step install-roundtrip) -------------------------------------
//
// 流れ: 先頭の共通のガード (main()) -> _work/rt-<tag>/ を作り直して OBS の zip を展開し
// 修正する -> 利用者らしい設定の形 (seed-config.py --shape user) -> cordmux なしで 2 回起動してスナップショットを取る
// (較正 = 揺れ) -> zip を置く -> 起動して [Vesktop に追加]・ダイアログ・[cordmux のシーンを作成] (2 つ目のコレクション
// でも) -> 閉じてスナップショットを取る (インストール後) -> アンインストール -> 置いたものを消す (人の手の代わり) -> cordmux なしで起動して
// スナップショットを取る (アンインストール後) -> 後始末。**スナップショットは後始末より前に取る** (cleanup が作業場所を消す)。
// 場面ごとの流れは、このコンポーネントを組み替えて作る。スナップショットを取ることと比べることは install_snapshot.py。
//
// 較正の相手 (インストール前) を 1 回目の後にしたのは、seed-config.py が書いた設定を OBS が最初の起動で
// 書き直す (初回の移し替え) ためで、その書き直しは 2 回目との差 (揺れ) に入る。

function rtLaunchArgs(args) {
  return args.layout === "programdata" ? OBS_LAUNCH_ARGS_INSTALLED : OBS_LAUNCH_ARGS;
}

function rtSnapshotRoots(args) {
  const roots = ["obs_config", "obs_program", "appdata", "localappdata", "custom_vencord"];
  if (args.layout === "programdata") roots.push("programdata_plugins");
  return roots;
}

function rtPrepareSteps(args) {
  const p = roundtripPaths(args.tag);
  return [
    step("rt-prepare", "Recreate the work folder " + p.workRel + " (delete, then make it empty)", { workRel: p.workRel, gate: true }),
    step("rt-extract", "Extract the OBS zip into " + p.obsRel, { zip: args.obsZip, destRel: p.obsRel, what: "obs", gate: true }),
  ];
}

// opts.custom: 自作の Vencord の場所を vencordDir に書く。opts.periods: 照合の周期の 2 鍵を短くする。
function rtSeedStep(args, opts) {
  const p = roundtripPaths(args.tag);
  const o = opts || {};
  const programdata = args.layout === "programdata";
  return step(
    "seed-config",
    "Write a user-like OBS setup (seed-config.py --shape user: 2 collections, sources, a filter, a volume, a profile; nothing of cordmux)" +
      (o.custom ? "; state.json points to a self-built Vencord" : "") +
      (o.periods ? "; short Vencord check periods (" + ROUNDTRIP_CHECK_PERIOD_S + " s)" : ""),
    {
      obs: p.obsRel,
      out: args.out,
      step: INSTALL_ROUNDTRIP_STEP,
      shape: "user",
      realProfile: false,
      useAppData: true,
      appDataRel: p.appDataRel,
      allowStateJsonWrite: false,
      recordDirRel: RECORD_DIR_REL,
      vencordState: o.custom ? "custom" : "absent",
      customVencordRel: o.custom ? p.customVencordRel : null,
      ...(o.periods ? { vencordCheckDisconnectedS: ROUNDTRIP_CHECK_PERIOD_S, vencordCheckConnectedS: ROUNDTRIP_CHECK_PERIOD_S } : {}),
      // --layout programdata (VM だけ): ポータブルでない OBS の設定の保存先へ書き、portable_mode.txt を置かない。
      installedObs: programdata,
      vm: Boolean(args.vm),
    },
  );
}

// opts.cordmux: cordmux を置いた後の起動 (Loaded Modules に cordmux.dll があることを見る)。
// opts.gateLoaded (既定 true): 読み込まれたことをゲートにする (old-obs / newer-libobs は答えを見る場面なので外す)。
// opts.obsRel: 別の場所の OBS を起こす (move の改名の後)。opts.tolerant: obs-websocket に繋がらなくても止めない。
function rtLaunchSteps(args, label, opts) {
  const o = opts || {};
  const steps = [
    step("launch-obs", label + " (LOCALAPPDATA / APPDATA swapped to " + roundtripPaths(args.tag).workRel + ")", {
      obs: o.obsRel || roundtripPaths(args.tag).obsRel,
      launchArgs: rtLaunchArgs(args),
      wsPort: OBSWS_CONTROL_PORT,
      realProfile: false,
    }),
    step("obsws-connect", "Connect to obs-websocket", { wsPort: OBSWS_CONTROL_PORT, ...(o.tolerant ? { tolerant: true } : {}) }),
  ];
  if (o.cordmux) {
    steps.push(
      step("read-obs-log", "cordmux.dll is in the Loaded Modules section of the OBS log", {
        pattern: "module-loaded",
        moduleName: "cordmux",
        ...(o.gateLoaded === false ? {} : { gate: true }),
      }),
    );
  }
  steps.push(step("wait", "Let OBS settle", { ms: o.settleMs || ROUNDTRIP_SETTLE_MS }));
  return steps;
}

function rtCloseStep(label) {
  return step("close-obs", label, { target: "first" });
}

function rtSnapshotStep(args, label) {
  return step("snapshot", "Snapshot the OBS settings, the OBS folder and the swapped APPDATA / LOCALAPPDATA (" + label + ")", {
    label,
    roots: rtSnapshotRoots(args),
    gate: true,
  });
}

function rtCompareStep(mode, label, base, other, extra) {
  const text = {
    calibration: "Calibration: the difference between two starts without cordmux is the wobble; a user source in it stops the run",
    installed: "After installing, only the product rows of the removal table, the placed files and the wobble differ",
    uninstalled: "After uninstalling and removing the placed files, only the wobble and the not-removed rows differ",
  }[mode];
  return step("snapshot-compare", text, { mode, label, base, other, ...(mode === "calibration" ? {} : { wobble: "calibration" }), gate: true, ...(extra || {}) });
}

// 較正の起動では 2 つ目のコレクションも 1 度開いて戻す。OBS はコレクションを開いたときに
// その JSON へ既定のキー (canvases / modules / current_transition など) を書き足すので、較正で
// 開かないと、インストール後に [cordmux のシーンを作成] のために開いた瞬間の書き足しが「揺れ」に入らず、
// インストール後とアンインストール後の比較が Chat.json の 41 キーで失敗する (リハーサルでの実測)。
function rtCalibrationVisitSteps() {
  const switchTo = (name, why) =>
    step("obsws-request", "Switch the scene collection to " + name + " (calibration: " + why + ")", {
      request: "SetCurrentSceneCollection",
      requestData: { sceneCollectionName: name },
      gate: true,
    });
  return [
    switchTo(ROUNDTRIP_COLLECTION_SECOND, "let OBS write its defaults into the second collection"),
    step("wait", "Let OBS settle on the second collection", { ms: ROUNDTRIP_COLLECTION_SWITCH_SETTLE_MS }),
    switchTo(ROUNDTRIP_COLLECTION_MAIN, "back to the main collection"),
    step("wait", "Let OBS settle on the main collection", { ms: ROUNDTRIP_COLLECTION_SWITCH_SETTLE_MS }),
  ];
}

function rtCalibrationSteps(args) {
  return [
    ...rtLaunchSteps(args, "Start OBS without cordmux (calibration 1/2)", {}),
    ...rtCalibrationVisitSteps(),
    rtCloseStep("Close OBS (calibration 1/2)"),
    rtSnapshotStep(args, "calib-1"),
    ...rtLaunchSteps(args, "Start OBS without cordmux (calibration 2/2)", {}),
    ...rtCalibrationVisitSteps(),
    rtCloseStep("Close OBS (calibration 2/2)"),
    rtSnapshotStep(args, "calib-2"),
    // 較正の差に利用者のソースが入ったら止める (揺れで利用者の変化を隠さない)。
    rtCompareStep("calibration", "calibration", "calib-1", "calib-2", { stopOnFail: true }),
  ];
}

// which: "new" (--zip) か "old" (--old-zip)。opts.tolerant: コピーできなかったファイルを記録して先へ進む
// (update-while-running の反例)。
function rtPlaceSteps(args, which, opts) {
  const p = roundtripPaths(args.tag);
  const o = opts || {};
  const old = which === "old";
  const dirRel = old ? p.oldZipDirRel : p.zipDirRel;
  return [
    step("rt-extract", "Extract the " + (old ? "previous " : "") + "cordmux zip into " + dirRel, { zip: old ? args.oldZip : args.zip, destRel: dirRel, what: old ? "cordmux-old" : "cordmux", gate: true }),
    step("rt-place", "Place the cordmux folder of the " + (old ? "previous " : "") + "zip (" + (args.layout || DEFAULT_ROUNDTRIP_LAYOUT) + ")" + (o.tolerant ? " while OBS is running" : ""), {
      layout: args.layout || DEFAULT_ROUNDTRIP_LAYOUT,
      fromRel: dirRel,
      label: o.label || (old ? "old" : "new"),
      ...(o.tolerant ? { tolerant: true } : { gate: true }),
    }),
  ];
}

// dialog: "absent" (vencordDir がない = 切り替えの確認ダイアログは出ない) / "confirm" / "cancel"。
function rtConnectSteps(dialog) {
  // [Vesktop に追加] は 1 つのボタンである。state.json が
  // 別の Vencord を指している場合 (confirm / cancel) は、押すと切り替えの確認ダイアログが出る。
  // ダイアログが出ると Invoke はモーダルの間戻らないので、押すのは dialog-click.ps1 の noWait (戻らなくても押せたことは ok)。
  const steps = [
    step("wait", "Let the dock draw its notices before pressing", { ms: ROUNDTRIP_DOCK_SETTLE_MS }),
    uiClickStep("Press [" + TEXT_ID_ADD_TO_VESKTOP + "] in the dock", DOCK_WINDOW, TEXT_ID_ADD_TO_VESKTOP, { noWait: true, label: "add-to-vesktop", gate: true }),
  ];
  if (dialog === "absent") {
    steps.push(
      step("dialog-click", "The switch dialog does not appear when state.json has no vencordDir", {
        target: "first",
        titleTextId: TEXT_ID_DIALOG_VESKTOP_ADD_TITLE,
        buttonTextId: null,
        expect: "absent",
        listOnly: true,
        timeoutMs: ROUNDTRIP_DIALOG_ABSENT_TIMEOUT_MS,
        label: "switch-dialog-absent",
        gate: true,
      }),
    );
  } else {
    const button = dialog === "confirm" ? TEXT_ID_DIALOG_SWITCH_BUTTON : OBS_TEXT_CANCEL;
    steps.push(uiClickStep("The switch dialog names the self-built Vencord; press [" + button + "]", { titleTextId: TEXT_ID_DIALOG_VESKTOP_ADD_TITLE }, button, {
      timeoutMs: ROUNDTRIP_DIALOG_TIMEOUT_MS,
      label: "switch-dialog-" + dialog,
      gate: true,
    }));
  }
  steps.push(step("wait", "Let the product finish writing state.json", { ms: ROUNDTRIP_WRITE_SETTLE_MS }));
  return steps;
}
// 導入のあとの利用者のシーンに、合成のソース cordmux を 1 つ置く (OBS の [+] から足す操作の代わり)。
// 1 つ目のコレクションの Opening シーンへ置き、both なら 2 つ目のコレクション (Chat) の Chat シーンにも置く。
// 置くのは wire-composite と同じ API (CreateInput)。撤去がシーンの項目ごと消すことを、導入の前後の比較で確認する。
export const ROUNDTRIP_SCENE_MAIN = "Opening";
export const ROUNDTRIP_SCENE_SECOND = "Chat";
function rtWireCompositeSteps(both) {
  const wire = (sceneName) => ({ ...wireCompositeStep("collection scene " + sceneName), sceneName, gate: true });
  const switchTo = (name) =>
    step("obsws-request", "Switch the scene collection to " + name, {
      request: "SetCurrentSceneCollection",
      requestData: { sceneCollectionName: name },
      gate: true,
    });
  const steps = [wire(ROUNDTRIP_SCENE_MAIN), step("wait", "Let the composite source settle", { ms: ROUNDTRIP_COLLECTION_SWITCH_SETTLE_MS })];
  if (both) {
    steps.push(
      switchTo(ROUNDTRIP_COLLECTION_SECOND),
      step("wait", "Let the dock redraw for the second collection", { ms: ROUNDTRIP_COLLECTION_SWITCH_SETTLE_MS }),
      wire(ROUNDTRIP_SCENE_SECOND),
      step("wait", "Let the composite source settle", { ms: ROUNDTRIP_COLLECTION_SWITCH_SETTLE_MS }),
      switchTo(ROUNDTRIP_COLLECTION_MAIN),
      step("wait", "Let the dock redraw after switching back", { ms: ROUNDTRIP_COLLECTION_SWITCH_SETTLE_MS }),
    );
  }
  return steps;
}

function rtUninstallSteps(args) {
  return [
    ...rtLaunchSteps(args, "Start OBS with cordmux to uninstall", { cordmux: true }),
    step("wait", "Let the dock settle", { ms: ROUNDTRIP_DOCK_SETTLE_MS }),
    // OBS の [ツール] -> 「cordmuxをアンインストール...」-> 確認 -> 結果の順に操作する (手順書と同じ操作)。
    // 結果のダイアログの文は label uninstall-done で judge-rt へ渡る。ページのボタンから実行する操作は、リハーサルが通す。
    ...uninstallSteps("roundtrip", ROUNDTRIP_UNINSTALL_DONE_TIMEOUT_MS, { gateResult: true, entry: "tools" }),
    step("screenshot-window", "Screenshot OBS after uninstalling", { target: "first", shotLabel: "after-uninstall" }),
    step("collect-plugin-log", "Collect the plugin log lines"),
    rtCloseStep("Close OBS (after uninstalling)"),
    // 置いたものを消す (利用者が手で消す操作の代わり)。OBS を閉じてから消す。
    step("rt-remove-placed", "Delete what the runner placed (stands in for the user deleting the folder)", { gate: true }),
  ];
}

function rtCaptureStep(from, label) {
  return step("capture-file", "Read " + from + " (" + label + ")", { from, label });
}

function rtJudgeStep(judge, label, inputs, extra) {
  return step("judge-rt", label, { judge, inputs, gate: true, ...(extra || {}) });
}

// ドックのウィンドウを押さずに読む。buttonTextId を渡すとそのボタンが出ていることを求める (expect: "button")。
// null なら文とボタンの一覧を残すだけ (expect: "present")。
function rtDockListStep(buttonTextId, label, gate) {
  return step("dialog-click", buttonTextId ? "Read the dock window: is [" + buttonTextId + "] offered, and what does it say (no press)" : "Read what the dock window says (no press)", {
    target: "first",
    ...DOCK_WINDOW,
    buttonTextId: buttonTextId || null,
    expect: buttonTextId ? "button" : "present",
    listOnly: true,
    timeoutMs: ROUNDTRIP_DIALOG_TIMEOUT_MS,
    label,
    ...(gate ? { gate: true } : {}),
  });
}

function rtEvidenceSteps(label) {
  return [
    step("screenshot-window", "Screenshot OBS and the dock (" + label + ")", { target: "first", shotLabel: label }),
    step("collect-plugin-log", "Collect the plugin log lines"),
  ];
}

// ---- 場面 ----

function rtScenarioRoundtrip(args, custom) {
  const steps = [...rtPrepareSteps(args), rtSeedStep(args, { custom }), ...rtCalibrationSteps(args), ...rtPlaceSteps(args, "new")];
  steps.push(...rtLaunchSteps(args, "Start OBS with cordmux (first start after placing the zip)", { cordmux: true }));
  steps.push(...rtConnectSteps(custom ? "confirm" : "absent"));
  steps.push(...rtWireCompositeSteps(true));
  steps.push(...rtEvidenceSteps("installed"));
  steps.push(rtCloseStep("Close OBS (installed)"));
  steps.push(rtSnapshotStep(args, "installed"));
  steps.push(rtCompareStep("installed", "ac04", "calib-1", "installed"));
  steps.push(...rtUninstallSteps(args));
  steps.push(...rtLaunchSteps(args, "Start OBS without cordmux (after uninstall)", {}));
  steps.push(rtCloseStep("Close OBS (after uninstall)"));
  steps.push(rtSnapshotStep(args, "uninstalled"));
  // 導入した後のコピーも渡す (利用者のシーンに置いた cordmux の項目で増え、撤去しても OBS が戻さない項目の番号の数えを見分けるため)。
  steps.push(rtCompareStep("uninstalled", "ac69", "calib-1", "uninstalled", { installed: "installed" }));
  if (custom) {
    // アンインストールで自作の場所へ戻ることを、結果 JSON だけで読めるように分けて確認する (直前の比較の差分でも見える)。
    steps.push(rtCaptureStep("state_json", "state-after-uninstall"));
    steps.push(rtJudgeStep("vencord-dir-equals", "vencordDir is back at the self-built Vencord", { state: { capture: "state-after-uninstall" }, expectedDir: { dir: "custom_vencord" } }));
  }
  return steps;
}

function rtScenarioOtherVencordCancel(args) {
  return [
    ...rtPrepareSteps(args),
    rtSeedStep(args, { custom: true }),
    ...rtPlaceSteps(args, "new"),
    ...rtLaunchSteps(args, "Start OBS with cordmux", { cordmux: true }),
    rtCaptureStep("state_json", "state-before-cancel"),
    ...rtConnectSteps("cancel"),
    rtCaptureStep("state_json", "state-after-cancel"),
    rtCaptureStep("backup", "backup-after-cancel"),
    rtJudgeStep("cancel-kept", "Counterexample: after [cancel] state.json is unchanged and no backup is written", {
      stateBefore: { capture: "state-before-cancel" },
      stateAfter: { capture: "state-after-cancel" },
      backupAfter: { capture: "backup-after-cancel" },
    }),
    ...rtEvidenceSteps("after-cancel"),
    rtCloseStep("Close OBS"),
  ];
}

// 導入して [Vesktop に追加] を押し、OBS を閉じるまで (move / force-update / motw 等の共通の前半)。
function rtInstallAndConnectSteps(args, seedOpts) {
  return [
    ...rtPrepareSteps(args),
    rtSeedStep(args, seedOpts),
    ...rtPlaceSteps(args, "new"),
    ...rtLaunchSteps(args, "Start OBS with cordmux", { cordmux: true }),
    ...rtConnectSteps("absent"),
    rtCloseStep("Close OBS"),
  ];
}

function rtScenarioMove(args, foreign) {
  const p = roundtripPaths(args.tag);
  const steps = [...rtInstallAndConnectSteps(args, {})];
  if (foreign) {
    // 反例: 利用者が自分の値に書き換えた (自作の Vencord に戻した) 後で移す。
    steps.push(step("edit-state-json", "Point vencordDir at a self-built Vencord (the user changed it back)", { vencordDir: "custom_vencord" }));
  }
  steps.push(rtCaptureStep("state_json", "state-before-move"), rtCaptureStep("backup", "backup-before-move"));
  steps.push(step("rt-rename-obs", "Rename the whole OBS folder " + p.obsRel + " -> " + p.obsMovedRel + " (the user moved it)", { fromRel: p.obsRel, toRel: p.obsMovedRel, gate: true }));
  steps.push(...rtLaunchSteps(args, "Start the moved OBS without pressing anything", { cordmux: true, obsRel: p.obsMovedRel, settleMs: ROUNDTRIP_STARTUP_CHECK_WAIT_MS }));
  if (foreign) steps.push(rtDockListStep(TEXT_ID_ADD_TO_VESKTOP, "dock-after-move", true));
  steps.push(...rtEvidenceSteps("after-move"));
  steps.push(rtCloseStep("Close the moved OBS"));
  steps.push(rtCaptureStep("state_json", "state-after-move"), rtCaptureStep("backup", "backup-after-move"));
  if (foreign) {
    steps.push(rtJudgeStep("state-kept", "Counterexample: a value the user chose is not rewritten at startup ([fix] is offered instead)", {
      stateBefore: { capture: "state-before-move" },
      stateAfter: { capture: "state-after-move" },
    }));
  } else {
    steps.push(rtJudgeStep("startup-rewrite", "The startup rewrite points vencordDir at the moved data/vencord, keeps the other keys and the backup's previous", {
      stateBefore: { capture: "state-before-move" },
      stateAfter: { capture: "state-after-move" },
      backupBefore: { capture: "backup-before-move" },
      backupAfter: { capture: "backup-after-move" },
      expectedDir: { dir: "bundle_shown" },
    }));
  }
  return steps;
}

// 上書きのメッセージはファイルの名前を画面に出さない (rules_dock.cpp の VencordOverwritten。名前はログに出す)。
// そのため、メッセージの見出しと [再インストール] が出ることを判定する。
// names は、どのファイルを壊したかを結果に残すために渡す。
// opts.statusTab: 先に状態タブを選ぶ。接続している間の上書き (InstallMismatch) はメッセージのレベル 2 で、[再インストール] は
// 状態タブのカードにある (1 行目は見出しだけ)。接続していない間の欠け (force-update-missing) は参加者タブのカードに出る。
function rtReinstallSteps(label, names, opts) {
  const statusTab = opts && opts.statusTab
    ? [
        uiClickStep("Select the dock [" + TEXT_ID_TAB_STATUS + "] tab (the card with [" + TEXT_ID_REINSTALL + "])", DOCK_WINDOW, TEXT_ID_TAB_STATUS, { controlType: "TabItem", namePrefix: true }),
        step("wait", "Let the status tab draw its cards", { ms: UI_APPLY_SETTLE_MS }),
      ]
    : [];
  return [
    ...statusTab,
    rtDockListStep(TEXT_ID_REINSTALL, "dock-" + label, true),
    rtJudgeStep("named-in-texts", "The dock shows the overwritten notice (files changed: " + (names || []).join(", ") + ")", { texts: { dialog: "dock-" + label }, names: { textPrefix: TEXT_ID_VENCORD_OVERWRITTEN } }),
    ...rtEvidenceSteps(label + "-named"),
    step("dock-click", "Press [" + TEXT_ID_REINSTALL + "]", { target: "first", textId: TEXT_ID_REINSTALL, index: 0, gate: true }),
    step("wait", "Let the product copy the files back and check again", { ms: ROUNDTRIP_REINSTALL_SETTLE_MS }),
    step("hash-bundle", "Hash data/vencord-src and data/vencord", { label: label + "-after-reinstall" }),
    rtJudgeStep("bundle-restored", "After [reinstall] every file of data/vencord-src is in data/vencord with the same hash", { hashes: { hashes: label + "-after-reinstall" } }),
    ...rtEvidenceSteps(label + "-reinstalled"),
  ];
}

function rtScenarioForceUpdateMissing(args) {
  return [
    ...rtInstallAndConnectSteps(args, {}),
    step("delete-bundle-files", "Delete the 4 bundle files from data/vencord (Force Update left them missing = --dock-install missing)", { names: VENCORD_BUNDLE_FILE_NAMES, gate: true }),
    rtCaptureStep("backup", "backup-before-reinstall"),
    ...rtLaunchSteps(args, "Start OBS (the startup check runs)", { cordmux: true, settleMs: ROUNDTRIP_STARTUP_CHECK_WAIT_MS }),
    ...rtReinstallSteps("missing", VENCORD_BUNDLE_FILE_NAMES),
    rtCaptureStep("backup", "backup-after-reinstall"),
    rtJudgeStep("written-at-advanced", "[reinstall] advances only written_at of the backup", { backupBefore: { capture: "backup-before-reinstall" }, backupAfter: { capture: "backup-after-reinstall" } }),
    rtCloseStep("Close OBS"),
  ];
}

function rtScenarioForceUpdateTamper(args) {
  return [
    ...rtInstallAndConnectSteps(args, { periods: true }),
    ...rtLaunchSteps(args, "Start OBS (short check periods)", { cordmux: true }),
    // 利用者の普段の状態 (cordmux のソースを置いた後) で上書きを起こす。ソースがないと、同じレベル 2 で並びが前の
    // 「cordmuxのソースがまだ追加されていません」が 1 行目に出て、上書きのメッセージと [再インストール] は状態タブだけに出る。
    ...rtWireCompositeSteps(false),
    step("harness-start", "Start the test sender (1 screen) so that the connected check period applies (uses the GPU)", {
      mode: "product",
      n: 1,
      seconds: ROUNDTRIP_TAMPER_HARNESS_SECONDS,
      hw: args.hw,
      warmup: args.warmup,
      syncTone: DEFAULT_SYNC_TONE,
    }),
    step("wait-connection", "Wait until the test sender connects", { timeoutMs: ROUNDTRIP_CONNECT_WAIT_TIMEOUT_MS, gate: true }),
    step("tamper-file", "Append 1 byte to data/vencord/" + ROUNDTRIP_TAMPER_FILE + " while OBS is running", { name: ROUNDTRIP_TAMPER_FILE, gate: true }),
    step("wait", "Wait for a few check periods", { ms: ROUNDTRIP_TAMPER_WAIT_MS }),
    ...rtReinstallSteps("tamper", [ROUNDTRIP_TAMPER_FILE], { statusTab: true }),
    step("harness-wait", "Wait for the test sender to finish", { seconds: ROUNDTRIP_TAMPER_HARNESS_SECONDS }),
    rtCloseStep("Close OBS"),
  ];
}

function rtScenarioUpdate(args, whileRunning) {
  const steps = [...rtPrepareSteps(args), rtSeedStep(args, {}), ...rtPlaceSteps(args, "old")];
  steps.push(...rtLaunchSteps(args, "Start OBS with the previous cordmux", { cordmux: true }));
  steps.push(...rtConnectSteps("absent"));
  if (whileRunning) {
    // 反例: 起動したまま上書きすると cordmux.dll が置き換わらない。手順書が「OBS を閉じる」と書く根拠になる。
    steps.push(...rtPlaceSteps(args, "new", { tolerant: true, label: "overwrite-while-running" }));
    steps.push(step("hash-files", "Hash the placed cordmux.dll and the one in the new zip", { label: "dll-after-overwrite", files: ["placed_dll", "zip_dll"] }));
    steps.push(rtJudgeStep("overwrite-refused", "Counterexample: while OBS runs, the placed cordmux.dll is not replaced", {
      placeReport: { placeReport: "overwrite-while-running" },
      hashes: { hashes: "dll-after-overwrite" },
    }));
    steps.push(rtCloseStep("Close OBS"));
    steps.push(...rtLaunchSteps(args, "Start OBS again (the mixed bundle is visible here)", { cordmux: true, settleMs: ROUNDTRIP_STARTUP_CHECK_WAIT_MS }));
    steps.push(rtDockListStep(null, "dock-mixed", false));
    steps.push(...rtEvidenceSteps("mixed"));
    steps.push(rtCloseStep("Close OBS"));
    return steps;
  }
  steps.push(...rtWireCompositeSteps(false));
  // 利用者が変えた設定の代わりに、ドックの [表示] タブの「名前」の [表示] を外す。押す前のドックを PNG に残す。
  // --old-zip は、ドックに [表示] タブがあるバージョンの zip であることを前提とする。
  steps.push(step("screenshot-window", "Take the dock before changing a setting", { target: "first", shotLabel: "before-toggle" }));
  steps.push(...displayTabSteps("change a setting the user changed", { gate: true }));
  steps.push(
    uiClickStep("Turn the name [" + TEXT_ID_LOOK_SHOW + "] off (a setting the user changed)", DOCK_WINDOW, TEXT_ID_LOOK_SHOW, {
      controlType: "CheckBox",
      group: TEXT_ID_LOOK_HEADING_NAME,
      desired: "off",
      gate: true,
    }),
  );
  steps.push(step("wait", "Let the setting be saved", { ms: ROUNDTRIP_WRITE_SETTLE_MS }));
  steps.push(rtCloseStep("Close OBS before updating (the manual says so)"));
  steps.push(
    rtCaptureStep("cordmux_settings", "settings-before-update"),
    rtCaptureStep("backup", "backup-before-update"),
    rtCaptureStep("state_json", "state-before-update"),
    rtCaptureStep("mark_src", "mark-before-update"),
  );
  steps.push(...rtPlaceSteps(args, "new"));
  steps.push(rtCaptureStep("zip_mark", "mark-in-new-zip"));
  steps.push(...rtLaunchSteps(args, "Start OBS with the updated cordmux", { cordmux: true, settleMs: ROUNDTRIP_STARTUP_CHECK_WAIT_MS }));
  steps.push(...rtEvidenceSteps("updated"));
  steps.push(rtCloseStep("Close OBS"));
  steps.push(
    rtCaptureStep("cordmux_settings", "settings-after-update"),
    rtCaptureStep("backup", "backup-after-update"),
    rtCaptureStep("state_json", "state-after-update"),
    rtCaptureStep("mark_src", "mark-after-update"),
  );
  steps.push(rtJudgeStep("update-kept", "The settings, the backup and state.json are the same and the bundle mark is the new one", {
    settingsBefore: { capture: "settings-before-update" },
    settingsAfter: { capture: "settings-after-update" },
    backupBefore: { capture: "backup-before-update" },
    backupAfter: { capture: "backup-after-update" },
    stateBefore: { capture: "state-before-update" },
    stateAfter: { capture: "state-after-update" },
    markBefore: { capture: "mark-before-update" },
    markAfter: { capture: "mark-after-update" },
    markInZip: { capture: "mark-in-new-zip" },
  }));
  return steps;
}

function rtScenarioMotw(args) {
  return [
    ...rtPrepareSteps(args),
    rtSeedStep(args, {}),
    ...rtPlaceSteps(args, "new"),
    step("mark-motw", "Give every placed file a Zone.Identifier (ZoneId=3), as Explorer does for files from the internet", { zoneId: 3, gate: true }),
    ...rtLaunchSteps(args, "Start OBS with the marked cordmux", { cordmux: true }),
    step("read-obs-log", "cordmux.dll loaded the bundled FFmpeg (the startup line of src-obs/plugin-main.cpp)", { pattern: "ffmpeg-versions", gate: true }),
    step("run-placed-ffmpeg", "Run the placed (marked) ffmpeg.exe -version", { gate: true }),
    ...rtEvidenceSteps("motw"),
    rtCloseStep("Close OBS"),
  ];
}

function rtScenarioDllClash(args) {
  return [
    ...rtPrepareSteps(args),
    rtSeedStep(args, {}),
    ...rtPlaceSteps(args, "new"),
    ...rtLaunchSteps(args, "Start OBS with cordmux", { cordmux: true }),
    step("read-obs-log", "The FFmpeg versions cordmux.dll sees (src-obs/plugin-main.cpp startup line)", { pattern: "ffmpeg-versions" }),
    step("list-modules", "List the FFmpeg DLLs (av* / sw*) loaded into obs64.exe and where each came from ", { target: "first", gate: true }),
    step("collect-plugin-log", "Collect the plugin log lines"),
    rtCloseStep("Close OBS"),
  ];
}

function rtScenarioOldObs(args) {
  return [
    ...rtPrepareSteps(args),
    rtSeedStep(args, {}),
    ...rtPlaceSteps(args, "new"),
    // 32.x (--obs-zip に 32.2.2 の zip を渡す) に、OBS 33 の置き方 (plugins\cordmux) の cordmux を置いたときに何が出るかを取る。
    // OBS 32 以前は旧方式の場所 (bin\64bit) しか読まないので、cordmux は読み込まれないはずである (OBS のログに cordmux の
    // Loading module がない)。読み込まれたことは取らず、読み込まれなかったことをゲートにする。obs-websocket に接続できなくても止めない。
    ...rtLaunchSteps(args, "Start the older OBS (32.x) with cordmux placed in the OBS 33 layout", { cordmux: true, gateLoaded: false, tolerant: true }),
    step("read-obs-log", "cordmux is not loaded: no Loading module line of cordmux in the OBS log", { pattern: "module-not-loaded", moduleName: "cordmux", gate: true }),
    step("read-obs-log", "Lines about cordmux in the OBS log (load failures, missing functions)", { pattern: "cordmux-lines" }),
    // 手順書の「ドックが表示されない」に、OBS を 33 以上へ更新する案内があること (表示されない利用者がそこを見るため)。
    step("check-guide-update-hint", "The guide's \"The dock doesn't appear\" says to update OBS to 33 or later", { hints: GUIDE_UPDATE_HINTS, gate: true }),
    ...rtEvidenceSteps("old-obs"),
    rtCloseStep("Close OBS"),
  ];
}

function rtScenarioNewerLibobs(args) {
  return [
    ...rtPrepareSteps(args),
    rtSeedStep(args, {}),
    ...rtPlaceSteps(args, "new"),
    step("rt-replace-dll", "Replace the placed cordmux.dll with the one whose obs_module_ver() is newer (newer_ver_dll.py)", { testDll: args.testDll, gate: true }),
    ...rtLaunchSteps(args, "Start OBS 33.x with the newer-version DLL", { cordmux: true, gateLoaded: false, tolerant: true }),
    step("read-obs-log", "Lines about cordmux in the OBS log", { pattern: "cordmux-lines" }),
    step("screenshot-window", "Screenshot OBS right after start", { target: "first", shotLabel: "newer-libobs-start" }),
    // プラグインの管理画面を開く操作だけは、実行する人が手で行う。
    step("wait", "Hold while the operator opens Tools > Plugin Manager of OBS 33.x", { ms: ROUNDTRIP_PLUGIN_MANAGER_HOLD_MS }),
    step("screenshot-window", "Screenshot every OBS window (the plugin manager should be open)", { target: "first", shotLabel: "newer-libobs-manager" }),
    step("collect-plugin-log", "Collect the plugin log lines"),
    rtCloseStep("Close OBS"),
  ];
}

// 終わりの 3 つ (write-result -> summarize -> cleanup) は他の step と同じ形のまま使い、cleanup にだけ
// 作業場所を消す指示を足す (撮ったものと結果は --out に残っている)。その前に OBS のログを --out へコピーする。
function rtFinishSteps(args) {
  const p = roundtripPaths(args.tag);
  const common = finishSteps(args, INSTALL_ROUNDTRIP_STEP);
  const last = common[common.length - 1];
  const cleanup = Object.assign({}, last, {
    label: last.label + "; then delete " + p.workRel + (args.keepWork ? " (kept: --keep-work)" : ""),
    rtWorkRel: p.workRel,
    keepWork: Boolean(args.keepWork),
    // ProgramData への書き込みの漏れの監視は、VM の programdata の場合にだけ入れる。
    watchProgramData: args.layout === "programdata",
  });
  return [
    step("copy-obs-logs", "Copy the OBS logs of this run into the output folder (before the work folder goes away)", {}),
    ...common.slice(0, -1),
    cleanup,
  ];
}

function buildInstallRoundtripSteps(args) {
  const builders = {
    roundtrip: (a) => rtScenarioRoundtrip(a, false),
    move: (a) => rtScenarioMove(a, false),
    "move-foreign": (a) => rtScenarioMove(a, true),
    "force-update-missing": rtScenarioForceUpdateMissing,
    "force-update-tamper": rtScenarioForceUpdateTamper,
    update: (a) => rtScenarioUpdate(a, false),
    "update-while-running": (a) => rtScenarioUpdate(a, true),
    "other-vencord": (a) => rtScenarioRoundtrip(a, true),
    "other-vencord-cancel": rtScenarioOtherVencordCancel,
    motw: rtScenarioMotw,
    "dll-clash": rtScenarioDllClash,
    "old-obs": rtScenarioOldObs,
    "newer-libobs": rtScenarioNewerLibobs,
  };
  const scenario = ROUNDTRIP_SCENARIO_VALUES.includes(args.scenario) ? args.scenario : DEFAULT_ROUNDTRIP_SCENARIO;
  return [...builders[scenario](args), ...rtFinishSteps(args)];
}

// ---- 往復の判定 (純関数。run-window.mjs の judge-rt が呼ぶ) -----------------------------

function parseObjectOrNull(text) {
  if (text === null || text === undefined) return null;
  const parsed = parseJsonOrNull(text);
  if (!parsed.ok || parsed.value === null || typeof parsed.value !== "object" || Array.isArray(parsed.value)) return null;
  return parsed.value;
}

// ビルドのマーカーのファイル (src-obs/install/build_mark.h の形: 1 行に 1 項目の「キー: 値」を緩く読む)。
export function parseBuildMarkText(text) {
  if (typeof text !== "string") return null;
  const out = {};
  const seen = new Set();
  for (const raw of text.replace(/^\uFEFF/, "").split("\n")) {
    const line = raw.replace(/\r$/, "");
    const colon = line.indexOf(":");
    if (colon < 0) continue;
    const key = line.slice(0, colon).trim();
    const value = line.slice(colon + 1).trim();
    if (key !== "build_id" && key !== "vencord_commit") continue;
    if (seen.has(key)) return null; // 同じキーが 2 回ある場合は読めないものとして扱う (build_mark.h と同じ)
    seen.add(key);
    out[key] = value;
  }
  if (!out.build_id || !out.vencord_commit) return null;
  return out;
}

function otherKeysChanged(before, after, ignored) {
  const keys = new Set([...Object.keys(before), ...Object.keys(after)]);
  for (const k of ignored || []) keys.delete(k);
  return [...keys].filter((k) => !sameJsonValue(before[k], after[k]));
}

function result(ok, reason, extra) {
  return Object.assign({ ok, reason: ok ? null : reason }, extra || {});
}

// 起動時の書き直しを判定する。vencordDir が移した先の data/vencord を指し、ほかのキーと
// バックアップの previous は変わらず、バックアップの written は移した先に進み、written_at と dll_dir も進む
// (フォルダーごと名前を変えるので dll_dir は前と違う場所になる)。バックアップのほかのキーは変わらない。
export function judgeStartupRewrite({ stateBefore, stateAfter, backupBefore, backupAfter, expectedDir }) {
  const sb = parseObjectOrNull(stateBefore);
  const sa = parseObjectOrNull(stateAfter);
  const bb = parseObjectOrNull(backupBefore);
  const ba = parseObjectOrNull(backupAfter);
  if (!sb || !sa) return result(false, "state.json before/after is not readable");
  if (!bb || !ba) return result(false, "the backup before/after is not readable");
  const pointsToMoved = typeof sa.vencordDir === "string" && normalizePathForCompare(sa.vencordDir) === normalizePathForCompare(expectedDir);
  const changedKeys = otherKeysChanged(sb, sa, ["vencordDir"]);
  const previousKept = sameJsonValue(bb.previous, ba.previous);
  const writtenMoved = typeof ba.written === "string" && normalizePathForCompare(ba.written) === normalizePathForCompare(expectedDir);
  const writtenAtAdvanced = Number.isFinite(Number(ba.written_at)) && Number(ba.written_at) > Number(bb.written_at);
  const dllDirMoved = typeof ba.dll_dir === "string" && ba.dll_dir !== "" &&
    normalizePathForCompare(ba.dll_dir) !== normalizePathForCompare(bb.dll_dir);
  const backupOtherKeys = otherKeysChanged(bb, ba, ["previous", "written", "written_at", "dll_dir"]);
  const checks = {
    pointsToMoved, otherKeysSame: changedKeys.length === 0, previousKept, writtenMoved,
    writtenAtAdvanced, dllDirMoved, backupOtherKeysSame: backupOtherKeys.length === 0,
  };
  const ok = Object.values(checks).every(Boolean);
  const reason = !pointsToMoved
    ? "vencordDir does not point at the moved data/vencord (now " + String(sa.vencordDir) + ")"
    : changedKeys.length > 0
      ? "other keys of state.json changed: " + changedKeys.join(", ")
      : !previousKept
        ? "the backup's previous changed"
        : !writtenMoved
          ? "the backup's written was not moved to the new place"
          : !writtenAtAdvanced
            ? "the backup's written_at did not advance"
            : !dllDirMoved
              ? "the backup's dll_dir did not move to the new place"
              : "other keys of the backup changed: " + backupOtherKeys.join(", ");
  return result(ok, reason, { checks, changedKeys, backupOtherKeys, vencordDir: sa.vencordDir ?? null });
}

// state.json のキーが 1 つも変わっていない (利用者が決めた値を起動時に書き直さない、という反例など)。
export function judgeStateKept({ stateBefore, stateAfter }) {
  const sb = parseObjectOrNull(stateBefore);
  const sa = parseObjectOrNull(stateAfter);
  if (!sb || !sa) return result(false, "state.json before/after is not readable");
  const changedKeys = otherKeysChanged(sb, sa, []);
  return result(changedKeys.length === 0, "state.json changed: " + changedKeys.join(", "), { changedKeys });
}

// 反例: [やめる] の後は state.json が変わらず、バックアップも書かれない。
export function judgeCancelKept({ stateBefore, stateAfter, backupAfter }) {
  const kept = judgeStateKept({ stateBefore, stateAfter });
  const backupWritten = backupAfter !== null && backupAfter !== undefined;
  const ok = kept.ok && !backupWritten;
  return result(ok, !kept.ok ? kept.reason : "a backup was written although the dialog was cancelled", { changedKeys: kept.changedKeys, backupWritten });
}

export function judgeVencordDirEquals({ state, expectedDir }) {
  const s = parseObjectOrNull(state);
  if (!s) return result(false, "state.json is not readable");
  const ok = typeof s.vencordDir === "string" && normalizePathForCompare(s.vencordDir) === normalizePathForCompare(expectedDir);
  return result(ok, "vencordDir is " + String(s.vencordDir) + ", expected " + String(expectedDir), { vencordDir: s.vencordDir ?? null });
}

// 上書きで更新しても設定・バックアップ・state.json が同じで、元のファイルのマーカーが新しい zip のものになる。
export function judgeUpdateKept({ settingsBefore, settingsAfter, backupBefore, backupAfter, stateBefore, stateAfter, markBefore, markAfter, markInZip }) {
  const pairs = [
    ["settings", settingsBefore, settingsAfter],
    ["backup", backupBefore, backupAfter],
    ["state", stateBefore, stateAfter],
  ];
  const problems = [];
  for (const [name, before, after] of pairs) {
    const b = parseObjectOrNull(before);
    const a = parseObjectOrNull(after);
    if (!b || !a) problems.push(name + " before/after is not readable");
    else if (!sameJsonValue(b, a)) problems.push(name + " changed: " + otherKeysChanged(b, a, []).join(", "));
  }
  const mb = parseBuildMarkText(markBefore);
  const ma = parseBuildMarkText(markAfter);
  const mz = parseBuildMarkText(markInZip);
  if (!mb || !ma || !mz) problems.push("a bundle mark is not readable");
  else {
    if (ma.build_id === mb.build_id) problems.push("the bundle mark did not change (" + ma.build_id + ")");
    if (ma.build_id !== mz.build_id || ma.vencord_commit !== mz.vencord_commit) problems.push("the bundle mark is not the one in the new zip");
  }
  return result(problems.length === 0, problems.join("; "), { problems, markBefore: mb, markAfter: ma, markInZip: mz });
}

// [再インストール] の後: data/vencord-src の全ファイルが data/vencord に同じハッシュである。
export function judgeBundleRestored({ hashes }) {
  const src = (hashes && hashes.src) || {};
  const shown = (hashes && hashes.shown) || {};
  const names = Object.keys(src);
  const mismatched = names.filter((n) => shown[n] !== src[n]);
  const ok = names.length > 0 && mismatched.length === 0;
  return result(ok, names.length === 0 ? "data/vencord-src has no file" : "differs from data/vencord-src: " + mismatched.join(", "), { mismatched, fileCount: names.length });
}

// 読んだ文のどれかに、名前が全部出ている。anyName のときは、名前のどれか 1 つが出ていれば
// よい (同じメッセージの見出しが、人の名前が分かるかで 2 つのキーを使い分けるとき = noticeJudgeStep)。
export function judgeNamedInTexts({ texts, names, anyName }) {
  const list = Array.isArray(texts) ? texts.map(String) : [];
  const wanted = names || [];
  const missing = wanted.filter((n) => !list.some((t) => t.includes(n)));
  const named = anyName ? wanted.length > 0 && missing.length < wanted.length : missing.length === 0;
  return result(list.length > 0 && named, list.length === 0 ? "no text was read from the dock" : "not named: " + missing.join(", "), { missing });
}

// [再インストール] はバックアップの written_at だけを進める。
export function judgeWrittenAtAdvanced({ backupBefore, backupAfter }) {
  const b = parseObjectOrNull(backupBefore);
  const a = parseObjectOrNull(backupAfter);
  if (!b || !a) return result(false, "the backup before/after is not readable");
  const changed = otherKeysChanged(b, a, ["written_at"]);
  const advanced = typeof a.written_at === "number" && typeof b.written_at === "number" ? a.written_at > b.written_at : String(a.written_at) > String(b.written_at);
  return result(advanced && changed.length === 0, !advanced ? "written_at did not advance" : "other keys of the backup changed: " + changed.join(", "), { changedKeys: changed, advanced });
}

// 反例: 起動したままの上書きでは cordmux.dll が置き換わらない (コピーできなかった、または中身が古いまま)。
export function judgeOverwriteRefused({ placeReport, hashes }) {
  const files = (placeReport && placeReport.failed) || [];
  const dllFailed = files.some((f) => /(^|[\\/])cordmux\.dll$/i.test(String(f.dest || f.path || "")));
  const placed = hashes && hashes.placed_dll;
  const inZip = hashes && hashes.zip_dll;
  const notReplaced = typeof placed === "string" && typeof inZip === "string" && placed !== inZip;
  return result(dllFailed || notReplaced, "cordmux.dll was replaced while OBS was running", { dllCopyFailed: dllFailed, dllDiffersFromZip: notReplaced });
}

// 手順書 (Markdown) の項 (「### 見出し」の行から、次の # で始まる見出しの行の前まで) に、案内の文があるかを判定する。
// guides: [{ rel, text, title, needle }]。title は項の見出し (「### 」の後ろの文字列)、needle は案内の文 (空白と改行は比べない)。
export function judgeGuideUpdateHint(guides) {
  const squeeze = (t) => String(t).replace(/\s+/g, "");
  const items = (guides || []).map((g) => {
    const lines = String(g.text || "").split(/\r?\n/);
    const start = lines.findIndex((l) => l.startsWith("### ") && l.slice(4).trim() === g.title);
    if (start === -1) return { rel: g.rel, ok: false, reason: "no item titled " + g.title };
    let end = lines.length;
    for (let i = start + 1; i < lines.length; i += 1) {
      if (/^#{1,6} /.test(lines[i])) {
        end = i;
        break;
      }
    }
    const body = squeeze(lines.slice(start, end).join("\n"));
    const found = body.includes(squeeze(g.needle));
    return { rel: g.rel, ok: found, reason: found ? null : "the item does not contain: " + g.needle };
  });
  const bad = items.filter((i) => !i.ok);
  return { ok: items.length > 0 && bad.length === 0, reason: bad.map((i) => i.rel + ": " + i.reason).join("; ") || null, items };
}

// 読み込まれなかったことの判定 (old-obs): OBS のログに「Loaded Modules:」の節があり (OBS が最後まで起動した)、その中にも、
// 「Loading module: <名前>」の行 (--verbose のときだけ書かれる) にも、モジュールの名前がない。節がなければ hit にしない
// (ログが途中までしかなくて、何も出ていないだけのときに合格にしないため)。
export function judgeModuleNotLoaded(lines, moduleFileName) {
  const list = lines || [];
  const needle = String(moduleFileName || "").toLowerCase();
  const judged = judgeModuleLoaded(list, moduleFileName);
  const startIdx = list.findIndex((l) => /Loaded Modules:/i.test(l));
  const loadingLines = needle.length > 0 ? list.filter((l) => l.toLowerCase().includes("loading module") && l.toLowerCase().includes(needle)) : [];
  const hit = startIdx !== -1 && needle.length > 0 && !judged.hit && loadingLines.length === 0;
  return { hit, blockLines: judged.blockLines, loadingLines };
}

// 同名の DLL: 読み込まれた av* / sw* がどこから来たかを調べる。合否ではなくログに残すための判定で、空のときだけ失敗とする。
export function judgeFfmpegModules({ modules, obsDir }) {
  const root = normalizePathForCompare(obsDir);
  const list = (modules || []).map((m) => {
    const p = normalizePathForCompare(m);
    const where = p.startsWith(root + "/bin/64bit/") ? "obs-bin" : p.startsWith(root + "/plugins/cordmux/") ? "plugin-dir" : "other";
    return { path: m, where };
  });
  return result(list.length > 0, "no FFmpeg DLL was listed", { modules: list });
}

export function judgeRoundtrip(name, inputs) {
  const judges = {
    "startup-rewrite": judgeStartupRewrite,
    "state-kept": judgeStateKept,
    "cancel-kept": judgeCancelKept,
    "vencord-dir-equals": judgeVencordDirEquals,
    "update-kept": judgeUpdateKept,
    "bundle-restored": judgeBundleRestored,
    "named-in-texts": judgeNamedInTexts,
    "not-named-in-texts": judgeNotNamedInTexts,
    "written-at-advanced": judgeWrittenAtAdvanced,
    "overwrite-refused": judgeOverwriteRefused,
    "ffmpeg-modules": judgeFfmpegModules,
    "notice-iff-frames-stopped": judgeNoticeIffFramesStopped,
    "version-row-shown": judgeVersionRowShown,
  };
  const fn = judges[name];
  if (!fn) return result(false, "unknown judge: " + name);
  return fn(inputs || {});
}
// judge-rt が指定できる判定の一覧。
export const ROUNDTRIP_JUDGE_NAMES = ["startup-rewrite", "state-kept", "cancel-kept", "vencord-dir-equals", "update-kept", "bundle-restored", "named-in-texts", "not-named-in-texts", "written-at-advanced", "overwrite-refused", "ffmpeg-modules", "notice-iff-frames-stopped", "version-row-shown"];

// hold と call-hold の --stream-sink の 3 つの手順 (STREAM_SINK_URL のコメントも参照)。
//   起動: hold-info の前 (hold-info に flv の場所と配信を受ける ffmpeg の pid を書くため)。rearm で正常に終わるたびに開き直す。
//   配信の停止: 待ちの後、OBS を閉じる前 (call-hold は締めのアンインストールの前。配信中はアンインストールできない)。
//     テストを実行する人が配信を止め忘れても、OBS を配信中のまま閉じない。配信していなければ何もしない (失敗にしない)。
//   終了: OBS を閉じた後 (送信側が切った後なので、最後の flv は ffmpeg が自分で書き終えている。先に止めると flv の途中で切れる)。
// ---- hold の --self-mic: OBS のログから自分の枠の音声ソースの測定値を読む -----------------------------------------------------
// 製品が書く行 (src-obs/pipeline/pipeline.cpp の stats_loop と src-obs/sources/self_speaking_source.cpp):
//   DEBUG  [cordmux] Stats self_speaking_source packets=N self_peak_p50_db=.. self_peak_p95_db=.. self_peak_max_db=.. self_talking_pct=.. self_threshold_db=..
//          10 秒ごと。その 10 秒に受け取ったパケットの peak (dBFS、1 dB 刻み) の分布と、話していると判定したパケットの割合。
//          パケットを受け取らなかった周期は書かれない。
//   INFO   [cordmux] Self slot audio source found (match=uuid|name) / not found / lost
// 話し始めと話し終わりの 1 回ごとの行は製品にない: 話している・話していないの移り変わりは self_talking_pct の並びで読む。
// 製品が行のキーを足しても読めるように、キー=値の組を拾う (順番と、知らないキーに頼らない)。
const SELF_STATS_MARK = "[cordmux] Stats self_speaking_source ";
const SELF_SOURCE_RE = /\[cordmux\] Self slot audio source (found \(match=(\w+)\)|not found|lost)/;
export function selfSpeakingStatsFromLog(lines) {
  const rows = [];
  let dayOffset = 0;
  let last = null;
  for (const raw of lines || []) {
    const line = String(raw);
    const at = line.indexOf(SELF_STATS_MARK);
    if (at < 0) continue;
    const pairs = {};
    for (const m of line.slice(at + SELF_STATS_MARK.length).matchAll(/(\w+)=(-?[0-9.]+)/g)) pairs[m[1]] = Number(m[2]);
    let t = obsLogSeconds(line);
    if (t !== null) {
      if (last !== null && t + dayOffset < last - 12 * 3600) dayOffset += 24 * 3600;
      t += dayOffset;
      last = t;
    }
    const num = (v) => (typeof v === "number" ? v : null);
    rows.push({
      t,
      packets: num(pairs.packets),
      p50Db: num(pairs.self_peak_p50_db),
      p95Db: num(pairs.self_peak_p95_db),
      maxDb: num(pairs.self_peak_max_db),
      talkingPct: num(pairs.self_talking_pct),
      thresholdDb: num(pairs.self_threshold_db),
    });
  }
  return rows;
}

export function selfSourceEventsFromLog(lines) {
  const events = [];
  for (const raw of lines || []) {
    const line = String(raw);
    const m = SELF_SOURCE_RE.exec(line);
    if (!m) continue;
    events.push({ t: obsLogSeconds(line), event: m[1].startsWith("found") ? "found" : m[1] === "lost" ? "lost" : "not-found", match: m[2] || null });
  }
  return events;
}

// 1 行 (10 秒) の読み分け。しきい値を超えるパケットが多いなら話している (talking)、超えないが大きな音のパケットがあるなら大きいのに
// 話していない (loud-silent。ミュートして話している形)、それ以外は静か (quiet)。割合のしきい値 (半分) は読み分けのための目安で、
// 製品の判定のしきい値ではない。行の欠けは unknown。
export const SELF_ROW_TALKING_PCT = 50;
export function classifySelfRow(row) {
  if (!row || typeof row.talkingPct !== "number" || typeof row.p95Db !== "number" || typeof row.thresholdDb !== "number") return "unknown";
  if (row.talkingPct >= SELF_ROW_TALKING_PCT) return "talking";
  return row.p95Db > row.thresholdDb ? "loud-silent" : "quiet";
}

// 行の並びを読み分けの並び (連続する同じ読みをまとめたもの) にし、入力を見つけた行が出ているか・行があるかを確認する。
export function judgeSelfSpeakingLog({ rows, events }) {
  const list = rows || [];
  const found = (events || []).filter((e) => e.event === "found");
  const kinds = list.map(classifySelfRow);
  const sequence = [];
  for (const k of kinds) {
    const lastRun = sequence[sequence.length - 1];
    if (lastRun && lastRun.kind === k) lastRun.rows += 1;
    else sequence.push({ kind: k, rows: 1 });
  }
  const counts = { talking: 0, "loud-silent": 0, quiet: 0, unknown: 0 };
  for (const k of kinds) counts[k] += 1;
  const peaks = list.filter((r) => typeof r.maxDb === "number").map((r) => r.maxDb);
  const ok = found.length > 0 && list.length > 0;
  return result(ok, found.length === 0 ? "the product never reported finding the audio input" : "no self_speaking_source stats row in the OBS log", {
    sourceEvents: events || [],
    foundMatch: found.length > 0 ? found[found.length - 1].match : null,
    rowCount: list.length,
    counts,
    sequence,
    maxDb: peaks.length > 0 ? Math.max(...peaks) : null,
    thresholdDb: list.length > 0 ? list[list.length - 1].thresholdDb : null,
    rows: list,
  });
}

function streamSinkStartStep() {
  return step("stream-sink-start", "配信の受け口 (同梱の ffmpeg -listen 1 で " + STREAM_SINK_URL + " を受ける。配信が止まるたびに開き直す) を起こす", {
    url: STREAM_SINK_URL,
    rearm: true,
  });
}

function streamStopIfActiveStep() {
  return step("stream-stop", "テストを実行する人が配信を始めていれば止める (OBS を閉じる前に。配信していなければ何もしない)", { ifActive: true });
}

function streamSinkStopStep() {
  return step("stream-sink-stop", "配信の受け口を閉じる (OBS を閉じた後)", {});
}

// --step hold。
//
// 多くの確認は「実際の Windows と OBS で、こう操作するとこう見えるか」である。どの step も最後に必ず OBS を閉じるので、
// UI Automation のツール (dialog-click・dock-click・dialog-sendkeys・screenshot・dock-width) を手で順に実行できる
// 「OBS を開いたままにする実行」がなかった。OBS を手で起動すると、APPDATA の差し替え・初期設定・配置を手で揃える手間と、
// 閉じ損ねる危険がある。そこで実行スクリプトの形 (先頭のガード・差し替え・配置・後片付け) のまま、
// 待ちの手順 (human の go-file) だけを挟む。
//
// 並び: 初期設定 (--no-seed なら配置だけ) -> 起動 -> 接続 -> (--n-screen / --n-voice があればテスト用の送信側を接続する) ->
// hold-info (OBS の pid と通知ファイルを <out>/window3b/hold-<tag>.json へ) -> human (通知ファイルが置かれるまで、
// 最長 --hold-minutes 分) -> 送信側を止める -> ログの回収 -> OBS を閉じる -> 共通の終了処理。
// 待ちがタイムアウトしても失敗にはしない (gate を付けない。閉じて後片付けすることが大事)。
// 手順の中にテストを実行する人の操作を並べない。操作は実行ごとに違い、見えたものを見て次を決めるので、
// 手順に固定すると、ウィンドウの中で 1 つ足すたびに実行計画を修正して起動し直すことになる。
export function holdHarnessCounts(args) {
  const nScreen = args.nScreenGiven ? args.nScreen : 0;
  const nVoice = args.nVoiceGiven ? args.nVoice : 0;
  return { nScreen, nVoice, any: nScreen + nVoice > 0 };
}

// --self-mic: 自分の枠の音声ソースにする入力を作る手順 (SELF_MIC_INPUT_NAME のコメントも参照)。作った後に、監視が「監視なし」に
// なっていること (音が出ないこと) を読み直し、製品が入力を見つけるまでの猶予を置く。
function selfMicSteps() {
  return [
    step("obsws-request", "自分の枠の音声ソースにする音声入力キャプチャ「" + SELF_MIC_INPUT_NAME + "」(既定のデバイス) を " + WIRE_SCENE_NAME + " に作る", {
      request: "CreateInput",
      requestData: {
        sceneName: WIRE_SCENE_NAME,
        inputName: SELF_MIC_INPUT_NAME,
        inputKind: SELF_MIC_INPUT_KIND,
        inputSettings: { device_id: SELF_MIC_DEVICE_ID },
        sceneItemEnabled: true,
      },
      label: "self-mic-create",
    }),
    step("obsws-request", "入力の監視を「監視なし」にする (OBS は音を鳴らさない)", {
      request: "SetInputAudioMonitorType",
      requestData: { inputName: SELF_MIC_INPUT_NAME, monitorType: SELF_MIC_MONITOR_TYPE },
      label: "self-mic-monitor",
    }),
    step("obsws-request", "入力の監視が「監視なし」になっているかを読み直す", {
      request: "GetInputAudioMonitorType",
      requestData: { inputName: SELF_MIC_INPUT_NAME },
      label: "self-mic-monitor-check",
    }),
    step("wait", "製品が自分の枠の音声ソースを見つけるまでの猶予", { ms: SELF_MIC_FOUND_WAIT_MS }),
  ];
}

// hold の送信側に渡す画面ごと・声ごとの値 (渡した物だけ)。
function holdHarnessExtra(args) {
  const extra = {};
  if (args.screenSizes) extra.screenSizes = args.screenSizes;
  if (args.screenNames) extra.screenNames = args.screenNames;
  if (args.screenImages) extra.screenImages = args.screenImages;
  if (args.voiceUsers) extra.voiceUsers = args.voiceUsers;
  return extra;
}

function buildHoldSteps(args) {
  const steps = [...(args.noSeed ? placeOnlySteps(args) : seedAndPlaceSteps(args)), ...launchAndConnectSteps(args)];
  if (args.selfMic) steps.push(...selfMicSteps());
  // 送信側より前に置く (送信側の有無に関わらず、プレビューに合成のソースを映す)。
  if (args.wireComposite) steps.push(wireCompositeStep("OBS のプレビューに、枠に載った人を描く合成のソースを映す"));
  const harness = holdHarnessCounts(args);
  if (harness.any) {
    const extra = holdHarnessExtra(args);
    const notes = [
      args.screenSizes ? "画面のストリームごとのサイズ " + args.screenSizes : null,
      args.screenNames ? "表示名 " + args.screenNames : null,
      args.screenImages ? "画面に描く画像 " + args.screenImages : null,
      args.voiceUsers ? "声の持ち主 " + args.voiceUsers + " (声に音を乗せる通知ファイルの場所を hold-info に書く)" : null,
    ].filter(Boolean);
    steps.push(
      step("harness-start", "テスト用の送信側を product モードで画面 " + harness.nScreen + " ストリーム + 声 " + harness.nVoice + " ストリームで接続する (待ちの間ずっと接続したまま)" + notes.map((n) => "。" + n).join(""), {
        mode: "product",
        nScreen: harness.nScreen,
        nVoice: harness.nVoice,
        seconds: args.holdMinutes * 60 + HOLD_HARNESS_MARGIN_S,
        hw: args.hw,
        warmup: args.warmup,
        syncTone: DEFAULT_SYNC_TONE,
        // フレームだけを止める通知ファイルの場所を実行スクリプトが決めて渡す (HARNESS_FRAMES_PAUSE_ENV のコメントも参照)。
        framesPause: true,
        // 声の持ち主を決めたときは、声に音を乗せる通知ファイルの場所も実行スクリプトが決めて渡す (HARNESS_VOICE_TONE_ENV)。
        ...(args.voiceUsers ? { voiceTone: true } : {}),
        ...(Object.keys(extra).length > 0 ? { extra } : {}),
      }),
    );
    steps.push(step("wait-connection", "送信側が接続を確立するまで待つ (接続できなくても待ちには入る。ログに残る)", { timeoutMs: DOCK_VIEW_CONNECT_WAIT_TIMEOUT_MS }));
  }
  if (args.streamSink) steps.push(streamSinkStartStep());
  steps.push(step("hold-info", "開いている OBS の pid と通知ファイルの場所を " + HOLD_INFO_FILE_PREFIX + "<tag>.json に書く (テストを実行する人がツールの -Pid に渡す)", {}));
  steps.push(
    humanStep(
      "テスト用のOBSを開いたまま待ちます。確認終えたら通知ファイルを置いてください (" + args.holdMinutes + " 分で自動で閉じます)",
      "go-file",
      args.holdMinutes * 60 * 1000,
      args.holdMinutes * 60,
    ),
  );
  if (harness.any) steps.push(step("harness-stop", "テスト用の送信側を止める", {}));
  if (args.streamSink) steps.push(streamStopIfActiveStep());
  steps.push(step("collect-plugin-log", "プラグインのログの回収"));
  steps.push(...closeSteps("first"));
  // OBS を閉じた後にログを読む (書き終わっている)。
  if (args.selfMic) {
    steps.push(step("collect-self-speaking-log", "OBS のログから自分の枠の音声ソースの測定値 (peak の分布と話している割合の行、入力を見つけた行) を抜き出す", {}));
  }
  if (args.streamSink) steps.push(streamSinkStopStep());
  steps.push(...finishSteps(args, "hold"));
  return steps;
}

// ---- --step call-hold ----------------------------------------------
// 実際の Vesktop と Discord を使う部分 (利用者が操作する) を 1 回の実行で行う: call-gate-a の初期設定と配置 (実際の保存先のまま、
// --dock-install ours、--allow-state-json-write、誰を枠に入れるかは auto = この間こちらはドックを押さない) -> 起動・接続 ->
// state.json のコピー -> 利用者が Vesktop を通知領域の [Quit] で終了して [Vesktopに追加] と [切り替え] (state.json が配置物を指すまで
// 待つ) -> 書いた値の判定 -> 利用者が Vesktop を起動する -> 送信側の hello を待つ (起動し直す前の数より増えたら) -> hold-info ->
// 通知ファイルまで待つ (最長 --hold-minutes。この間にテストを実行する人が、利用者と通話の操作を進める) -> call-1 の締め (利用者が Vesktop を
// [Quit] で終了する -> バックアップを読む -> 利用者が [ツール] からアンインストール -> state.json がバックアップの previous に戻ったかを判定 ->
// 利用者が Vesktop を起動する) -> ログの回収 -> OBS を閉じる -> OBS の録画を残す (テストを実行する人が手で録ったものも =
// STEPS_KEEPING_UNLABELED_OBS_RECORDINGS)。終了の操作が [Quit] なのは VESKTOP_HIDDEN_ICON_HINT の前のコメントを参照。
// 利用者の操作の並びは、Vesktop の終了と [Vesktopに追加] (1 つの依頼) -> 判定 -> Vesktop の起動である ([Vesktopに追加] は Vesktop の
// 起動中は押せない)。
// 締めは call-gate-a / call-gate-c / call-1 と同じ判定ツールと同じ利用者向けの文を使う。
// call-gate-a -> call-gate-b -> call-gate-c -> call-1 の 4 つを続けない。call-gate-a と c は OBS を閉じ、call-1 は
// 123 分の決まった手順で、長い通話の操作を挟めないため。
// --step hold に --real-profile を許さない。hold は偽の state.json が前提で、締めのアンインストールで利用者の Vesktop を
// 元に戻す段階がなく、閉じ損ねると利用者の Vesktop が配置物を指したまま残るため。
// --rehearsal: 偽の state.json (差し替えた APPDATA) とテスト用の送信側で同じ並びを通す (実際の通話の前に 1 度)。
// [Vesktopに追加] とアンインストールは実行スクリプトが押し、開き直す人の操作はない。テスト用の送信側には通知ファイルも渡す。
export function callHoldHarnessCounts(args) {
  if (!args || !args.rehearsal) return { nScreen: 0, nVoice: 0, any: false };
  const nScreen = args.nScreenGiven ? args.nScreen : CALL_HOLD_REHEARSAL_N_SCREEN;
  const nVoice = args.nVoiceGiven ? args.nVoice : CALL_HOLD_REHEARSAL_N_VOICE;
  return { nScreen, nVoice, any: nScreen + nVoice > 0 };
}

// [Vesktopに追加] を押して (実際の通話では、利用者が Vesktop を終了してから押す。リハーサルでは実行スクリプトが押し、利用者の Vesktop は
// 始める前の確認で動いていない)、state.json が配置物を指したこととバックアップを確認し、開き直す前の
// hello の数を保存するまで。call-hold の先頭と、call-1 の --start-with-add の先頭が同じこの並びを使う (CALL_ONE_OPTION_FLAGS のコメントも参照)。
// call-1 に call-gate-a の手順をコピーしない。同じ判定の並びが 2 か所に分かれ、片方だけ修正する恐れがあるため。
export const ADD_TO_VESKTOP_BASELINE_LABEL = "before-reopen";
function addToVesktopSteps(args) {
  const steps = [
    vesktopStateJsonStep("copy-state-json", "state.json のコピーを結果フォルダへ取る (証拠。読むだけで、書くのは製品の [" + TEXT_ID_ADD_TO_VESKTOP + "] を押した結果)", {
      destName: STATE_JSON_BEFORE_NAME,
    }),
    step("wait", "ドックがメッセージを描くまでの猶予 (押す前)", { ms: SELECT_FOUR_SETTLE_WAIT_MS }),
  ];
  if (args.rehearsal) {
    steps.push(
      step("dock-click", "リハーサル: 実行スクリプトが [" + TEXT_ID_ADD_TO_VESKTOP + "] を押す (偽の state.json に製品が書く)", {
        target: "first",
        textId: TEXT_ID_ADD_TO_VESKTOP,
        index: 0,
        invokeTimeoutMs: ROUNDTRIP_INVOKE_TIMEOUT_MS,
        gate: true,
      }),
    );
  } else {
    // 利用者は Vesktop を終了してから押す ([Vesktopに追加] は Vesktop の起動中は押せない。vesktopQuitAndAddMessage の前のコメント)。
    steps.push(
      humanStep(
        vesktopQuitAndAddMessage(),
        "state-json-points-to-placed",
        HUMAN_TIMEOUT_MS,
        HUMAN_ALLOWANCE_SHORT_S,
        { expectedDir: bundledVencordDir(args), gate: true },
      ),
    );
  }
  steps.push(
    step("wait", "state.json が書き終わるまでの猶予", { ms: CALL_GATE_WRITE_SETTLE_MS }),
    vesktopStateJsonStep("read-state-json", "押した直後の state.json を読む", { label: "after-write" }),
    step("judge-vencord-dir", "vencordDir が配置物の data/vencord を指し、他の鍵が 1 文字も変わっていないかを見る", {
      expectedDir: bundledVencordDir(args),
      compareOtherKeys: true,
      gate: true,
    }),
    step("check-file-exists", "製品がバックアップを書いたか (" + VENCORD_DIR_BACKUP_REL + ")", { path: args.obs + "/" + VENCORD_DIR_BACKUP_REL }),
    // 開き直す前の hello の数を保存する (wait-connection はこれより増えたときだけ通す。開き直す前の接続や、前の実行のログの
    // hello で通らないため)。
    step("count-connections", "送信側の hello の数を保存する (開き直した後の接続だけを数えるため)", { label: ADD_TO_VESKTOP_BASELINE_LABEL }),
  );
  return steps;
}

function buildCallHoldSteps(args) {
  const callArgs = { ...args, dockAssignMode: "auto" };
  const steps = [...seedAndPlaceSteps(callArgs), ...launchAndConnectSteps(args)];
  steps.push(...addToVesktopSteps(args));
  const harness = callHoldHarnessCounts(args);
  if (harness.any) {
    steps.push(
      step("harness-start", "リハーサル: テスト用の送信側を product モードで画面 " + harness.nScreen + " ストリーム + 声 " + harness.nVoice + " ストリームで接続する (利用者の Vesktop の代わり。待ちの間ずっと接続したまま)", {
        mode: "product",
        nScreen: harness.nScreen,
        nVoice: harness.nVoice,
        seconds: args.holdMinutes * 60 + HOLD_HARNESS_MARGIN_S,
        hw: args.hw,
        warmup: args.warmup,
        syncTone: DEFAULT_SYNC_TONE,
        framesPause: true,
      }),
    );
  } else if (!args.rehearsal) {
    steps.push(humanStep(VESKTOP_START_AFTER_ADD_MESSAGE, "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_REOPEN_S));
  }
  steps.push(
    step("wait-connection", args.rehearsal ? "テスト用の送信側が接続を確立するまで待つ" : "開き直した Vesktop の送信側が接続してくるのを待つ", {
      timeoutMs: args.rehearsal ? REHEARSAL_CONNECT_WAIT_TIMEOUT_MS : CALL_GATE_CONNECT_WAIT_TIMEOUT_MS,
      baseline: ADD_TO_VESKTOP_BASELINE_LABEL,
      gate: true,
    }),
    step("wait", "ドックの 1 行目と参加者の一覧が引き直されるまでの猶予", { ms: CALL_GATE_SHOT_SETTLE_MS }),
    step("screenshot-window", "ドック (1 行目と参加者の一覧) を PNG に撮る", { target: "first", shotLabel: "dock" }),
    ...(args.streamSink ? [streamSinkStartStep()] : []),
    step("hold-info", "開いている OBS の pid と通知ファイルの場所を " + HOLD_INFO_FILE_PREFIX + "<tag>.json に書く (テストを実行する人がツールの -Pid に渡す)", {}),
    humanStep(
      "テスト用のOBSを開いたまま待ちます。確認終えたら通知ファイルを置いてください (" + args.holdMinutes + " 分で締めに進みます)",
      "go-file",
      args.holdMinutes * 60 * 1000,
      args.holdMinutes * 60,
    ),
  );
  if (harness.any) steps.push(step("harness-stop", "リハーサル: テスト用の送信側を止める (利用者が Vesktop を閉じる代わり)", {}));
  // 配信中はアンインストールできない (uninstall_streaming) ので、締めの前に止める。
  if (args.streamSink) steps.push(streamStopIfActiveStep());
  steps.push(step("collect-plugin-log", "プラグインのログの回収 (締めの前)"));
  // 締め (call-1 と同じ判定と同じ利用者向けの文。callUninstallSteps のコメントも参照)。
  steps.push(...callUninstallSteps(args));
  steps.push(step("wait", "state.json とバックアップが書き終わるまでの猶予", { ms: CALL_GATE_WRITE_SETTLE_MS }));
  steps.push(vesktopStateJsonStep("read-state-json", "アンインストールの後の state.json を読む", { label: "after-uninstall" }));
  steps.push(
    step("judge-vencord-dir-restored", "アンインストールで vencordDir がバックアップの previous に戻ったか (戻しは cleanup.ps1 -CallWindow ではなくアンインストールで済ませる)", {
      backupPath: vencordDirBackupPath(args),
      gate: true,
    }),
  );
  if (!args.rehearsal) steps.push(humanStep(VESKTOP_START_AFTER_UNINSTALL_MESSAGE, "go-file", HUMAN_TIMEOUT_MS, HUMAN_ALLOWANCE_SHORT_S));
  steps.push(...closeSteps("first"));
  if (args.streamSink) steps.push(streamSinkStopStep());
  steps.push(...finishSteps(args, "call-hold"));
  return steps;
}

// args (parseArgs が返した args。args.step は STEP_VALUES のどれかを前提とする) から、
// その step 用の手順の一覧を純粋なデータとして作る。step が不正なら ok:false を返す
// (parseArgs は既に弾いているが、この関数を単独で使う側のためにもう一度確認する)。
export function planSteps(args) {
  if (!STEP_VALUES.includes(args && args.step)) {
    return { ok: false, error: "--step は " + STEP_VALUES.join("|") + " のどれか", steps: [] };
  }
  const builders = {
    place: buildPlaceSteps,
    through: buildThroughSteps,
    integration: buildIntegrationSteps,
    guard: buildGuardSteps,
    "second-obs": buildSecondObsSteps,
    "discord-first": buildDiscordFirstSteps,
    "dock-view": buildDockViewSteps,
    "mic-check": buildMicCheckSteps,
    "call-gate-a": buildCallGateASteps,
    "call-gate-b": buildCallGateBSteps,
    "call-gate-c": buildCallGateCSteps,
    "call-gate-d": buildCallGateDSteps,
    "call-1": buildCallOneSteps,
    record: buildRecordSteps,
    "webcodecs-check": buildWebCodecsCheckSteps,
    "install-roundtrip": buildInstallRoundtripSteps,
    hold: buildHoldSteps,
    "call-hold": buildCallHoldSteps,
  };
  return { ok: true, steps: assignIds(builders[args.step](args)) };
}

// 差し替えた環境変数の組 env の VENCORD_USER_DATA_DIR が、作業のフォルダー workRootAbs (絶対パス) の下を指していなければ、
// その理由を返す (指していれば null)。製品は APPDATA を読まず、Vesktop の設定の場所を VENCORD_USER_DATA_DIR で決めるので、
// 差し替えたつもりで VENCORD_USER_DATA_DIR が抜けると、製品が利用者の実際の %APPDATA%\Vesktop\state.json を書き換える。
// OBS を起動する前に、この確認で止める。
export function vesktopDirSwapProblem(env, workRootAbs) {
  const dir = env && env.VENCORD_USER_DATA_DIR;
  if (typeof dir !== "string" || dir.trim() === "") {
    return "VENCORD_USER_DATA_DIR is not set; the product would use the real Vesktop settings folder";
  }
  if (!isPathUnderRoot(dir, workRootAbs, true)) {
    return "VENCORD_USER_DATA_DIR is not under the work folder: " + dir;
  }
  return null;
}
