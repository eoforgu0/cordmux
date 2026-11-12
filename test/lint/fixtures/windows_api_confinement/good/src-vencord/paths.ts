// windows_api_confinement.py の OK 例。src-vencord の直下の paths.ts では Windows の名前を使ってよい。

export function isWindows(): boolean {
  return process.platform === "win32";
}
