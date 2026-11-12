// windows_api_confinement.py の NG 例。名前が paths.ts でも、許すのは src-vencord の直下の 1 つだけで、
// transport/ の下の paths.ts は許さない。

export function isWindows(): boolean {
    return process.platform === "win32";
}
