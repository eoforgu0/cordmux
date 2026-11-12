// windows_api_confinement.py の NG 例。transport/ は paths.ts ではないので、OS を見分ける名前を書いてはいけない。
// 二重引用符以外の書き方 (単引用符とテンプレート文字列)、ブラウザーの API での見分け方、ユーザーフォルダーの環境変数を
// 1 つずつ書く (--selftest が名前ごとに検出を確かめる)。

export function isWindowsBySingleQuote(): boolean {
    return process.platform === 'win32';
}

export function isWindowsByTemplate(): boolean {
    return process.platform === `win32`;
}

export function isWindowsByNavigator(): boolean {
    return navigator.platform.startsWith("Win");
}

export function isWindowsByUserAgentData(): boolean {
    const data = (navigator as unknown as { userAgentData?: { platform: string } }).userAgentData;
    return data?.platform === "Windows";
}

export function homeDir(): string | undefined {
    return process.env.USERPROFILE;
}
