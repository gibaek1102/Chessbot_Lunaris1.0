// Lunaris 1.2 — UCI chess engine + self-play data generator
// Build: g++ -O3 -march=native -std=c++17 -pthread lunaris.cpp -o lunaris
//
// v12: 탐색 내부 구조 개선 (같은 노드 예산에서 기력·라벨 품질 동등 이상을 SPRT/갈림길 분석으로 검증)
//  * 단계식 수 생성: TT수 -> 좋은 잡기(SEE는 꺼낼 때만) -> 킬러/카운터 -> 조용한 수(필요할 때만 생성·점수) -> 나쁜 잡기
//    가지치기로 어차피 버려질 조용한 수는 아예 생성하지 않음
//  * 합법성: 핀/체커를 노드당 1회 계산해 수를 두기 전에 판정 (불법수 make/unmake 제거)
//  * 반복 판정 키 필터, 체크 여부 캐시
// v11: NNUE 누적기 지연 갱신, 매직 비트보드, 평가 캐시, 멀티스레드 datagen, 안전 종료, seed 중복 차단
#include <bits/stdc++.h>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#if defined(__AVX2__) || defined(__AVX512BW__)
#include <immintrin.h>
#endif
using namespace std;

typedef uint64_t U64;
typedef uint32_t Move;

// ===================== Basics =====================
enum Color { WHITE, BLACK };
enum PieceType { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
const int NO_PIECE = 12;
inline int pc(int c, int t) { return c * 6 + t; }
inline int ptype(int p) { return p % 6; }
inline int pcolor(int p) { return p / 6; }

inline int lsb(U64 b) { return __builtin_ctzll(b); }
inline int msb(U64 b) { return 63 - __builtin_clzll(b); }
inline int poplsb(U64 &b) { int s = lsb(b); b &= b - 1; return s; }
inline int popcnt(U64 b) { return __builtin_popcountll(b); }

// Move: from(6) | to(6)<<6 | promo(3)<<12 | flag(2)<<16   flag: 1=EP 2=CASTLE 3=DOUBLE PUSH
inline Move mk(int f, int t, int promo = 0, int flag = 0) { return f | (t << 6) | (promo << 12) | (flag << 16); }
inline int mFrom(Move m) { return m & 63; }
inline int mTo(Move m) { return (m >> 6) & 63; }
inline int mPromo(Move m) { return (m >> 12) & 7; }
inline int mFlag(Move m) { return (m >> 16) & 3; }

string sqName(int s) { return string(1, 'a' + s % 8) + string(1, '1' + s / 8); }
string moveStr(Move m) {
    if (!m) return "0000";
    string s = sqName(mFrom(m)) + sqName(mTo(m));
    if (mPromo(m)) s += "nbrq"[mPromo(m) - 1];
    return s;
}

// ===================== Attack tables =====================
U64 knightAtt[64], kingAtt[64], pawnAtt[2][64], rays[8][64];
// 0..3 positive directions (N, E, NE, NW), 4..7 negative (S, W, SW, SE)
const int DR[8] = {1, 0, 1, 1, -1, 0, -1, -1};
const int DF[8] = {0, 1, 1, -1, 0, -1, -1, 1};

U64 zPiece[12][64], zSide, zCastle[16], zEp[8];
U64 betweenBB[64][64], lineBB[64][64];   // strictly-between squares / full line through two aligned squares
int castleMask[64];

inline U64 rayAtt(int d, int s, U64 occ) {
    U64 a = rays[d][s], b = a & occ;
    if (b) a ^= rays[d][d < 4 ? lsb(b) : msb(b)];
    return a;
}
U64 slowSlider(int s, U64 occ, bool rook) {
    static const int RD[4] = {0, 1, 4, 5}, BD[4] = {2, 3, 6, 7};
    U64 a = 0;
    for (int i = 0; i < 4; i++) a |= rayAtt(rook ? RD[i] : BD[i], s, occ);
    return a;
}

// Fancy magic bitboards. Embedded magics are verified at start-up (a fixed-seed search is the fallback).
struct Magic { U64 mask, magic; U64 *att; int shift; };
Magic rookMag[64], bishopMag[64];
U64 rookTable[102400], bishopTable[5248];

static const U64 BISHOP_MAGICS[64] = {
0x0404010404240040ULL, 0x0044013204011018ULL, 0x0008082100200800ULL, 0x0002208200000001ULL,
0x2b02021000008128ULL, 0xe002011108038040ULL, 0x0120490809400021ULL, 0x0008108401205004ULL,
0x40280822686a0400ULL, 0x80000218522c11c0ULL, 0x0040501410404800ULL, 0x2000082040400a14ULL,
0x0008040420000184ULL, 0x0050010120111000ULL, 0x2101040088041020ULL, 0x0102004048049006ULL,
0x4012000810100080ULL, 0x0410044290452300ULL, 0x0008092110410202ULL, 0x6001001805410000ULL,
0x1116805400e01200ULL, 0x0401008210008428ULL, 0x0c0100b88808a204ULL, 0x0012000502c23260ULL,
0x0011404010041108ULL, 0x0028606044242095ULL, 0x0110480001020408ULL, 0x0082008020080880ULL,
0x1410840020802000ULL, 0x0504010018110c80ULL, 0x1028051012090100ULL, 0x0208468005042100ULL,
0x0508844020040800ULL, 0x0111042018020840ULL, 0xd004040105108102ULL, 0x0001820082080080ULL,
0x08100200220a0082ULL, 0x1048008500380910ULL, 0x0001810a02040200ULL, 0x00810a0080ca0841ULL,
0x0818010590042000ULL, 0x0002809c10002000ULL, 0x029108440204d004ULL, 0x0100024208000082ULL,
0x4021202009000220ULL, 0x0a0400c806000040ULL, 0x000202020a020420ULL, 0x0044080200540021ULL,
0x0461009010080400ULL, 0x0800412801101029ULL, 0x4000904054100024ULL, 0x0141004084040080ULL,
0x2020884008288002ULL, 0x100011201d010400ULL, 0x002420080608a000ULL, 0x0008810102020186ULL,
0x0301140402021102ULL, 0x014020420a100220ULL, 0x8404880600422878ULL, 0x0000001e54420880ULL,
0x0200009110820200ULL, 0x4000010930902084ULL, 0x2084042048012104ULL, 0x0148200800450024ULL
};
static const U64 ROOK_MAGICS[64] = {
0x00800020ba400180ULL, 0x0440002000441000ULL, 0x0100120b00200040ULL, 0x4080061000080080ULL,
0x2200020010200804ULL, 0x22000e0024011098ULL, 0x0200020000810804ULL, 0x0200088049020024ULL,
0x4480800040008026ULL, 0x00660021004a0080ULL, 0x600080100080200aULL, 0x0020800804801000ULL,
0x2021001100080004ULL, 0x800a00182a002430ULL, 0x0001000402000100ULL, 0x6620800100104880ULL,
0x0040088000408022ULL, 0x2000404010002007ULL, 0x4401030040600094ULL, 0x0010008010080680ULL,
0x48c4008008008006ULL, 0x0005010008040002ULL, 0x00008c00a8100211ULL, 0x03800200040a80edULL,
0xc0d4400080082080ULL, 0x0000400100210092ULL, 0x0280100480200081ULL, 0x0480100300210008ULL,
0x0408040080800800ULL, 0xa080040080800200ULL, 0x0080018400281042ULL, 0x0500148200040169ULL,
0x0080002000404000ULL, 0x1c40004090802001ULL, 0x2680401101002000ULL, 0x9840800800801004ULL,
0x010a040280800800ULL, 0x2004008004804200ULL, 0x0708015044000862ULL, 0x4002006082000104ULL,
0x0080400020808000ULL, 0x049002200046400aULL, 0x0000200011010040ULL, 0x0040100100090020ULL,
0x10020021100a0004ULL, 0x2342001020040400ULL, 0x8450500201040028ULL, 0x00001881004a0004ULL,
0x4000208001004100ULL, 0x8880200080400480ULL, 0x0000a00080100180ULL, 0x0010004118040140ULL,
0x2020040008008080ULL, 0x5450020080040080ULL, 0x809008015a108400ULL, 0x8000a4108102ca00ULL,
0x0280110229408003ULL, 0x8001f94003008121ULL, 0x400212400a200101ULL, 0x0050002050094501ULL,
0x0002002048118402ULL, 0x4002000801845002ULL, 0x4a40408208011004ULL, 0x0842010020441082ULL
};
void initMagics() {
    static const int RD[4] = {0, 1, 4, 5}, BD[4] = {2, 3, 6, 7};
    mt19937_64 rng(0xC0FFEE1234ULL);
    vector<U64> occs(4096), atts(4096), used(4096);
    vector<int> epoch(4096, 0); int cur = 0;
    for (int rook = 0; rook < 2; rook++) {
        U64 *base = rook ? rookTable : bishopTable;
        size_t off = 0;
        for (int s = 0; s < 64; s++) {
            Magic &M = rook ? rookMag[s] : bishopMag[s];
            U64 mask = 0;
            for (int i = 0; i < 4; i++) {
                U64 r = rays[rook ? RD[i] : BD[i]][s];
                if (r) r ^= 1ULL << ((rook ? RD[i] : BD[i]) < 4 ? msb(r) : lsb(r));   // drop the edge square
                mask |= r;
            }
            int bits = popcnt(mask), n = 0;
            U64 sub = 0;
            do { occs[n] = sub; atts[n] = slowSlider(s, sub, rook); n++; sub = (sub - mask) & mask; } while (sub);
            M.mask = mask; M.shift = 64 - bits; M.att = base + off;
            bool first = true;
            while (true) {
                U64 mg = first ? (rook ? ROOK_MAGICS[s] : BISHOP_MAGICS[s]) : rng() & rng() & rng();   // embedded magic, verified
                if (!first && popcnt((mask * mg) >> 56) < 6) continue;
                first = false;
                cur++;
                bool ok = true;
                for (int i = 0; i < n && ok; i++) {
                    unsigned idx = (unsigned)((occs[i] * mg) >> M.shift);
                    if (epoch[idx] < cur) { epoch[idx] = cur; used[idx] = atts[i]; }
                    else if (used[idx] != atts[i]) ok = false;
                }
                if (ok) { M.magic = mg; break; }
            }
            for (int i = 0; i < n; i++) M.att[(occs[i] * M.magic) >> M.shift] = atts[i];
            off += size_t(1) << bits;
        }
    }
}
inline U64 bishopAtt(int s, U64 o) { const Magic &M = bishopMag[s]; return M.att[((o & M.mask) * M.magic) >> M.shift]; }
inline U64 rookAtt(int s, U64 o) { const Magic &M = rookMag[s]; return M.att[((o & M.mask) * M.magic) >> M.shift]; }

void initTables() {
    for (int s = 0; s < 64; s++) {
        int r = s / 8, f = s % 8;
        auto add = [&](U64 &b, int rr, int ff) { if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) b |= 1ULL << (rr * 8 + ff); };
        int kn[8][2] = {{1,2},{2,1},{-1,2},{-2,1},{1,-2},{2,-1},{-1,-2},{-2,-1}};
        for (auto &d : kn) add(knightAtt[s], r + d[0], f + d[1]);
        for (int a = -1; a <= 1; a++) for (int b = -1; b <= 1; b++) if (a || b) add(kingAtt[s], r + a, f + b);
        add(pawnAtt[WHITE][s], r + 1, f - 1); add(pawnAtt[WHITE][s], r + 1, f + 1);
        add(pawnAtt[BLACK][s], r - 1, f - 1); add(pawnAtt[BLACK][s], r - 1, f + 1);
        for (int d = 0; d < 8; d++) {
            int rr = r + DR[d], ff = f + DF[d];
            while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) { rays[d][s] |= 1ULL << (rr * 8 + ff); rr += DR[d]; ff += DF[d]; }
        }
    }
    mt19937_64 rng(0x5EED1234ULL);
    for (int p = 0; p < 12; p++) for (int s = 0; s < 64; s++) zPiece[p][s] = rng();
    zSide = rng();
    for (int i = 0; i < 16; i++) zCastle[i] = rng();
    for (int i = 0; i < 8; i++) zEp[i] = rng();
    for (int i = 0; i < 64; i++) castleMask[i] = 15;
    castleMask[0] = 15 & ~2; castleMask[7] = 15 & ~1; castleMask[4] = 15 & ~3;
    castleMask[56] = 15 & ~8; castleMask[63] = 15 & ~4; castleMask[60] = 15 & ~12;
    initMagics();
    for (int a = 0; a < 64; a++) for (int d = 0; d < 8; d++) {
        U64 r = rays[d][a];
        while (r) {
            int b = poplsb(r);
            betweenBB[a][b] = rays[d][a] & ~rays[d][b] & ~(1ULL << b);
            lineBB[a][b] = rays[d][a] | rays[d ^ 4][a] | (1ULL << a);
        }
    }
}

