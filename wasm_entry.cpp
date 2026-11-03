// 브라우저(WASM)용 진입점: 스레드 없이 UCI 명령을 한 줄씩 처리한다.
// 출력("info ...", "bestmove ...")은 stdout으로 나가고, JS 쪽에서 Module.print 로 받는다.
#define NO_THREADS 1
#define main lunaris_native_main
#include "lunaris.cpp"
#undef main

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

extern "C" {

EMSCRIPTEN_KEEPALIVE void lunaris_init(int hashMb) {
    ios::sync_with_stdio(false);
    initTables(); initEval();
    uci.tt.resize(max(1, hashMb));
    initThreads(uci, 1);
#ifdef HAS_EMBEDDED_NET
    if (!loadNetBytes(NET_DATA, sizeof NET_DATA)) cerr << "embedded net has wrong size, using HCE" << endl;
#endif
    for (int d = 0; d < 64; d++) for (int m = 0; m < 64; m++)
        lmrTab[d][m] = (d && m) ? (int)(0.75 + log(d) * log(m) / 2.25) : 0;
    rootPos.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    gameKeys[0] = rootPos.key; gameLen = 1;
}

// UCI 명령 한 줄 실행. 'go ...' 는 탐색이 끝날 때까지 돌아오지 않는다 (bestmove 출력 후 반환).
EMSCRIPTEN_KEEPALIVE void lunaris_command(const char *line) { uciLine(line); }

}
