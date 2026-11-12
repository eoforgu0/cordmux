// windows_api_confinement.py の NG 例。ws/ は platform/ ではない。

#include <windows.h>

// このファイルは platform/ の外にあるので、<windows.h> を読み込んではいけない
void start_listener() {}