// ===================== NNUE =====================
// (768 -> HID)x2 -> 1, SCReLU, perspective net. Quantization QA=255, QB=64, eval scale 400.
#ifndef HIDDEN
#define HIDDEN 512   // default 512 since v10; build with -DHIDDEN=256 or 128 for older nets (net file must match)
#endif
const int HID = HIDDEN, QA = 255, QB = 64, NNUE_SCALE = 400, OUT_B = 8;   // OUT_B output buckets, chosen by piece count
static_assert(HID % 32 == 0, "HIDDEN must be a multiple of 32");
struct Net {
    alignas(64) int16_t w1[768 * HID];
    alignas(64) int16_t b1[HID];
    alignas(64) int16_t w2[OUT_B * 2 * HID];
    int32_t b2[OUT_B];
};
const size_t NET_BYTES = sizeof(int16_t) * (768 * HID + HID + OUT_B * 2 * HID) + sizeof(int32_t) * OUT_B;
const size_t NET_BYTES_V1 = sizeof(int16_t) * (768 * HID + HID + 2 * HID) + sizeof(int32_t);   // old single-output nets
Net net;   // one copy shared (read-only) by every search / datagen thread
bool useNNUE = false;
// piece = color*6 + type. Perspective-relative: own pieces 0..5, enemy 6..11; black flips ranks.
inline int featIdx(int persp, int piece, int sq) {
    if (persp == BLACK) sq ^= 56;
    return ((pcolor(piece) != persp) * 6 + ptype(piece)) * 64 + sq;
}
bool loadNetBytes(const unsigned char *data, size_t n) {
    if (n != NET_BYTES && n != NET_BYTES_V1) return false;
    const unsigned char *q = data;
    memcpy(net.w1, q, sizeof net.w1); q += sizeof net.w1;
    memcpy(net.b1, q, sizeof net.b1); q += sizeof net.b1;
    if (n == NET_BYTES) {
        memcpy(net.w2, q, sizeof net.w2); q += sizeof net.w2;
        memcpy(net.b2, q, sizeof net.b2);
    } else {   // v1 net: one output head, copied into every bucket
        for (int b = 0; b < OUT_B; b++) memcpy(net.w2 + b * 2 * HID, q, sizeof(int16_t) * 2 * HID);
        q += sizeof(int16_t) * 2 * HID;
        int32_t b2v; memcpy(&b2v, q, sizeof b2v);
        for (int b = 0; b < OUT_B; b++) net.b2[b] = b2v;
    }
    useNNUE = true;
    return true;
}
bool loadNetFile(const string &path) {
    ifstream f(path, ios::binary);
    if (!f) return false;
    vector<unsigned char> buf((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    return loadNetBytes(buf.data(), buf.size());
}
#if __has_include("net_data.h")
#include "net_data.h"   // generated by embed_net.py: static const unsigned char NET_DATA[]; NET_DATA_LEN
#define HAS_EMBEDDED_NET 1
#endif

// Pieces that changed in one move (at most 2 added, 3 removed — castling / capture-promotion).
struct DirtyPiece { int8_t addP[2], addS[2], subP[3], subS[3]; uint8_t na, ns; };

// Accumulators live outside Position, one per ply on a per-thread stack, and are brought up to date lazily
// (only when a node is actually evaluated). int16 wrap-around arithmetic is modular, so the values are identical
// to the old eager copy-make updates.
struct alignas(64) Accumulator {
    int16_t v[2][HID];
    DirtyPiece dp;
    bool computed;
};
typedef int16_t vec16 __attribute__((vector_size(64), aligned(64)));   // 32 lanes; split by the compiler to SSE/AVX2/AVX-512
const int VW = 32;
inline const vec16 &V(const int16_t *p) { return *(const vec16 *)p; }
inline vec16 &V(int16_t *p) { return *(vec16 *)p; }
inline const int16_t *wcol(int persp, int p, int s) { return &net.w1[featIdx(persp, p, s) * HID]; }

void accRefresh(const struct Position &p, Accumulator &a);
inline void accUpdate(const Accumulator &prev, Accumulator &cur) {
    const DirtyPiece &d = cur.dp;
    for (int persp = 0; persp < 2; persp++) {
        const int16_t *in = prev.v[persp];
        int16_t *out = cur.v[persp];
        if (d.na == 1 && d.ns == 1) {                       // quiet move
            const int16_t *a0 = wcol(persp, d.addP[0], d.addS[0]), *s0 = wcol(persp, d.subP[0], d.subS[0]);
            for (int i = 0; i < HID; i += VW) V(out + i) = V(in + i) + V(a0 + i) - V(s0 + i);
        } else if (d.na == 1 && d.ns == 2) {                // capture / en passant / promotion
            const int16_t *a0 = wcol(persp, d.addP[0], d.addS[0]), *s0 = wcol(persp, d.subP[0], d.subS[0]), *s1 = wcol(persp, d.subP[1], d.subS[1]);
            for (int i = 0; i < HID; i += VW) V(out + i) = V(in + i) + V(a0 + i) - V(s0 + i) - V(s1 + i);
        } else if (d.na == 2 && d.ns == 2) {                // castling
            const int16_t *a0 = wcol(persp, d.addP[0], d.addS[0]), *a1 = wcol(persp, d.addP[1], d.addS[1]);
            const int16_t *s0 = wcol(persp, d.subP[0], d.subS[0]), *s1 = wcol(persp, d.subP[1], d.subS[1]);
            for (int i = 0; i < HID; i += VW) V(out + i) = V(in + i) + V(a0 + i) + V(a1 + i) - V(s0 + i) - V(s1 + i);
        } else {                                            // null move (no change) / generic
            for (int i = 0; i < HID; i += VW) V(out + i) = V(in + i);
            for (int k = 0; k < d.na; k++) { const int16_t *w = wcol(persp, d.addP[k], d.addS[k]); for (int i = 0; i < HID; i += VW) V(out + i) += V(w + i); }
            for (int k = 0; k < d.ns; k++) { const int16_t *w = wcol(persp, d.subP[k], d.subS[k]); for (int i = 0; i < HID; i += VW) V(out + i) -= V(w + i); }
        }
    }
    cur.computed = true;
}

// ===================== Position =====================
struct Position {
    U64 bb[12], occ[2], all;
    uint8_t board[64];
    int side, ep, castle, halfmove, fullmove;
    U64 key;
    mutable int8_t chk = -1;   // cached inCheck() (-1 = unknown); reset by every move

    void clear() {
        memset(bb, 0, sizeof bb); occ[0] = occ[1] = all = 0;
        for (int i = 0; i < 64; i++) board[i] = NO_PIECE;
        side = WHITE; ep = -1; castle = 0; halfmove = 0; fullmove = 1; key = 0; chk = -1;
    }
    inline void put(int p, int s) { U64 b = 1ULL << s; bb[p] |= b; occ[pcolor(p)] |= b; all |= b; board[s] = p; key ^= zPiece[p][s]; }
    inline void remove(int s) { int p = board[s]; U64 b = 1ULL << s; bb[p] ^= b; occ[pcolor(p)] ^= b; all ^= b; board[s] = NO_PIECE; key ^= zPiece[p][s]; }

    bool attacked(int s, int by) const {
        if (pawnAtt[by ^ 1][s] & bb[pc(by, PAWN)]) return true;
        if (knightAtt[s] & bb[pc(by, KNIGHT)]) return true;
        if (kingAtt[s] & bb[pc(by, KING)]) return true;
        U64 bq = bb[pc(by, BISHOP)] | bb[pc(by, QUEEN)];
        if (bq && (bishopAtt(s, all) & bq)) return true;
        U64 rq = bb[pc(by, ROOK)] | bb[pc(by, QUEEN)];
        if (rq && (rookAtt(s, all) & rq)) return true;
        return false;
    }
    int kingSq(int c) const { return lsb(bb[pc(c, KING)]); }
    bool inCheck() const { if (chk < 0) chk = attacked(kingSq(side), side ^ 1); return chk; }

    void setFen(const string &fen) {
        clear();
        istringstream ss(fen);
        string pos, stm, cas, eps;
        ss >> pos >> stm >> cas >> eps;
        int r = 7, f = 0;
        const string pcs = "PNBRQKpnbrqk";
        for (char c : pos) {
            if (c == '/') { r--; f = 0; }
            else if (isdigit(c)) f += c - '0';
            else { size_t i = pcs.find(c); if (i != string::npos) put((int)i, r * 8 + f); f++; }
        }
        side = (stm == "b") ? BLACK : WHITE;
        for (char c : cas) {
            if (c == 'K') castle |= 1; if (c == 'Q') castle |= 2;
            if (c == 'k') castle |= 4; if (c == 'q') castle |= 8;
        }
        if (eps.size() == 2) ep = (eps[1] - '1') * 8 + (eps[0] - 'a');
        if (!(ss >> halfmove)) halfmove = 0;
        if (!(ss >> fullmove)) fullmove = 1;
        key ^= zCastle[castle];
        if (ep >= 0) key ^= zEp[ep & 7];
        if (side == BLACK) key ^= zSide;
    }
};

void accRefresh(const Position &p, Accumulator &a) {
    for (int persp = 0; persp < 2; persp++) {
        int16_t *out = a.v[persp];
        for (int i = 0; i < HID; i += VW) V(out + i) = V(net.b1 + i);
        U64 b = p.all;
        while (b) { int s = poplsb(b); const int16_t *w = wcol(persp, p.board[s], s); for (int i = 0; i < HID; i += VW) V(out + i) += V(w + i); }
    }
    a.computed = true;
}

// ===================== Move generation =====================
struct MoveList {
    Move m[256]; alignas(32) int score[256 + 8]; int n = 0;   // score[n..n+7] = INT_MIN padding for SIMD scans
    void add(Move x) { m[n++] = x; }
};

// mode: GEN_ALL, GEN_CAPS (captures + every promotion + en passant), GEN_QUIETS (everything else)
enum { GEN_ALL = 0, GEN_CAPS = 1, GEN_QUIETS = 2 };
void genMoves(const Position &p, MoveList &ml, int mode) {
    int us = p.side, them = us ^ 1;
    U64 own = p.occ[us], enemy = p.occ[them];
    U64 targets = mode == GEN_CAPS ? enemy : mode == GEN_QUIETS ? ~p.all : ~own;
    const bool caps = mode != GEN_QUIETS, quiets = mode != GEN_CAPS;

    // Pawns
    U64 pawns = p.bb[pc(us, PAWN)];
    int fwd = us == WHITE ? 8 : -8;
    U64 rank7 = us == WHITE ? 0x00FF000000000000ULL : 0xFF00ULL;
    U64 rank3 = us == WHITE ? 0xFF0000ULL : 0xFF0000000000ULL;
    while (pawns) {
        int s = poplsb(pawns), t = s + fwd;
        bool promo = (1ULL << s) & rank7;
        if (!((p.all >> t) & 1)) {
            if (promo) { if (caps) for (int pt = QUEEN; pt >= KNIGHT; pt--) ml.add(mk(s, t, pt)); }
            else if (quiets) {
                ml.add(mk(s, t));
                int t2 = t + fwd;
                if (((1ULL << t) & rank3) && !((p.all >> t2) & 1)) ml.add(mk(s, t2, 0, 3));
            }
        }
        if (caps) {
            U64 a = pawnAtt[us][s] & enemy;
            while (a) {
                int t2 = poplsb(a);
                if (promo) { for (int pt = QUEEN; pt >= KNIGHT; pt--) ml.add(mk(s, t2, pt)); }
                else ml.add(mk(s, t2));
            }
            if (p.ep >= 0 && ((pawnAtt[us][s] >> p.ep) & 1)) ml.add(mk(s, p.ep, 0, 1));
        }
    }
    // Pieces
    for (int pt = KNIGHT; pt <= KING; pt++) {
        U64 b = p.bb[pc(us, pt)];
        while (b) {
            int s = poplsb(b);
            U64 a;
            switch (pt) {
                case KNIGHT: a = knightAtt[s]; break;
                case BISHOP: a = bishopAtt(s, p.all); break;
                case ROOK:   a = rookAtt(s, p.all); break;
                case QUEEN:  a = bishopAtt(s, p.all) | rookAtt(s, p.all); break;
                default:     a = kingAtt[s];
            }
            a &= targets;
            while (a) ml.add(mk(s, poplsb(a)));
        }
    }
    // Castling
    if (quiets) {
        if (us == WHITE) {
            if ((p.castle & 1) && !(p.all & 0x60ULL) && !p.attacked(4, BLACK) && !p.attacked(5, BLACK) && !p.attacked(6, BLACK)) ml.add(mk(4, 6, 0, 2));
            if ((p.castle & 2) && !(p.all & 0xEULL) && !p.attacked(4, BLACK) && !p.attacked(3, BLACK) && !p.attacked(2, BLACK)) ml.add(mk(4, 2, 0, 2));
        } else {
            if ((p.castle & 4) && !(p.all & (0x60ULL << 56)) && !p.attacked(60, WHITE) && !p.attacked(61, WHITE) && !p.attacked(62, WHITE)) ml.add(mk(60, 62, 0, 2));
            if ((p.castle & 8) && !(p.all & (0xEULL << 56)) && !p.attacked(60, WHITE) && !p.attacked(59, WHITE) && !p.attacked(58, WHITE)) ml.add(mk(60, 58, 0, 2));
        }
    }
}

// True iff genMoves(p, GEN_ALL) would produce m (used to validate TT moves / killers without generating).
bool pseudoLegal(const Position &p, Move m) {
    if (!m || (m & ~0x37FFFu)) return false;   // bit 15 and bits 18+ are never set in a real move
    int from = mFrom(m), to = mTo(m), promo = mPromo(m), flag = mFlag(m), us = p.side, them = us ^ 1;
    int pce = p.board[from];
    if (pce == NO_PIECE || pcolor(pce) != us || from == to) return false;
    if (p.board[to] != NO_PIECE && pcolor(p.board[to]) == us) return false;
    int pt = ptype(pce);
    if (pt == PAWN) {
        int fwd = us == WHITE ? 8 : -8, r = from >> 3;
        bool onR7 = r == (us == WHITE ? 6 : 1);
        if (flag == 1) return !promo && p.ep >= 0 && to == p.ep && ((pawnAtt[us][from] >> to) & 1);
        if (flag == 3) return !promo && r == (us == WHITE ? 1 : 6) && to == from + 2 * fwd
                              && p.board[from + fwd] == NO_PIECE && p.board[to] == NO_PIECE;
        if (flag != 0) return false;
        if (onR7 != (promo != 0) || (promo && (promo < KNIGHT || promo > QUEEN))) return false;
        if (to == from + fwd) return p.board[to] == NO_PIECE;
        if ((pawnAtt[us][from] >> to) & 1) return p.board[to] != NO_PIECE && pcolor(p.board[to]) == them;
        return false;
    }
    if (promo) return false;
    if (flag == 2) {
        if (pt != KING) return false;
        if (us == WHITE) {
            if (from != 4) return false;
            if (to == 6) return (p.castle & 1) && !(p.all & 0x60ULL) && !p.attacked(4, BLACK) && !p.attacked(5, BLACK) && !p.attacked(6, BLACK);
            if (to == 2) return (p.castle & 2) && !(p.all & 0xEULL) && !p.attacked(4, BLACK) && !p.attacked(3, BLACK) && !p.attacked(2, BLACK);
        } else {
            if (from != 60) return false;
            if (to == 62) return (p.castle & 4) && !(p.all & (0x60ULL << 56)) && !p.attacked(60, WHITE) && !p.attacked(61, WHITE) && !p.attacked(62, WHITE);
            if (to == 58) return (p.castle & 8) && !(p.all & (0xEULL << 56)) && !p.attacked(60, WHITE) && !p.attacked(59, WHITE) && !p.attacked(58, WHITE);
        }
        return false;
    }
    if (flag) return false;
    U64 a;
    switch (pt) {
        case KNIGHT: a = knightAtt[from]; break;
        case BISHOP: a = bishopAtt(from, p.all); break;
        case ROOK:   a = rookAtt(from, p.all); break;
        case QUEEN:  a = bishopAtt(from, p.all) | rookAtt(from, p.all); break;
        default:     a = kingAtt[from];
    }
    return (a >> to) & 1;
}

// Plays m (no legality test). If dp is given, the changed pieces are recorded there for the lazy NNUE update.
inline void doMove(Position &p, Move m, DirtyPiece *dp) {
    int f = mFrom(m), t = mTo(m), promo = mPromo(m), flag = mFlag(m);
    int us = p.side, them = us ^ 1, piece = p.board[f];
    DirtyPiece tmp; DirtyPiece &d = dp ? *dp : tmp;
    d.na = d.ns = 0;
    auto rem = [&](int s) { d.subP[d.ns] = (int8_t)p.board[s]; d.subS[d.ns++] = (int8_t)s; p.remove(s); };
    auto add = [&](int pp, int s) { d.addP[d.na] = (int8_t)pp; d.addS[d.na++] = (int8_t)s; p.put(pp, s); };
    p.key ^= zCastle[p.castle];
    if (p.ep >= 0) p.key ^= zEp[p.ep & 7];
    p.halfmove++;
    if (flag == 1) rem(t + (us == WHITE ? -8 : 8));
    else if (p.board[t] != NO_PIECE) { rem(t); p.halfmove = 0; }
    rem(f);
    add(promo ? pc(us, promo) : piece, t);
    if (ptype(piece) == PAWN) p.halfmove = 0;
    if (flag == 2) {
        if (t == 6)       { rem(7);  add(pc(us, ROOK), 5); }
        else if (t == 2)  { rem(0);  add(pc(us, ROOK), 3); }
        else if (t == 62) { rem(63); add(pc(us, ROOK), 61); }
        else              { rem(56); add(pc(us, ROOK), 59); }
    }
    p.ep = (flag == 3) ? (f + t) / 2 : -1;
    p.castle &= castleMask[f] & castleMask[t];
    p.key ^= zCastle[p.castle];
    if (p.ep >= 0) p.key ^= zEp[p.ep & 7];
    if (us == BLACK) p.fullmove++;
    p.side = them; p.key ^= zSide; p.chk = -1;
}
// Copy-make. Returns false if the move leaves own king in check.
bool makeMove(Position &p, Move m, DirtyPiece *dp = nullptr) {
    doMove(p, m, dp);
    return !p.attacked(p.kingSq(p.side ^ 1), p.side);
}

void makeNull(Position &p) {
    if (p.ep >= 0) { p.key ^= zEp[p.ep & 7]; p.ep = -1; }
    p.side ^= 1; p.key ^= zSide; p.halfmove++; p.chk = -1;
}

bool hasLegalMove(const Position &p) {
    MoveList ml; genMoves(p, ml, GEN_ALL);
    for (int i = 0; i < ml.n; i++) { Position c = p; if (makeMove(c, ml.m[i])) return true; }
    return false;
}
vector<Move> legalMoves(const Position &p) {
    MoveList ml; genMoves(p, ml, GEN_ALL);
    vector<Move> v;
    for (int i = 0; i < ml.n; i++) { Position c = p; if (makeMove(c, ml.m[i])) v.push_back(ml.m[i]); }
    return v;
}

// ===================== Perft =====================
U64 perft(const Position &p, int depth) {
    MoveList ml; genMoves(p, ml, GEN_ALL);
    U64 n = 0;
    for (int i = 0; i < ml.n; i++) {
        Position c = p;
        if (!makeMove(c, ml.m[i])) continue;
        n += depth <= 1 ? 1 : perft(c, depth - 1);
    }
    return n;
}
void perftDivide(const Position &p, int depth) {
    auto t0 = chrono::steady_clock::now();
    MoveList ml; genMoves(p, ml, GEN_ALL);
    U64 total = 0;
    for (int i = 0; i < ml.n; i++) {
        Position c = p;
        if (!makeMove(c, ml.m[i])) continue;
        U64 n = depth <= 1 ? 1 : perft(c, depth - 1);
        cout << moveStr(ml.m[i]) << ": " << n << "\n";
        total += n;
    }
    double s = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
    cout << "\nNodes: " << total << "  time " << fixed << setprecision(2) << s << "s  nps " << (U64)(total / max(s, 1e-9)) << endl;
}

// ===================== Evaluation (tapered, hand-crafted fallback) =====================
const int MAT_MG[6] = {82, 337, 365, 477, 1025, 0};
const int MAT_EG[6] = {94, 281, 297, 512, 936, 0};
const int PHASE_W[6] = {0, 1, 1, 2, 4, 0};
int pstMg[12][64], pstEg[12][64];

void initEval() {
    for (int s = 0; s < 64; s++) {
        int r = s / 8, f = s % 8;
        double cd = fabs(r - 3.5) + fabs(f - 3.5);   // 1..7
        double fc = fabs(f - 3.5);                    // 0.5..3.5
        int mg[6], eg[6];
        mg[PAWN] = (r - 1) * 4 + (fc < 1 ? (r >= 3 ? 18 : 8) : 0) - (fc > 2.5 ? 4 : 0);
        eg[PAWN] = (r - 1) * (r - 1) * 3 + (r - 1) * 4;
        if (r == 0 || r == 7) mg[PAWN] = eg[PAWN] = 0;
        mg[KNIGHT] = (int)((4 - cd) * 10);   eg[KNIGHT] = (int)((4 - cd) * 8);
        mg[BISHOP] = (int)((4 - cd) * 5) + (r == 0 ? -10 : 0); eg[BISHOP] = (int)((4 - cd) * 5);
        mg[ROOK]   = (r == 6 ? 20 : 0) + (fc < 1 ? 6 : 0); eg[ROOK] = (r == 6 ? 12 : 0);
        mg[QUEEN]  = (int)((4 - cd) * 2) + (r == 0 ? -4 : 0); eg[QUEEN] = (int)((4 - cd) * 6);
        mg[KING]   = (r == 0 ? (f == 6 || f == 1 || f == 2 ? 30 : (f == 4 || f == 3 ? 0 : 15)) : -15 * r - 10);
        eg[KING]   = (int)((4 - cd) * 12);
        for (int t = 0; t < 6; t++) {
            pstMg[pc(WHITE, t)][s] = MAT_MG[t] + mg[t];
            pstEg[pc(WHITE, t)][s] = MAT_EG[t] + eg[t];
            pstMg[pc(BLACK, t)][s ^ 56] = MAT_MG[t] + mg[t];
            pstEg[pc(BLACK, t)][s ^ 56] = MAT_EG[t] + eg[t];
        }
    }
}

const U64 FILE_A = 0x0101010101010101ULL;
int evaluateHCE(const Position &p) {
    int mg[2] = {0, 0}, eg[2] = {0, 0}, phase = 0;
    for (int c = 0; c < 2; c++) {
        U64 ownPawns = p.bb[pc(c, PAWN)], oppPawns = p.bb[pc(c ^ 1, PAWN)];
        for (int t = 0; t < 6; t++) {
            int P = pc(c, t);
            U64 b = p.bb[P];
            phase += PHASE_W[t] * popcnt(b);
            while (b) {
                int s = poplsb(b);
                mg[c] += pstMg[P][s]; eg[c] += pstEg[P][s];
                int f = s & 7;
                U64 file = FILE_A << f;
                if (t == BISHOP) { int mob = popcnt(bishopAtt(s, p.all) & ~p.occ[c]); mg[c] += (mob - 6) * 4; eg[c] += (mob - 6) * 5; }
                else if (t == KNIGHT) { int mob = popcnt(knightAtt[s] & ~p.occ[c]); mg[c] += (mob - 4) * 4; eg[c] += (mob - 4) * 4; }
                else if (t == ROOK) {
                    int mob = popcnt(rookAtt(s, p.all) & ~p.occ[c]); mg[c] += (mob - 7) * 2; eg[c] += (mob - 7) * 4;
                    if (!(file & ownPawns)) { mg[c] += (file & oppPawns) ? 12 : 25; eg[c] += 10; }
                } else if (t == QUEEN) { int mob = popcnt((bishopAtt(s, p.all) | rookAtt(s, p.all)) & ~p.occ[c]); mg[c] += (mob - 14); eg[c] += (mob - 14) * 2; }
                else if (t == PAWN) {
                    U64 adj = ((f > 0 ? file >> 1 : 0) | (f < 7 ? file << 1 : 0));
                    if (!(adj & ownPawns)) { mg[c] -= 12; eg[c] -= 18; }              // isolated
                    if (popcnt(file & ownPawns) > 1) { mg[c] -= 6; eg[c] -= 14; }    // doubled (counted per pawn)
                    int r = s >> 3;
                    U64 front = c == WHITE ? (~0ULL << (8 * (r + 1))) : (r ? (~0ULL >> (8 * (8 - r))) : 0);
                    if (!((file | adj) & front & oppPawns)) {
                        int rr = c == WHITE ? r : 7 - r;
                        mg[c] += rr * rr * 2; eg[c] += rr * rr * 6;
                    }
                } else if (t == KING) {
                    U64 shield = (file | (f > 0 ? file >> 1 : 0) | (f < 7 ? file << 1 : 0)) & ownPawns;
                    int r = s >> 3;
                    U64 near = c == WHITE ? (0xFFFFULL << (8 * min(r + 1, 7))) : (r >= 2 ? (0xFFFFULL << (8 * (r - 2))) : 0xFFULL);
                    mg[c] += popcnt(shield & near) * 12;
                }
            }
        }
        if (popcnt(p.bb[pc(c, BISHOP)]) >= 2) { mg[c] += 30; eg[c] += 50; }
    }
    phase = min(phase, 24);
    int mgS = mg[0] - mg[1], egS = eg[0] - eg[1];
    int score = (mgS * phase + egS * (24 - phase)) / 24;
    score += 12; // tempo for white, flipped below
    return p.side == WHITE ? score : -(score - 24);
}

// sum_i clamp(acc_i,0,QA)^2 * w_i. Per-lane int32 is exact (|v*w| <= 255*127 fits int16), the cross-lane sum is int64,
// so the result is the same integer for every SIMD width.
inline int64_t screluDot(const int16_t *acc, const int16_t *w) {
#if defined(__AVX512BW__)
    const __m512i zero = _mm512_setzero_si512(), qa = _mm512_set1_epi16(QA);
    __m512i sum = _mm512_setzero_si512();
    for (int i = 0; i < HID; i += 32) {
        __m512i v = _mm512_load_si512((const void *)(acc + i));
        v = _mm512_min_epi16(_mm512_max_epi16(v, zero), qa);
        __m512i t = _mm512_mullo_epi16(v, _mm512_load_si512((const void *)(w + i)));
#if defined(__AVX512VNNI__)
        sum = _mm512_dpwssd_epi32(sum, t, v);
#else
        sum = _mm512_add_epi32(sum, _mm512_madd_epi16(t, v));
#endif
    }
    __m512i lo = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(sum)), hi = _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(sum, 1));
    return _mm512_reduce_add_epi64(_mm512_add_epi64(lo, hi));
#elif defined(__AVX2__)
    const __m256i zero = _mm256_setzero_si256(), qa = _mm256_set1_epi16(QA);
    __m256i sum = _mm256_setzero_si256();
    for (int i = 0; i < HID; i += 16) {
        __m256i v = _mm256_load_si256((const __m256i *)(acc + i));
        v = _mm256_min_epi16(_mm256_max_epi16(v, zero), qa);
        __m256i t = _mm256_mullo_epi16(v, _mm256_load_si256((const __m256i *)(w + i)));
#if defined(__AVXVNNI__)
        sum = _mm256_dpwssd_avx_epi32(sum, t, v);
#else
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(t, v));
#endif
    }
    __m256i lo = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(sum)), hi = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(sum, 1));
    __m256i s4 = _mm256_add_epi64(lo, hi);
    alignas(32) int64_t r[4]; _mm256_store_si256((__m256i *)r, s4);
    return r[0] + r[1] + r[2] + r[3];
