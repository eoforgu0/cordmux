# セキュリティポリシー / Security Policy

[English](#english)

## 日本語

### 脆弱性の報告

脆弱性は、Issue などの公開の場に書かないでください。GitHub のプライベート脆弱性レポート (Private vulnerability reporting) で報告できます。報告のフォームは https://github.com/eoforgu0/cordmux/security/advisories/new にあります。

### 対象範囲

cordmux は、同じ PC の OBS と Vesktop の間で、127.0.0.1 への WebSocket 接続を使って映像と音声を受け渡します。接続にはトークンが必要です。cordmux は、OBS を起動するたびにトークンを生成し、OBS を実行している Windows ユーザーだけが読み取れるファイル (`%LOCALAPPDATA%\cordmux\session.json`) に保存します。

同じ Windows ユーザーの権限で動くほかのプログラムと、同じ Vesktop で動くほかのプラグインは、このトークンを読めます。これらのプログラムとプラグインから映像と音声を保護することは、対象範囲に含みません。

## English

### Reporting a Vulnerability

Don't post vulnerabilities publicly, for example in an issue. You can report them through GitHub's private vulnerability reporting. The report form is at https://github.com/eoforgu0/cordmux/security/advisories/new.

### Scope

cordmux passes video and audio between OBS and Vesktop on the same PC over a WebSocket connection on 127.0.0.1. The connection requires a token. cordmux creates a new token each time OBS starts, and saves it in a file that only the current Windows user can read (`%LOCALAPPDATA%\cordmux\session.json`).

Other programs that run as the same Windows user, and other plugins that run in the same Vesktop, can read this token. Protecting the video and audio from these programs and plugins is outside the scope of this policy.
