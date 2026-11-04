// shared poker helpers: classes, 7-card evaluator, per-thread RNG
#pragma once
#include <stdint.h>
#include <string.h>

#define NC 169
static int ncombo[NC];
static int combo[NC][12][2];

typedef struct { uint64_t s[2]; } Rng;
static inline uint64_t rng_next(Rng *r) {
    uint64_t s1 = r->s[0]; const uint64_t s0 = r->s[1];
    r->s[0] = s0; s1 ^= s1 << 23; r->s[1] = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
    return r->s[1] + s0;
}
static inline int rng_int(Rng *r, int n) { return (int)((rng_next(r) >> 11) % (uint64_t)n); }
static inline void rng_seed(Rng *r, uint64_t seed) {
    r->s[0] = seed * 0x9E3779B97F4A7C15ULL + 1; r->s[1] = (seed ^ 0xD1B54A32D192ED03ULL) * 0xBF58476D1CE4E5B9ULL + 7;
    for (int i = 0; i < 20; i++) rng_next(r);
}

static inline int straight_high(int mask) {
    int ext = (mask << 1) | ((mask >> 12) & 1);
    for (int h = 13; h >= 4; h--) { int need = 0x1F << (h - 4); if ((ext & need) == need) return h - 1; }
    return -1;
}
static uint32_t eval7(const int *c) {
    int sm[4] = {0}, cnt[13] = {0}, sc[4] = {0}, all = 0;
    for (int i = 0; i < 7; i++) { int r = c[i] >> 2, s = c[i] & 3; sm[s] |= 1 << r; cnt[r]++; sc[s]++; all |= 1 << r; }
    for (int s = 0; s < 4; s++) if (sc[s] >= 5) {
        int sh = straight_high(sm[s]);
        if (sh >= 0) return (8u << 20) | sh;
        uint32_t v = 0; int k = 0;
        for (int r = 12; r >= 0 && k < 5; r--) if (sm[s] >> r & 1) { v = (v << 4) | r; k++; }
        return (5u << 20) | v;
    }
    int quad = -1, tr[2] = {-1, -1}, nt = 0, pr[3] = {-1, -1, -1}, np = 0;
    for (int r = 12; r >= 0; r--) {
        if (cnt[r] == 4) quad = r;
        else if (cnt[r] == 3) { if (nt < 2) tr[nt++] = r; }
        else if (cnt[r] == 2) { if (np < 3) pr[np++] = r; }
    }
    if (quad >= 0) { int k = 0; for (int r = 12; r >= 0; r--) if (r != quad && cnt[r]) { k = r; break; } return (7u << 20) | (quad << 4) | k; }
    if (nt >= 1 && (nt >= 2 || np >= 1)) {
        int p = -1; if (nt >= 2) p = tr[1]; if (np >= 1 && pr[0] > p) p = pr[0];
        return (6u << 20) | (tr[0] << 4) | p;
    }
    int sh = straight_high(all); if (sh >= 0) return (4u << 20) | sh;
    if (nt == 1) { uint32_t v = tr[0]; int k = 0; for (int r = 12; r >= 0 && k < 2; r--) if (cnt[r] == 1) { v = (v << 4) | r; k++; } return (3u << 20) | v; }
    if (np >= 2) { int k = 0; for (int r = 12; r >= 0; r--) if (cnt[r] >= 1 && r != pr[0] && r != pr[1]) { k = r; break; } return (2u << 20) | (pr[0] << 8) | (pr[1] << 4) | k; }
    if (np == 1) { uint32_t v = pr[0]; int k = 0; for (int r = 12; r >= 0 && k < 3; r--) if (cnt[r] == 1) { v = (v << 4) | r; k++; } return (1u << 20) | v; }
    uint32_t v = 0; int k = 0; for (int r = 12; r >= 0 && k < 5; r--) if (cnt[r]) { v = (v << 4) | r; k++; } return v;
}
static void build_classes(void) {
    for (int g = 0; g < NC; g++) {
        int r = g / 13, c = g % 13, n = 0;
        if (r == c) { int rk = 12 - r; for (int s1 = 0; s1 < 4; s1++) for (int s2 = s1 + 1; s2 < 4; s2++) { combo[g][n][0] = rk * 4 + s1; combo[g][n][1] = rk * 4 + s2; n++; } }
        else if (r < c) { int a = 12 - r, b = 12 - c; for (int s = 0; s < 4; s++) { combo[g][n][0] = a * 4 + s; combo[g][n][1] = b * 4 + s; n++; } }
        else { int a = 12 - c, b = 12 - r; for (int s1 = 0; s1 < 4; s1++) for (int s2 = 0; s2 < 4; s2++) if (s1 != s2) { combo[g][n][0] = a * 4 + s1; combo[g][n][1] = b * 4 + s2; n++; } }
        ncombo[g] = n;
    }
}
static inline int conflict2(const int *a, const int *b) { return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1]; }
// order encoding: perms of players (best, mid, worst)
static const int PERM[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
static inline int perm_index(int a, int b) { // best a, mid b
    for (int o = 0; o < 6; o++) if (PERM[o][0] == a && PERM[o][1] == b) return o;
    return -1;
}
// canonical triple index for i<=j<=k
static inline long tri_index(int i, int j, int k) {
    // number of triples with first < i, etc. use precomputed offsets
    extern long TOFF[NC][NC];
    return TOFF[i][j] + (k - j);
}