#else
    int64_t s = 0;
    for (int i = 0; i < HID; i++) { int v = min(max((int)acc[i], 0), QA); s += (int16_t)(v * w[i]) * v; }
    return s;
#endif
}
int nnueEval(const Position &p, const Accumulator &a) {
    const int16_t *us = a.v[p.side], *them = a.v[p.side ^ 1];
    int bucket = max(0, min(OUT_B - 1, (popcnt(p.all) - 2) / 4));   // 2..32 pieces -> 0..7
    const int16_t *w2 = net.w2 + bucket * 2 * HID;
    int64_t sum = (screluDot(us, w2) + screluDot(them, w2 + HID)) / QA + net.b2[bucket];
    int e = (int)(sum * NNUE_SCALE / (QA * QB));
    return max(-20000, min(20000, e));
}
int evaluateStatic(const Position &p) {   // one-off evaluation (UCI "eval")
    if (!useNNUE) return evaluateHCE(p);
    static Accumulator a; accRefresh(p, a);
    return nnueEval(p, a);
}

// All UCI output goes through one lock: the search thread prints while the main thread reads commands.
mutex ioMtx;
inline void uciOut(const string &s) { lock_guard<mutex> lk(ioMtx); cout << s << endl; }

// ===================== Search state =====================
const int INF = 32000, MATE = 31000, MAXPLY = 128;
enum { TT_EXACT = 0, TT_LOWER = 1, TT_UPPER = 2 };
// Two 64-bit words per entry, stored as (key ^ data, data): a torn write by another thread fails the key check
// and is treated as a miss, so search threads can share the table without locks.
struct TTEntry { U64 a, b; };
struct TTData { Move move; int16_t score; int8_t depth; uint8_t flag; };

struct TransTable {
    TTEntry *e = nullptr; U64 mask = 0; size_t n = 0;
    ~TransTable() { free(e); }
    void resize(int mb) {
        size_t k = 1; while (k * 2 * sizeof(TTEntry) <= (size_t)mb * 1024 * 1024) k *= 2;
        if (k != n) {
            free(e); n = k; mask = k - 1;
            size_t bytes = n * sizeof(TTEntry), al = bytes >= (2u << 20) ? (2u << 20) : 64;   // 2MB-aligned -> transparent huge pages
            e = (TTEntry *)aligned_alloc(al, bytes);
#ifdef MADV_HUGEPAGE
            madvise(e, n * sizeof(TTEntry), MADV_HUGEPAGE);
#endif
        }
        clear();
    }
    void clear() { memset(e, 0, n * sizeof(TTEntry)); }
    size_t mb() const { return n * sizeof(TTEntry) >> 20; }
};

struct ThreadData;
// Everything one search (one UCI search, or one datagen worker) shares between its threads.
struct SearchShared {
    TransTable tt;
    atomic<bool> stop{false};
    chrono::steady_clock::time_point tStart;
    long long hardLimitMs = 0, softLimitMs = 0;
    U64 softNodeLimit = 0, hardNodeLimit = 0;
    bool silent = false;
    Move lastBest = 0; int lastScore = 0;
    vector<unique_ptr<ThreadData>> threads;
};

#ifndef EVAL_CACHE_BITS
#define EVAL_CACHE_BITS 16
#endif
struct StackEntry { int piece, to; };
// Per-thread search state (Lazy SMP): every search thread owns one; only the TT is shared.
struct ThreadData {
    SearchShared *sh;
    TTEntry *ttE; U64 ttMask;   // cached from sh->tt
    U64 nodes;
    Move killers[MAXPLY][2];
    int histTab[12][64];
    U64 keyHist[4096]; int histLen;
    uint16_t repCnt[4096];                     // how many keyHist entries share these low 12 key bits (repetition pre-filter)
    Move rootBest;
    int evalStack[MAXPLY + 4];
    Move counterMove[12][64];                  // counter-move & continuation history (1-ply and 2-ply)
    int16_t contHist[12][64][12][64];
    int16_t corrHist[2][16384];                // eval correction by pawn structure, units of 1/256 cp
    StackEntry stk[MAXPLY + 4];
    Accumulator acc[MAXPLY + 8];
    // Eval cache: static eval is a pure function of the position, and iterative deepening re-evaluates the same
    // positions over and over. Entry = (key with low 16 bits replaced by the eval); index uses the low bits, so a
    // hit means the full 64-bit key matched. Results are bit-identical to evaluating again.
    U64 evalCache[1 << EVAL_CACHE_BITS];
};
thread_local ThreadData *TD = nullptr;

void initThreads(SearchShared &S, int n) {
    S.threads.clear();
    for (int i = 0; i < n; i++) {
        S.threads.push_back(make_unique<ThreadData>());   // value-initialised: all zero
        S.threads.back()->sh = &S;
    }
}
U64 threadNodes(const ThreadData *t) { return t->nodes; }
U64 totalNodes(SearchShared &S) { U64 t = 0; for (auto &d : S.threads) t += d->nodes; return t; }
void clearHistories(SearchShared &S) {
    for (auto &d : S.threads) {
        memset(d->histTab, 0, sizeof d->histTab); memset(d->contHist, 0, sizeof d->contHist);
        memset(d->corrHist, 0, sizeof d->corrHist); memset(d->counterMove, 0, sizeof d->counterMove);
    }
}
void rebuildRep(ThreadData *d) { memset(d->repCnt, 0, sizeof d->repCnt); for (int i = 0; i < d->histLen; i++) d->repCnt[d->keyHist[i] & 4095]++; }
void setGameHistory(SearchShared &S, const U64 *keys, int n) {
    for (auto &d : S.threads) { memcpy(d->keyHist, keys, sizeof(U64) * n); d->histLen = n; rebuildRep(d.get()); }
}
#define nodes (TD->nodes)
#define killers (TD->killers)
#define histTab (TD->histTab)
#define keyHist (TD->keyHist)
#define histLen (TD->histLen)
#define rootBest (TD->rootBest)
#define evalStack (TD->evalStack)
#define counterMove (TD->counterMove)
#define contHist (TD->contHist)
#define corrHist (TD->corrHist)
#define SH (TD->sh)

// Lazy NNUE: bring acc[ply] up to date from the nearest computed ancestor.
inline int evaluateNoCache(const Position &p, int ply) {
    if (!useNNUE) return evaluateHCE(p);
    Accumulator *A = TD->acc;
    if (!A[ply].computed) {
        int j = ply - 1;
        while (!A[j].computed) j--;
        for (int k = j + 1; k <= ply; k++) accUpdate(A[k - 1], A[k]);
    }
    return nnueEval(p, A[ply]);
}
inline int evaluate(const Position &p, int ply) {
    static_assert(EVAL_CACHE_BITS >= 16, "eval cache index must cover the low 16 key bits");
    U64 &e = TD->evalCache[p.key & ((1ULL << EVAL_CACHE_BITS) - 1)];
    if (e && ((e ^ p.key) >> 16) == 0) return (int16_t)(uint16_t)e;
    int v = evaluateNoCache(p, ply);
    if (v >= -32768 && v <= 32767) e = (p.key & ~0xFFFFULL) | (uint16_t)(int16_t)v;
    return v;
}
void clearEvalCaches(SearchShared &S) { for (auto &d : S.threads) memset(d->evalCache, 0, sizeof d->evalCache); }

inline bool ttRead(U64 key, TTData &d) {
    const TTEntry &e = TD->ttE[key & TD->ttMask];
    U64 b = __atomic_load_n(&e.b, __ATOMIC_RELAXED), a = __atomic_load_n(&e.a, __ATOMIC_RELAXED);
    d.move = (Move)(b & 0xFFFFFFFFULL); d.score = (int16_t)(uint16_t)(b >> 32); d.depth = (int8_t)(uint8_t)(b >> 48); d.flag = (uint8_t)(b >> 56);
    return (a ^ b) == key;
}
inline void ttWrite(U64 key, U64 packed) {
    TTEntry &e = TD->ttE[key & TD->ttMask];
    __atomic_store_n(&e.b, packed, __ATOMIC_RELAXED); __atomic_store_n(&e.a, key ^ packed, __ATOMIC_RELAXED);
}
inline void ttPrefetch(U64 key) { __builtin_prefetch(&TD->ttE[key & TD->ttMask]); }
inline U64 ttPack(Move m, int score, int depth, int flag) {
    return (U64)m | ((U64)(uint16_t)(int16_t)score << 32) | ((U64)(uint8_t)(int8_t)depth << 48) | ((U64)(uint8_t)flag << 56);
}
inline int toTT(int s, int ply) { return s >= MATE - MAXPLY ? s + ply : s <= -MATE + MAXPLY ? s - ply : s; }
inline int fromTT(int s, int ply) { return s >= MATE - MAXPLY ? s - ply : s <= -MATE + MAXPLY ? s + ply : s; }

int lmrTab[64][64];
inline int contScore(int ply, int pce, int to) {
    int s = 0;
    if (ply >= 1 && TD->stk[ply - 1].piece >= 0) s += contHist[TD->stk[ply - 1].piece][TD->stk[ply - 1].to][pce][to];
    if (ply >= 2 && TD->stk[ply - 2].piece >= 0) s += contHist[TD->stk[ply - 2].piece][TD->stk[ply - 2].to][pce][to];
    return s;
}
inline void gravity(int16_t &e, int bonus) { e += bonus - e * abs(bonus) / 16384; }

inline long long elapsedMs(const SearchShared &S) { return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - S.tStart).count(); }
inline void checkTime() {
    SearchShared &S = *SH;
    if (S.hardLimitMs > 0 && elapsedMs(S) >= S.hardLimitMs) S.stop.store(true, memory_order_relaxed);
    if (S.hardNodeLimit > 0 && nodes >= S.hardNodeLimit) S.stop.store(true, memory_order_relaxed);
}
inline bool stopped() { return SH->stop.load(memory_order_relaxed); }

inline void pushKey(U64 k) { keyHist[histLen++] = k; TD->repCnt[k & 4095]++; }
inline void popKey() { TD->repCnt[keyHist[--histLen] & 4095]--; }
bool isRepetition(const Position &p) {
    if (TD->repCnt[p.key & 4095] < 2) return false;   // the current position is the only one with these key bits
    for (int i = histLen - 3; i >= 0 && i >= histLen - 1 - p.halfmove; i -= 2)
        if (keyHist[i] == p.key) return true;
    return false;
}
bool hasNonPawn(const Position &p) {
    int c = p.side;
    return p.bb[pc(c, KNIGHT)] | p.bb[pc(c, BISHOP)] | p.bb[pc(c, ROOK)] | p.bb[pc(c, QUEEN)];
}

const int SEE_VAL[7] = {100, 320, 330, 500, 900, 20000, 0};
inline bool isCapture(const Position &p, Move m) { return p.board[mTo(m)] != NO_PIECE || mFlag(m) == 1; }

// Static Exchange Evaluation: true if the exchange on mTo(m) nets >= threshold
U64 attackersTo(const Position &p, int s, U64 occ) {
    return (pawnAtt[BLACK][s] & p.bb[pc(WHITE, PAWN)]) | (pawnAtt[WHITE][s] & p.bb[pc(BLACK, PAWN)])
         | (knightAtt[s] & (p.bb[pc(WHITE, KNIGHT)] | p.bb[pc(BLACK, KNIGHT)]))
         | (kingAtt[s] & (p.bb[pc(WHITE, KING)] | p.bb[pc(BLACK, KING)]))
         | (bishopAtt(s, occ) & (p.bb[pc(WHITE, BISHOP)] | p.bb[pc(BLACK, BISHOP)] | p.bb[pc(WHITE, QUEEN)] | p.bb[pc(BLACK, QUEEN)]))
         | (rookAtt(s, occ) & (p.bb[pc(WHITE, ROOK)] | p.bb[pc(BLACK, ROOK)] | p.bb[pc(WHITE, QUEEN)] | p.bb[pc(BLACK, QUEEN)]));
}
bool see(const Position &p, Move m, int threshold) {
    if (mFlag(m) == 1 || mFlag(m) == 2 || mPromo(m)) return true;
    int from = mFrom(m), to = mTo(m);
    int swapv = (p.board[to] == NO_PIECE ? 0 : SEE_VAL[ptype(p.board[to])]) - threshold;
    if (swapv < 0) return false;
    swapv = SEE_VAL[ptype(p.board[from])] - swapv;
    if (swapv <= 0) return true;
    U64 occ = p.all ^ (1ULL << from) ^ (1ULL << to);
    U64 diag = p.bb[pc(WHITE, BISHOP)] | p.bb[pc(BLACK, BISHOP)] | p.bb[pc(WHITE, QUEEN)] | p.bb[pc(BLACK, QUEEN)];
    U64 orth = p.bb[pc(WHITE, ROOK)] | p.bb[pc(BLACK, ROOK)] | p.bb[pc(WHITE, QUEEN)] | p.bb[pc(BLACK, QUEEN)];
    U64 att = attackersTo(p, to, occ);
    int stm = p.side, res = 1;
    while (true) {
        stm ^= 1;
        att &= occ;
        U64 mine = att & p.occ[stm];
        if (!mine) break;
        res ^= 1;
        int pt = PAWN;
        while (!(mine & p.bb[pc(stm, pt)])) pt++;
        if (pt == KING) return (att & p.occ[stm ^ 1]) ? res ^ 1 : res;
        if ((swapv = SEE_VAL[pt] - swapv) < res) break;
        occ ^= 1ULL << lsb(mine & p.bb[pc(stm, pt)]);
        if (pt == PAWN || pt == BISHOP || pt == QUEEN) att |= bishopAtt(to, occ) & diag;
        if (pt == ROOK || pt == QUEEN) att |= rookAtt(to, occ) & orth;
    }
    return res;
}

// ---- legality without making the move: pins + checkers computed once per node ----
struct ChkInfo { U64 checkers, pinned; int ksq; };
inline void computeChk(const Position &p, ChkInfo &ci) {
    int us = p.side, them = us ^ 1;
    ci.ksq = p.kingSq(us);
    ci.checkers = attackersTo(p, ci.ksq, p.all) & p.occ[them];
    ci.pinned = 0;
    U64 snipers = (rookAtt(ci.ksq, 0) & (p.bb[pc(them, ROOK)] | p.bb[pc(them, QUEEN)]))
                | (bishopAtt(ci.ksq, 0) & (p.bb[pc(them, BISHOP)] | p.bb[pc(them, QUEEN)]));
    while (snipers) {
        int sq = poplsb(snipers);
        U64 b = betweenBB[ci.ksq][sq] & p.all;
        if (b && !(b & (b - 1)) && (b & p.occ[us])) ci.pinned |= b;
    }
}
// m must be pseudo-legal. Same answer as "make it and see if the king is attacked".
inline bool legalMove(const Position &p, Move m, const ChkInfo &ci) {
    int from = mFrom(m), to = mTo(m);
    if (mFlag(m) == 1) { Position c = p; return makeMove(c, m); }   // en passant: rare, do it the slow way
    if (from == ci.ksq) {
        if (mFlag(m) == 2) return true;                                // castling: checked at generation
        return !(attackersTo(p, to, p.all ^ (1ULL << from)) & p.occ[p.side ^ 1]);
    }
    if (ci.checkers) {
        if (ci.checkers & (ci.checkers - 1)) return false;            // double check: king must move
        if (!(((ci.checkers | betweenBB[ci.ksq][lsb(ci.checkers)]) >> to) & 1)) return false;
    }
    if ((ci.pinned >> from) & 1) return (lineBB[ci.ksq][from] >> to) & 1;
    return true;
}

// Selects the first index holding the maximum score among sc[i..n) (sc[n..n+7] must be INT_MIN padding).
inline void pickBest(Move *mv, int *sc, int n, int i) {
    int best = i;
#if defined(__AVX2__)
    if (n - i > 6) {
        __m256i vm = _mm256_set1_epi32(INT_MIN);
        for (int k = i; k < n; k += 8) vm = _mm256_max_epi32(vm, _mm256_loadu_si256((const __m256i *)(sc + k)));
        __m128i h = _mm_max_epi32(_mm256_castsi256_si128(vm), _mm256_extracti128_si256(vm, 1));
        h = _mm_max_epi32(h, _mm_shuffle_epi32(h, 0x4E)); h = _mm_max_epi32(h, _mm_shuffle_epi32(h, 0xB1));
        __m256i vx = _mm256_set1_epi32(_mm_cvtsi128_si32(h));
        for (int k = i;; k += 8) {
            unsigned msk = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_loadu_si256((const __m256i *)(sc + k)), vx)));
            if (msk) { best = k + __builtin_ctz(msk); break; }
        }
    } else
#endif
    {
        for (int j = i + 1; j < n; j++) if (sc[j] > sc[best]) best = j;
    }
    swap(mv[i], mv[best]); swap(sc[i], sc[best]);
}

// Captures by MVV-LVA (+promotion bonus); push promotions after every capture, under-promotions last.
inline int capScore(const Position &p, Move m) {
    int to = mTo(m);
    if (p.board[to] != NO_PIECE || mFlag(m) == 1) {
        int victim = mFlag(m) == 1 ? PAWN : ptype(p.board[to]);
        int sc = SEE_VAL[victim] * 10 - SEE_VAL[ptype(p.board[mFrom(m)])] / 10;
        return mPromo(m) ? sc + 50000 : sc;
    }
    return mPromo(m) == QUEEN ? -100000 : -200000;
}

// Staged move ordering for the main search: TT move -> good captures (SEE tested lazily) -> killers -> counter move
// -> quiets by history -> bad captures. Quiets are only generated / scored if the node gets that far, and not at all
// once the caller sets skipQuiets (all remaining quiets would be pruned anyway).
struct MovePicker {
    enum { TT, GEN_C, GOOD_C, KILL0, KILL1, COUNTER, GEN_Q, QUIET, BAD_INIT, BAD, DONE };
    const Position &p;
    Move tt, k0, k1, cm;
    const int16_t *ch1 = nullptr, *ch2 = nullptr;
    int stage = TT;
    bool skipQuiets = false;
    bool goodCap = false;   // last returned move is a capture already proven SEE >= 0
    // One buffer: captures at [0,nc); losing captures are moved down to [0,nb) as they are met (nb <= ci);
    // quiets are generated after the captures, at [nc, ml.n).
    MoveList ml;
    int nc = 0, ci = 0, nb = 0, bi = 0, qi = 0;

    MovePicker(const Position &pos, Move ttm, int ply) : p(pos), tt(ttm) {
        k0 = killers[ply][0]; k1 = killers[ply][1]; cm = 0;
        if (ply >= 1 && TD->stk[ply - 1].piece >= 0) {
            cm = counterMove[TD->stk[ply - 1].piece][TD->stk[ply - 1].to];
            ch1 = &contHist[TD->stk[ply - 1].piece][TD->stk[ply - 1].to][0][0];
        }
        if (ply >= 2 && TD->stk[ply - 2].piece >= 0) ch2 = &contHist[TD->stk[ply - 2].piece][TD->stk[ply - 2].to][0][0];
        if (!pseudoLegal(p, tt)) tt = 0;
    }
    inline bool validQuiet(Move m) const {
        return m && m != tt && !(p.board[mTo(m)] != NO_PIECE || mFlag(m) == 1) && !mPromo(m) && pseudoLegal(p, m);
    }
    Move next() {
        goodCap = false;
        switch (stage) {
        case TT:
            stage = GEN_C;
            if (tt) return tt;
            [[fallthrough]];
        case GEN_C: {
            stage = GOOD_C;
            genMoves(p, ml, GEN_CAPS);
            int n = 0;
            for (int i = 0; i < ml.n; i++) { Move m = ml.m[i]; if (m == tt) continue; ml.m[n] = m; ml.score[n++] = capScore(p, m); }
            ml.n = nc = n;
            for (int i = 0; i < 8; i++) ml.score[n + i] = INT_MIN;
        }
            [[fallthrough]];
        case GOOD_C:
            while (ci < nc) {
                pickBest(ml.m, ml.score, nc, ci);
                Move m = ml.m[ci]; int sc = ml.score[ci]; ci++;
                if (sc <= -150000 || (!mPromo(m) && !see(p, m, 0))) { ml.m[nb] = m; ml.score[nb++] = sc; continue; }
                goodCap = true;
                return m;
            }
            stage = KILL0;
            [[fallthrough]];
        case KILL0:
            stage = KILL1;
            if (!skipQuiets && validQuiet(k0)) return k0;
            [[fallthrough]];
        case KILL1:
            stage = COUNTER;
            if (!skipQuiets && k1 != k0 && validQuiet(k1)) return k1;
            [[fallthrough]];
        case COUNTER:
            stage = GEN_Q;
            if (!skipQuiets && cm != k0 && cm != k1 && validQuiet(cm)) return cm;
            [[fallthrough]];
        case GEN_Q:
            stage = QUIET;
            if (!skipQuiets) {
                ml.n = nc;
                genMoves(p, ml, GEN_QUIETS);
                const int (*hist)[64] = histTab;
                int n = nc;
                for (int i = nc; i < ml.n; i++) {
                    Move m = ml.m[i];
                    if (m == tt || m == k0 || m == k1 || m == cm) continue;   // already tried (if they were valid)
                    int mover = p.board[mFrom(m)], to = mTo(m), idx = mover * 64 + to;
                    int sc = hist[mover][to];
                    if (ch1) sc += ch1[idx];
                    if (ch2) sc += ch2[idx];
                    ml.m[n] = m; ml.score[n++] = sc;
                }
                ml.n = n; qi = nc;
                for (int i = 0; i < 8; i++) ml.score[n + i] = INT_MIN;
            }
            [[fallthrough]];
        case QUIET:
            if (!skipQuiets && qi < ml.n) { pickBest(ml.m, ml.score, ml.n, qi); return ml.m[qi++]; }
            stage = BAD_INIT;
            [[fallthrough]];
        case BAD_INIT:
            for (int i = 0; i < 8; i++) ml.score[nb + i] = INT_MIN;   // everything past nb is consumed by now
            stage = BAD;
            [[fallthrough]];
        case BAD:
            if (bi < nb) { pickBest(ml.m, ml.score, nb, bi); return ml.m[bi++]; }
            stage = DONE;
            [[fallthrough]];
        default:
            return 0;
        }
    }
};

// Plays a (legal) move for the child at ply+1, recording the dirty pieces into acc[ply+1] (evaluated lazily later).
inline void doChild(Position &c, Move m, int ply) {
    Accumulator &A = TD->acc[ply + 1];
    doMove(c, m, &A.dp);
    A.computed = false;
    ttPrefetch(c.key);
}

// ===================== Search =====================
int qsearch(Position &p, int alpha, int beta, int ply) {
    if ((++nodes & 2047) == 0) checkTime();
    if (stopped()) return 0;
    bool pvNode = beta - alpha > 1;
    TTData e; bool ttHit = ttRead(p.key, e);
    Move ttMove = 0; int ttScore = 0;
    if (ttHit) {
        ttMove = e.move; ttScore = fromTT(e.score, ply);
        if (!pvNode && (e.flag == TT_EXACT || (e.flag == TT_LOWER && ttScore >= beta) || (e.flag == TT_UPPER && ttScore <= alpha))) return ttScore;
    }
    int stand = evaluate(p, ply);
    if (ply >= MAXPLY - 1) return stand;
    if (ttHit && abs(ttScore) < MATE - MAXPLY && (e.flag == TT_EXACT || (e.flag == TT_LOWER && ttScore > stand) || (e.flag == TT_UPPER && ttScore < stand))) stand = ttScore;
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;
    int origAlpha = alpha;
    Move bestMove = 0;
    MoveList ml; genMoves(p, ml, GEN_CAPS);
    for (int i = 0; i < ml.n; i++) ml.score[i] = ml.m[i] == ttMove ? 10000000 : capScore(p, ml.m[i]);
    for (int i = 0; i < 8; i++) ml.score[ml.n + i] = INT_MIN;
    ChkInfo ci; bool ciReady = false;
    int best = stand;
    for (int i = 0; i < ml.n; i++) {
        pickBest(ml.m, ml.score, ml.n, i);
        Move m = ml.m[i];
        if (!mPromo(m)) {
            int victim = mFlag(m) == 1 ? PAWN : ptype(p.board[mTo(m)]);
            if (stand + SEE_VAL[victim] + 200 < alpha) continue;   // delta pruning
            if (!see(p, m, 0)) continue;                            // skip losing captures (SEE only when reached)
        }
        if (!ciReady) { computeChk(p, ci); ciReady = true; }
        if (!legalMove(p, m, ci)) continue;
        Position c = p;
        doChild(c, m, ply);
        int s = -qsearch(c, -beta, -alpha, ply + 1);
        if (stopped()) return 0;
        if (s > best) { best = s; bestMove = m; if (s > alpha) { alpha = s; if (s >= beta) break; } }
    }
    {   // never evict a deeper search entry with a qsearch one
        TTData cur; ttRead(p.key, cur);
        if (cur.depth <= 0) ttWrite(p.key, ttPack(bestMove, toTT(best, ply), 0, best >= beta ? TT_LOWER : (best > origAlpha ? TT_EXACT : TT_UPPER)));
    }
    return best;
}

int search(Position &p, int depth, int alpha, int beta, int ply, bool doNull) {
    bool pvNode = beta - alpha > 1;
    if (ply > 0 && (p.halfmove >= 100 || isRepetition(p))) return 0;
    bool inCheck = p.inCheck();
    if (inCheck) depth++;
    if (depth <= 0) return qsearch(p, alpha, beta, ply);
    if ((++nodes & 2047) == 0) checkTime();
    if (stopped()) return 0;
    if (ply >= MAXPLY - 1) return evaluate(p, ply);

    // Mate distance pruning
    if (ply > 0) {
        alpha = max(alpha, -MATE + ply);
        beta = min(beta, MATE - ply - 1);
        if (alpha >= beta) return alpha;
    }

    TTData e; bool ttHit = ttRead(p.key, e);
    Move ttMove = 0;
    if (ttHit) {
        ttMove = e.move;
        if (!pvNode && e.depth >= depth) {
            int s = fromTT(e.score, ply);
            if (e.flag == TT_EXACT || (e.flag == TT_LOWER && s >= beta) || (e.flag == TT_UPPER && s <= alpha)) return s;
        }
    }
    if (!ttMove && depth >= 4) depth--;

    int rawEval = inCheck ? -INF : evaluate(p, ply);
    int pawnIdx = (int)(((p.bb[pc(WHITE, PAWN)] * 0x9E3779B97F4A7C15ULL) ^ (p.bb[pc(BLACK, PAWN)] * 0xC2B2AE3D27D4EB4FULL)) >> 50);
    int staticEval = inCheck ? -INF : rawEval + corrHist[p.side][pawnIdx] / 256;
    int evalBeforeTT = staticEval;
    evalStack[ply] = staticEval;
    bool improving = !inCheck && ply >= 2 && evalStack[ply - 2] != -INF && staticEval > evalStack[ply - 2];
    if (!inCheck && ttHit && abs(e.score) < MATE - MAXPLY) {
        int ts = fromTT(e.score, ply);
        if (e.flag == TT_EXACT || (e.flag == TT_LOWER && ts > staticEval) || (e.flag == TT_UPPER && ts < staticEval)) staticEval = ts;
    }

    if (!pvNode && !inCheck) {
        // Reverse futility pruning
        if (depth <= 6 && staticEval - 80 * (depth - improving) >= beta && abs(beta) < MATE - MAXPLY) return staticEval;
        // Null move pruning
        if (doNull && depth >= 3 && staticEval >= beta && hasNonPawn(p)) {
            Position c = p; makeNull(c);
            Accumulator &A = TD->acc[ply + 1]; A.dp.na = A.dp.ns = 0; A.computed = false;
            TD->stk[ply].piece = -1;
            pushKey(c.key);
            int R = 3 + depth / 6;
            int s = -search(c, depth - 1 - R, -beta, -beta + 1, ply + 1, false);
            popKey();
            if (stopped()) return 0;
            if (s >= beta) return s >= MATE - MAXPLY ? beta : s;
        }
    }

    MovePicker mp(p, ttMove, ply);
    ChkInfo ci; computeChk(p, ci);
    int best = -INF, legal = 0, origAlpha = alpha;
    Move bestMove = 0;
    Move quietsTried[64]; int nQuiets = 0;
    while (true) {
        // When this holds every quiet move is pruned below, so don't even generate / score them.
        if (!pvNode && !inCheck && legal > 0 && depth <= 3 && best > -MATE + MAXPLY
            && (staticEval + 120 * depth <= alpha || legal >= 4 + depth * depth * 3)) mp.skipQuiets = true;
        Move m = mp.next();
        if (!m) break;
        bool quiet = !isCapture(p, m) && !mPromo(m);

        // Futility / late move pruning for quiet moves
        if (!pvNode && !inCheck && quiet && legal > 0 && best > -MATE + MAXPLY) {
            if (depth <= 3 && staticEval + 120 * depth <= alpha) continue;
            if (depth <= 3 && legal >= 4 + depth * depth * 3) continue;
        }
        // SEE pruning: drop moves that lose too much material at shallow depth
        if (!pvNode && !inCheck && legal > 0 && depth <= 8 && best > -MATE + MAXPLY) {
            if (quiet ? !see(p, m, -50 * depth) : (!mp.goodCap && !see(p, m, -25 * depth * depth))) continue;
        }

        if (!legalMove(p, m, ci)) continue;
        int movedPiece = p.board[mFrom(m)];
        Position c = p;
        doChild(c, m, ply);
        legal++;
        TD->stk[ply].piece = movedPiece; TD->stk[ply].to = mTo(m);
        if (quiet && nQuiets < 64) quietsTried[nQuiets++] = m;
        pushKey(c.key);
        int s;
        if (legal == 1) s = -search(c, depth - 1, -beta, -alpha, ply + 1, true);
        else {
            int r = 0;
            if (depth >= 3 && quiet && !inCheck && legal > 3) {
                r = lmrTab[min(depth, 63)][min(legal, 63)];
                if (pvNode) r--;
                if (c.inCheck()) r--;
                r = max(0, min(r, depth - 2));
            }
            s = -search(c, depth - 1 - r, -alpha - 1, -alpha, ply + 1, true);
            if (s > alpha && r > 0) s = -search(c, depth - 1, -alpha - 1, -alpha, ply + 1, true);
            if (s > alpha && s < beta) s = -search(c, depth - 1, -beta, -alpha, ply + 1, true);
        }
        popKey();
        if (stopped()) return 0;

        if (s > best) {
            best = s; bestMove = m;
            if (s > alpha) {
                alpha = s;
                if (ply == 0) rootBest = m;
                if (s >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                        int &h = histTab[p.board[mFrom(m)]][mTo(m)];
                        h += depth * depth;
                        if (h > 700000) for (auto &row : histTab) for (int &x : row) x /= 2;
                        if (ply >= 1 && TD->stk[ply - 1].piece >= 0) counterMove[TD->stk[ply - 1].piece][TD->stk[ply - 1].to] = m;
                        int bonus = min(16 * depth * depth + 32 * depth, 1200);
                        for (int q = 0; q < nQuiets; q++) {
                            Move qm = quietsTried[q];
                            int b = (qm == m) ? bonus : -bonus;
                            int pce = p.board[mFrom(qm)], to = mTo(qm);
                            if (ply >= 1 && TD->stk[ply - 1].piece >= 0) gravity(contHist[TD->stk[ply - 1].piece][TD->stk[ply - 1].to][pce][to], b);
                            if (ply >= 2 && TD->stk[ply - 2].piece >= 0) gravity(contHist[TD->stk[ply - 2].piece][TD->stk[ply - 2].to][pce][to], b);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (!legal) return inCheck ? -MATE + ply : 0;

    int flag = best >= beta ? TT_LOWER : (best > origAlpha ? TT_EXACT : TT_UPPER);
    if (!inCheck && abs(best) < MATE - MAXPLY && (!bestMove || (!isCapture(p, bestMove) && !mPromo(bestMove)))
        && !(flag == TT_LOWER && best <= evalBeforeTT) && !(flag == TT_UPPER && best >= evalBeforeTT)) {
        int16_t &ch = corrHist[p.side][pawnIdx];
        int w = min(depth + 1, 16);
        int target = max(-16384, min(16384, (best - rawEval) * 256));
        ch = (int16_t)max(-16384, min(16384, (ch * (256 - w) + target * w) / 256));
    }
    {
        TTData cur; bool hit = ttRead(p.key, cur);
        if (!hit || depth >= cur.depth || flag == TT_EXACT) ttWrite(p.key, ttPack(bestMove, toTT(best, ply), depth, flag));
    }
    return best;
}

bool isLegalMove(const Position &p, Move m) {
    MoveList ml; genMoves(p, ml, GEN_ALL);
    for (int i = 0; i < ml.n; i++) if (ml.m[i] == m) { Position c = p; return makeMove(c, m); }
    return false;
}

string getPV(Position p, int depth) {
    string pv;
    set<U64> seen;
    for (int i = 0; i < depth; i++) {
        TTData e;
        if (!ttRead(p.key, e) || !e.move || seen.count(p.key) || !isLegalMove(p, e.move)) break;
        seen.insert(p.key);
        pv += moveStr(e.move) + " ";
        makeMove(p, e.move);
    }
    return pv;
}

const int skipSize[20]  = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4};
const int skipPhase[20] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 6, 7};

// Iterative deepening for one search thread. Thread 0 owns time control and UCI output; helpers only feed the TT.
void searchLoop(SearchShared *S, Position root, int maxDepth, int id) {
    TD = S->threads[id].get();
    TD->ttE = S->tt.e; TD->ttMask = S->tt.mask;
    const bool isMain = id == 0;
    nodes = 0;
    memset(killers, 0, sizeof killers);
    for (auto &e : TD->stk) e.piece = -1;
    for (auto &row : histTab) for (int &x : row) x /= 8;
    if (useNNUE) accRefresh(root, TD->acc[0]);
    Move best = 0;
    {   // fallback: first legal move
        MoveList ml; genMoves(root, ml, GEN_ALL);
        for (int i = 0; i < ml.n; i++) { Position c = root; if (makeMove(c, ml.m[i])) { best = ml.m[i]; break; } }
    }
    int prevScore = 0;
    for (int d = 1; d <= maxDepth; d++) {
        if (!isMain) { int k = (id - 1) % 20; if (((d + skipPhase[k]) / skipSize[k]) & 1) continue; }
        rootBest = 0;
        int alpha = -INF, beta = INF, delta = 25, score;
        if (d >= 5) { alpha = max(prevScore - delta, -INF); beta = min(prevScore + delta, INF); }
        while (true) {
            score = search(root, d, alpha, beta, 0, false);
            if (stopped()) break;
            if (score <= alpha) { beta = (alpha + beta) / 2; alpha = max(score - delta, -INF); }
            else if (score >= beta) { beta = min(score + delta, INF); }
            else break;
            delta += delta / 2;
            if (delta > 1000) { alpha = -INF; beta = INF; }
        }
        if (rootBest) best = rootBest;
        if (stopped()) break;
        prevScore = score;
        if (!isMain) continue;
        if (!S->silent) {
            long long ms = elapsedMs(*S);
            U64 total = totalNodes(*S);
            string sc = abs(score) >= MATE - MAXPLY
                ? "mate " + to_string(score > 0 ? (MATE - score + 1) / 2 : -(MATE + score) / 2)
                : "cp " + to_string(score);
            uciOut("info depth " + to_string(d) + " score " + sc + " nodes " + to_string(total)
                   + " nps " + to_string(total * 1000 / max(ms, 1LL)) + " time " + to_string(ms) + " pv " + getPV(root, d));
        }
        if (S->softLimitMs > 0 && elapsedMs(*S) * 100 >= S->softLimitMs * 55) break;   // next iteration usually costs ~2x
        if (S->softNodeLimit > 0 && nodes >= S->softNodeLimit) break;
        if (abs(score) >= MATE - MAXPLY && d >= 8 && S->softLimitMs > 0) break;
    }
    if (!isMain) return;
    S->lastBest = best; S->lastScore = prevScore;
    if (!S->silent) uciOut("bestmove " + moveStr(best));
}

void think(SearchShared *S, Position root, int maxDepth) {
    S->tStart = chrono::steady_clock::now();
    vector<thread> helpers;
    for (int id = 1; id < (int)S->threads.size(); id++) helpers.emplace_back(searchLoop, S, root, maxDepth, id);
    searchLoop(S, root, maxDepth, 0);
    if (!helpers.empty()) { S->stop = true; for (auto &h : helpers) h.join(); }
}

// ===================== Data generation =====================
string toFen(const Position &p) {
    const string pcs = "PNBRQKpnbrqk";
    string s;
    for (int r = 7; r >= 0; r--) {
        int e = 0;
        for (int f = 0; f < 8; f++) {
            int x = p.board[r * 8 + f];
            if (x == NO_PIECE) e++;
            else { if (e) { s += char('0' + e); e = 0; } s += pcs[x]; }
        }
        if (e) s += char('0' + e);
        if (r) s += '/';
    }
    s += p.side == WHITE ? " w " : " b ";
    string c;
    if (p.castle & 1) c += 'K'; if (p.castle & 2) c += 'Q';
    if (p.castle & 4) c += 'k'; if (p.castle & 8) c += 'q';
    s += (c.empty() ? "-" : c) + " " + (p.ep >= 0 ? sqName(p.ep) : "-") + " " + to_string(p.halfmove) + " " + to_string(p.fullmove);
    return s;
}
bool insufficientMaterial(const Position &p) {
    if (p.bb[pc(WHITE, PAWN)] | p.bb[pc(BLACK, PAWN)] | p.bb[pc(WHITE, ROOK)] | p.bb[pc(BLACK, ROOK)]
        | p.bb[pc(WHITE, QUEEN)] | p.bb[pc(BLACK, QUEEN)]) return false;
    return popcnt(p.all) <= 3;   // K vs K, K+minor vs K
}

atomic<int> gSignals{0};                // 1st Ctrl+C: finish current games and exit cleanly; 2nd: exit now
atomic<U64> gGames{0}, gPositions{0};
extern "C" void onSignal(int) {
    if (gSignals.fetch_add(1) >= 1) _exit(130);   // completed games are already on disk (one write() per game)
    static const char msg[] = "\n[Ctrl+C] 진행 중인 게임만 마치고 종료합니다 (바로 끝내려면 한 번 더)\n";
    ssize_t r = ::write(2, msg, sizeof msg - 1); (void)r;
}

bool writeAll(int fd, const string &s) {
    const char *p = s.data(); size_t left = s.size();
    while (left) { ssize_t w = ::write(fd, p, left); if (w < 0) { if (errno == EINTR) continue; return false; } p += w; left -= (size_t)w; }
    return true;
}

// One self-play worker == one old "lunaris datagen" process with the same seed: identical output, line for line.
// Output: "<fen> | <white-relative cp> | <1.0/0.5/0.0 white result>"
void datagenWorker(int games, U64 nodesPerMove, const string &outPath, uint64_t seed, bool verbose) {
    mt19937_64 rng(seed);
    int fd = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { cerr << "cannot open " << outPath << endl; return; }
    SearchShared S;
    initThreads(S, 1);
    S.silent = true; S.softNodeLimit = nodesPerMove; S.hardNodeLimit = nodesPerMove * 8;
    S.hardLimitMs = S.softLimitMs = 0;
    TD = S.threads[0].get();
    U64 written = 0;
    auto t0 = chrono::steady_clock::now();
    string buf; buf.reserve(1 << 16);
    for (int g = 0; g < games && !gSignals.load(); g++) {
        S.tt.resize(16);   // allocated once, zeroed every game (same state as a fresh table)
        clearHistories(S);
        Position pos; pos.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        histLen = 0; memset(TD->repCnt, 0, sizeof TD->repCnt); pushKey(pos.key);
        int randomPlies = 8 + (int)(rng() & 1);
        bool ok = true;
        for (int i = 0; i < randomPlies && ok; i++) {
            vector<Move> lm = legalMoves(pos);
            if (lm.empty()) { ok = false; break; }
            makeMove(pos, lm[rng() % lm.size()]); pushKey(pos.key);
        }
        if (!ok || !hasLegalMove(pos)) continue;
        buf.clear();
        size_t nrec = 0;
        double result = 0.5;
        int winRun = 0, drawRun = 0, ply = 0;
        bool discard = false;
        vector<size_t> lineEnds;   // records are written with a placeholder result, patched at the end
        while (true) {
            if (!hasLegalMove(pos)) { result = pos.inCheck() ? (pos.side == WHITE ? 0.0 : 1.0) : 0.5; break; }
            if (pos.halfmove >= 100 || isRepetition(pos) || insufficientMaterial(pos) || histLen >= 1000) { result = 0.5; break; }
            S.stop = false;
            searchLoop(&S, pos, MAXPLY - 1, 0);
            TD = S.threads[0].get();
            Move bm = S.lastBest; int sc = S.lastScore;
            int wsc = pos.side == WHITE ? sc : -sc;
            if (ply == 0 && abs(sc) > 600) { discard = true; break; }   // unbalanced random opening
            if (abs(sc) >= MATE - MAXPLY) { result = wsc > 0 ? 1.0 : 0.0; break; }
            if (abs(sc) >= 2000) { if (++winRun >= 4) { result = wsc > 0 ? 1.0 : 0.0; break; } } else winRun = 0;
            if (ply >= 60 && abs(sc) <= 8) { if (++drawRun >= 12) { result = 0.5; break; } } else drawRun = 0;
            if (!pos.inCheck() && !isCapture(pos, bm) && !mPromo(bm)) {
                buf += toFen(pos); buf += " | "; buf += to_string(wsc); buf += " | 0.5\n";
                lineEnds.push_back(buf.size());
                nrec++;
            }
            makeMove(pos, bm); pushKey(pos.key); ply++;
        }
        if (discard) continue;
        const char *rs = result == 1.0 ? "1.0" : result == 0.0 ? "0.0" : "0.5";
        for (size_t end : lineEnds) memcpy(&buf[end - 4], rs, 3);
        if (!buf.empty() && !writeAll(fd, buf)) { cerr << "write failed: " << outPath << " (" << strerror(errno) << ")" << endl; break; }
        written += nrec;
        gGames.fetch_add(1); gPositions.fetch_add(nrec);
        if (verbose && (g + 1) % 20 == 0) {
            double secs = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
            cerr << "games " << g + 1 << " positions " << written << " (" << (int)(written / secs) << " pos/s)" << endl;
        }
    }
    ::close(fd);
}

bool fileHasData(const string &path) {
    struct stat st; return ::stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

// lunaris gen <threads> <seedStart> <gamesPerThread> [nodes=5000] [outDir=data]
//   thread i uses seed seedStart+i and writes <outDir>/gen_<seed>.txt (same file/contents as the old per-process setup)
int datagenMulti(int threads, uint64_t seedStart, int games, U64 nodesPer, const string &outDir, bool force) {
    ::mkdir(outDir.c_str(), 0755);
    vector<string> paths;
    for (int i = 0; i < threads; i++) {
        string p = outDir + "/gen_" + to_string(seedStart + i) + ".txt";
        if (fileHasData(p) && !force) {
            cerr << "이미 데이터가 있는 seed 입니다: " << p << "\n"
                 << "같은 seed를 다시 쓰면 같은 게임이 중복됩니다. 다른 seed를 쓰세요 (예: " << seedStart + threads << ").\n"
                 << "(정말 덮어붙이려면 마지막 인자에 --force)" << endl;
            return 1;
        }
        paths.push_back(p);
    }
    signal(SIGINT, onSignal); signal(SIGTERM, onSignal); signal(SIGHUP, onSignal); signal(SIGPIPE, SIG_IGN);
    ofstream logf(outDir + "/log.txt", ios::app);   // progress goes to the terminal and to <outDir>/log.txt
    auto log = [&](const string &msg) { cerr << msg << endl; if (logf) { logf << msg << "\n"; logf.flush(); } };
    log("Lunaris datagen: " + to_string(threads) + " threads, seed " + to_string(seedStart) + ".." + to_string(seedStart + threads - 1)
        + ", " + to_string(games) + " games/thread, " + to_string(nodesPer) + " nodes/move -> " + outDir + "/");
    vector<thread> ws;
    atomic<int> running{threads};
    for (int i = 0; i < threads; i++)
        ws.emplace_back([&, i] { datagenWorker(games, nodesPer, paths[i], seedStart + i, false); running--; });
    auto t0 = chrono::steady_clock::now();
    U64 lastPos = 0; auto lastT = t0;
    int tick = 0;
    while (running.load() > 0) {
        this_thread::sleep_for(chrono::milliseconds(250));
        if (++tick % 40 != 0 && running.load() > 0) continue;   // report every 10 s
        auto now = chrono::steady_clock::now();
        double secs = chrono::duration<double>(now - t0).count(), dt = chrono::duration<double>(now - lastT).count();
        U64 pos = gPositions.load();
        log("[" + to_string((long long)secs) + "s] games " + to_string(gGames.load()) + " positions " + to_string(pos)
            + " (" + to_string((long long)(pos / max(secs, 1e-9))) + " pos/s avg, " + to_string((long long)((pos - lastPos) / max(dt, 1e-9))) + " now)"
            + (gSignals.load() ? "  [종료 중: 진행 중인 게임 마무리]" : ""));
        lastPos = pos; lastT = now;
    }
    for (auto &w : ws) w.join();
    log("완료: games " + to_string(gGames.load()) + " positions " + to_string(gPositions.load()));
    return 0;
}


// Hidden "selftest": fuzzes move generation / legality / staged ordering against brute force.
int selftest(int positions) {
    SearchShared S; initThreads(S, 1); S.tt.resize(1); TD = S.threads[0].get(); TD->ttE = S.tt.e; TD->ttMask = S.tt.mask;
    mt19937_64 rng(12345);
    const char *starts[] = {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8"};
    vector<Move> universe;
    long long checked = 0, errors = 0;
    for (int it = 0; it < positions; it++) {
        Position pos; pos.setFen(starts[it % 5]);
        int plies = rng() % 120;
        for (int i = 0; i < plies; i++) { auto lm = legalMoves(pos); if (lm.empty()) break; makeMove(pos, lm[rng() % lm.size()]); }
        MoveList all; genMoves(pos, all, GEN_ALL);
        set<Move> A(all.m, all.m + all.n);
        for (int i = 0; i < all.n; i++) if (universe.size() < 20000) universe.push_back(all.m[i]);
        // caps + quiets == all
        MoveList c, q; genMoves(pos, c, GEN_CAPS); genMoves(pos, q, GEN_QUIETS);
        multiset<Move> U(c.m, c.m + c.n); U.insert(q.m, q.m + q.n);
        if (U != multiset<Move>(all.m, all.m + all.n)) { errors++; cerr << "split mismatch " << toFen(pos) << endl; }
        // pseudoLegal vs membership
        for (int k = 0; k < 300; k++) {
            Move m = k < (int)universe.size() ? universe[rng() % universe.size()] : (Move)(rng() & 0x3FFFF);
            if (rng() % 4 == 0) m = (Move)(rng() & 0x3FFFF);
            bool want = A.count(m) > 0;
            if (pseudoLegal(pos, m) != want) { errors++; cerr << "pseudoLegal " << moveStr(m) << " flag " << mFlag(m) << " want " << want << " " << toFen(pos) << endl; }
            checked++;
        }
        // legalMove vs make
        ChkInfo ci; computeChk(pos, ci);
        for (int i = 0; i < all.n; i++) {
            Position cc = pos; bool want = makeMove(cc, all.m[i]);
            if (legalMove(pos, all.m[i], ci) != want) { errors++; cerr << "legal " << moveStr(all.m[i]) << " " << toFen(pos) << endl; }
            checked++;
        }
        // picker yields exactly all moves once (random tt / killers / counter)
        auto rnd = [&]() -> Move { return universe.empty() ? 0 : universe[rng() % universe.size()]; };
        Move tt = rng() % 2 ? (all.n ? all.m[rng() % all.n] : 0) : rnd();
        killers[5][0] = rnd(); killers[5][1] = rng() % 3 ? rnd() : killers[5][0];
        TD->stk[4].piece = rng() % 12; TD->stk[4].to = rng() % 64; TD->stk[3].piece = -1;
        counterMove[TD->stk[4].piece][TD->stk[4].to] = rnd();
        MovePicker mp(pos, tt, 5);
        multiset<Move> got; Move m;
        while ((m = mp.next())) got.insert(m);
        if (got != multiset<Move>(all.m, all.m + all.n)) { errors++; cerr << "picker mismatch " << got.size() << " vs " << all.n << " " << toFen(pos) << endl; }
        checked++;
    }
    cout << "selftest: " << checked << " checks, " << errors << " errors" << endl;
    return errors ? 1 : 0;
}

// ===================== UCI =====================
SearchShared uci;
Position rootPos;
U64 gameKeys[4096]; int gameLen = 0;

Move parseMove(const Position &p, const string &s) {
    MoveList ml; genMoves(p, ml, GEN_ALL);
    for (int i = 0; i < ml.n; i++) if (moveStr(ml.m[i]) == s) return ml.m[i];
    return 0;
}

void handlePosition(istringstream &ss) {
    string tok, fen;
    ss >> tok;
    if (tok == "startpos") { fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"; ss >> tok; }
    else if (tok == "fen") { while (ss >> tok && tok != "moves") fen += tok + " "; }
    rootPos.setFen(fen);
    gameLen = 0; gameKeys[gameLen++] = rootPos.key;
    if (tok == "moves") {
        while (ss >> tok) {
            Move m = parseMove(rootPos, tok);
            if (!m) break;
            makeMove(rootPos, m);
            gameKeys[gameLen++] = rootPos.key;
        }
    }
}

thread searchThread;
long long moveOverhead = 50; int timeHorizon = 32;   // assumed moves still to play (UCI option 'Time Horizon')
//    // ms reserved per move for GUI / network latency
void stopSearch() { uci.stop = true; if (searchThread.joinable()) searchThread.join(); }

void handleGo(istringstream &ss) {
    stopSearch();
    string tok;
    long long wtime = -1, btime = -1, winc = 0, binc = 0, movetime = -1;
    int depth = MAXPLY - 1, movestogo = 0;
    long long nodeLim = 0; bool softMode = false;
    bool infinite = false;
    while (ss >> tok) {
        if (tok == "wtime") ss >> wtime; else if (tok == "btime") ss >> btime;
        else if (tok == "winc") ss >> winc; else if (tok == "binc") ss >> binc;
        else if (tok == "movetime") ss >> movetime; else if (tok == "depth") ss >> depth;
        else if (tok == "movestogo") ss >> movestogo; else if (tok == "infinite") infinite = true;
        else if (tok == "nodes") ss >> nodeLim;
        else if (tok == "softnodes") { ss >> nodeLim; softMode = true; }   // datagen limits: soft N, hard 8N
    }
    uci.softLimitMs = uci.hardLimitMs = 0;
    uci.softNodeLimit = uci.hardNodeLimit = nodeLim > 0 ? (U64)nodeLim : 0;   // fixed-node search (fast, low-noise testing)
    if (softMode) { uci.softNodeLimit = nodeLim; uci.hardNodeLimit = nodeLim * 8; }
    long long t = rootPos.side == WHITE ? wtime : btime, inc = rootPos.side == WHITE ? winc : binc;
    if (movetime > 0) { uci.softLimitMs = uci.hardLimitMs = max(1LL, movetime - moveOverhead * 2 / 5); }
    else if (t >= 0 && !infinite) {
        long long avail = max(10LL, t - moveOverhead);
        int mtg = movestogo > 0 ? min(movestogo, timeHorizon) : timeHorizon;
        int moveNo = gameLen / 2;   // spend less in the opening, full share from move ~14
        double phase = moveNo < 6 ? 0.6 : moveNo < 14 ? 0.6 + 0.4 * (moveNo - 6) / 8.0 : 1.0;
        uci.softLimitMs = max(5LL, (long long)((avail / mtg + inc * 3 / 4) * phase));
        uci.hardLimitMs = max(10LL, min(avail / 6, uci.softLimitMs * 3));
        uci.softLimitMs = min(uci.softLimitMs, uci.hardLimitMs);
    }
    setGameHistory(uci, gameKeys, gameLen);
    uci.stop = false;
    Position root = rootPos;
#ifdef NO_THREADS
    think(&uci, root, depth);   // WASM 등 스레드가 없는 환경: 탐색이 끝날 때까지 여기서 블록됨
#else
    searchThread = thread(think, &uci, root, depth);
#endif
}

void bench() {
    const char *fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    };
    U64 total = 0; auto t0 = chrono::steady_clock::now();
    for (auto f : fens) {
        rootPos.setFen(f);
        TD = uci.threads[0].get(); keyHist[0] = rootPos.key; histLen = 1; rebuildRep(TD);
        uci.softLimitMs = uci.hardLimitMs = 0; uci.stop = false;
        think(&uci, rootPos, 10); total += threadNodes(uci.threads[0].get());
    }
    double s = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
    cout << "bench nodes " << total << " nps " << (U64)(total / s) << endl;
}

// Handles one UCI command line. Returns false on 'quit'.
bool uciLine(const string &line) {
    istringstream ss(line);
    string cmd; ss >> cmd;
    if (cmd == "uci") {
        lock_guard<mutex> lk(ioMtx);
        cout << "id name Lunaris 1.2.1\nid author Eric\n"
             << "option name Hash type spin default 64 min 1 max 4096\n"
             << "option name Threads type spin default 1 min 1 max 256\n"
             << "option name Move Overhead type spin default 50 min 0 max 5000\n"
             << "option name Time Horizon type spin default 32 min 20 max 100\n"
             << "option name EvalFile type string default <embedded>\nuciok" << endl;
    }
    else if (cmd == "isready") uciOut("readyok");
    else if (cmd == "ucinewgame") { stopSearch(); uci.tt.clear(); clearHistories(uci); }
    else if (cmd == "setoption") {
        string t, name, value; ss >> t;
        while (ss >> t && t != "value") name += (name.empty() ? "" : " ") + t;
        getline(ss, value); value.erase(0, value.find_first_not_of(' '));
        for (char &ch : name) ch = (char)tolower(ch);
        if (value.empty()) return true;
        if (name == "hash") { stopSearch(); uci.tt.resize(max(1, stoi(value))); }
        else if (name == "threads") { stopSearch(); initThreads(uci, max(1, min(256, stoi(value)))); }
        else if (name == "move overhead") moveOverhead = max(0, min(5000, stoi(value)));
        else if (name == "time horizon") timeHorizon = max(20, min(100, stoi(value)));
        else if (name == "evalfile") {
            stopSearch();
            if (value == "<hce>") useNNUE = false;
            else if (!loadNetFile(value)) uciOut("info string failed to load " + value);
            clearEvalCaches(uci);
        }
    }
    else if (cmd == "position") { stopSearch(); handlePosition(ss); }
    else if (cmd == "go") {
        string t; ss >> t;
        if (t == "perft") { int d; ss >> d; perftDivide(rootPos, d); }
        else { istringstream ss2(line.substr(2)); handleGo(ss2); }
    }
    else if (cmd == "stop") stopSearch();
    else if (cmd == "see") { string mv; ss >> mv; Move m = parseMove(rootPos, mv); cout << "see>=0 " << see(rootPos, m, 0) << " see>=100 " << see(rootPos, m, 100) << endl; }
    else if (cmd == "d") uciOut("Fen: " + toFen(rootPos));
    else if (cmd == "eval") uciOut("eval " + to_string(evaluateStatic(rootPos)) + " (side to move)");
    else if (cmd == "quit") { stopSearch(); return false; }
    return true;
}

int main(int argc, char **argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);   // reading a command must not flush cout while the search thread writes to it
    initTables(); initEval();
    const bool genMode = argc > 1 && (string(argv[1]) == "gen" || string(argv[1]) == "datagen");
    if (!genMode) { uci.tt.resize(64); initThreads(uci, 1); }   // datagen workers own their tables
#ifdef HAS_EMBEDDED_NET
    if (!loadNetBytes(NET_DATA, sizeof NET_DATA)) cerr << "embedded net has wrong size, using HCE" << endl;
#endif
    for (int d = 0; d < 64; d++) for (int m = 0; m < 64; m++)
        lmrTab[d][m] = (d && m) ? (int)(0.75 + log(d) * log(m) / 2.25) : 0;
    rootPos.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    gameKeys[0] = rootPos.key; gameLen = 1;
    if (argc > 1 && string(argv[1]) == "bench") { bench(); return 0; }
    if (argc > 1 && string(argv[1]) == "selftest") return selftest(argc > 2 ? atoi(argv[2]) : 20000);
    if (argc > 1 && string(argv[1]) == "datagen") {
        // (legacy, single thread) lunaris datagen <games> <nodes> <out.txt> <seed>
        int games = argc > 2 ? atoi(argv[2]) : 1000;
        U64 nodesPer = argc > 3 ? strtoull(argv[3], 0, 10) : 5000;
        string out = argc > 4 ? argv[4] : "data.txt";
        uint64_t seed = argc > 5 ? strtoull(argv[5], 0, 10) : 1;
        signal(SIGINT, onSignal); signal(SIGTERM, onSignal);
        datagenWorker(games, nodesPer, out, seed, true);
        return 0;
    }
    if (argc > 1 && string(argv[1]) == "gen") {
        // lunaris gen <threads> <seedStart> <gamesPerThread> [nodes=5000] [outDir=data] [--force]
        vector<string> a; bool force = false;
        for (int i = 2; i < argc; i++) { string s = argv[i]; if (s == "--force") force = true; else a.push_back(s); }
        if (a.size() < 3) { cerr << "usage: lunaris gen <threads> <seedStart> <gamesPerThread> [nodes=5000] [outDir=data] [--force]" << endl; return 2; }
        int threads = max(1, atoi(a[0].c_str()));
        uint64_t seed = strtoull(a[1].c_str(), 0, 10);
        int games = atoi(a[2].c_str());
        U64 nodesPer = a.size() > 3 ? strtoull(a[3].c_str(), 0, 10) : 5000;
        string out = a.size() > 4 ? a[4] : "data";
        return datagenMulti(threads, seed, games, nodesPer, out, force);
    }

    string line;
    while (getline(cin, line)) {
        if (!uciLine(line)) break;
    }
    stopSearch();
    return 0;
}
