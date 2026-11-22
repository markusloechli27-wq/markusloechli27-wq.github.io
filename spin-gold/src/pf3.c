// pf3.c — three-player flop solver for spin pots (Discounted CFR, vectorised over hands).
// Players in postflop order: 0 acts first (SB), 1 (BB), 2 last (BTN). Equal stacks behind, pot P0 at the flop.
// Betting on the flop: check / bet (pot fractions, SIZES) / all-in; facing a bet: fold / call / all-in. A bet and one
// raise per player round (the raise is always all-in). When the flop betting ends with two or three players, turn and
// river come without more betting: the pot goes to the best hand, averaged exactly over every turn-river runout.
// Card removal: each hand against each opponent exact; between the two opponents it is taken over the whole board
// (pairs that overlap each other and also touch the hero's cards are subtracted once too often: under 1 % of the mass).
// usage: ./pf3 ranges.json key0 key1 key2 pot stack flops.txt nflops part parts > out.jsonl
//   env: SIZES (pot fractions, default "0.33"), MAXIT (default 300), ADD_ALLIN (all-in allowed when the stack is at most
//   this many pots, default 9: in every solved 3-way spot), PF3_TEST (print values for a brute-force check and exit)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

// ---------------- hand evaluation (7 cards), as pfsolve.c ----------------
static int top_bit(uint32_t m) { return 31 - __builtin_clz(m); }
static uint32_t straight_hi(uint32_t m) { uint32_t e = (m << 1) | ((m >> 12) & 1), s = e & (e >> 1) & (e >> 2) & (e >> 3) & (e >> 4); return s ? (uint32_t)top_bit(s) + 4 : 0; }
static uint32_t kick(uint32_t m, int k, uint32_t acc) { for (int i = 0; i < k; i++) { int b = top_bit(m); acc = (acc << 4) | (uint32_t)b; m &= ~(1u << b); } return acc; }
static uint32_t eval7(const uint8_t *c) {
  uint32_t sm[4] = {0, 0, 0, 0}; int cnt[13] = {0};
  for (int i = 0; i < 7; i++) { sm[c[i] & 3] |= 1u << (c[i] >> 2); cnt[c[i] >> 2]++; }
  for (int s = 0; s < 4; s++) if (__builtin_popcount(sm[s]) >= 5) { uint32_t st = straight_hi(sm[s]); if (st) return (8u << 20) | st; return (5u << 20) | kick(sm[s], 5, 0); }
  uint32_t all = sm[0] | sm[1] | sm[2] | sm[3]; int q = -1, t1 = -1, t2 = -1, p1 = -1, p2 = -1;
  for (int r = 12; r >= 0; r--) { int n = cnt[r]; if (n == 4) q = r; else if (n == 3) { if (t1 < 0) t1 = r; else if (t2 < 0) t2 = r; } else if (n == 2) { if (p1 < 0) p1 = r; else if (p2 < 0) p2 = r; } }
  if (q >= 0) return (7u << 20) | ((uint32_t)q << 4) | (uint32_t)top_bit(all & ~(1u << q));
  if (t1 >= 0 && (t2 >= 0 || p1 >= 0)) { int pr = t2 > p1 ? t2 : p1; return (6u << 20) | ((uint32_t)t1 << 4) | (uint32_t)pr; }
  uint32_t st = straight_hi(all); if (st) return (4u << 20) | st;
  if (t1 >= 0) return (3u << 20) | kick(all & ~(1u << t1), 2, (uint32_t)t1);
  if (p1 >= 0 && p2 >= 0) return (2u << 20) | ((uint32_t)p1 << 8) | ((uint32_t)p2 << 4) | (uint32_t)top_bit(all & ~(1u << p1) & ~(1u << p2));
  if (p1 >= 0) return (1u << 20) | kick(all & ~(1u << p1), 3, (uint32_t)p1);
  return kick(all, 5, 0);
}
static int combo_index(int a, int b) { if (a > b) { int t = a; a = b; b = t; } return b * (b - 1) / 2 + a; }
static int card_of(const char *s) { const char *R = "23456789TJQKA", *S = "cdhs"; const char *r = strchr(R, s[0]), *u = strchr(S, s[1]); return r && u ? (int)(r - R) * 4 + (int)(u - S) : -1; }
static int class_of(int a, int b) { int ra = 12 - (a >> 2), rb = 12 - (b >> 2); if (ra > rb) { int t = ra; ra = rb; rb = t; } if (ra == rb) return ra * 13 + ra; return ((a & 3) == (b & 3)) ? ra * 13 + rb : rb * 13 + ra; }

// ---------------- game ----------------
enum { A_FOLD, A_CHECK, A_CALL, A_BET, A_ALLIN };
static const char *ANM[] = {"fold", "check", "call", "bet", "allin"};
enum { N_ACT, N_FOLD, N_SHOW };
typedef struct { uint8_t type, player, nact, acttype, in; int32_t child; size_t off; float amt, c[3]; char name[64]; } Node;
static Node *ND; static int NN, NCAP; static size_t NF;
static float P0, STK, SZ[4], ADD_ALLIN = 9, ALLIN_THR = .67f; static int NSZ = 1;
static uint8_t BOARD[3];
static int NH[3]; static uint8_t *HC[3]; static float *W[3]; static int16_t *CI[3];   // hands: cards, range weight, combo index
static int16_t POS[3][1326];                                                         // combo -> hand index or -1
static float *REG, *SUM;

typedef struct { float b[3]; uint8_t in, ai, acted; int p, nbets; char name[64]; } St;
static int new_node(void) { if (NN == NCAP) { NCAP = NCAP ? NCAP * 2 : 256; ND = realloc(ND, sizeof(Node) * NCAP); } memset(&ND[NN], 0, sizeof(Node)); return NN++; }
static float maxb(const St *s) { float m = 0; for (int q = 0; q < 3; q++) if (s->b[q] > m) m = s->b[q]; return m; }
static float potof(const St *s) { return P0 + s->b[0] + s->b[1] + s->b[2]; }
static int ncount(uint8_t m) { return __builtin_popcount(m); }
static void build(int ni, St s);
// after an action: one player left, the round closed (showdown), or the next player to act
static void advance(int ni, St s) {
  Node *n = &ND[ni]; memcpy(n->c, s.b, sizeof s.b); n->in = s.in; strcpy(n->name, s.name);
  if (ncount(s.in) == 1) { n->type = N_FOLD; return; }
  float m = maxb(&s); int nxt = -1;
  for (int k = 1; k <= 3; k++) { int q = (s.p + k) % 3; if (!(s.in >> q & 1) || (s.ai >> q & 1)) continue;
    if (s.b[q] < m - 1e-5f || !(s.acted >> q & 1)) { nxt = q; break; } }
  // nobody left to act, or everyone but one is all-in and that one has matched: showdown after the runout
  if (nxt < 0) { n->type = N_SHOW; return; }
  int canAct = 0; for (int q = 0; q < 3; q++) if ((s.in >> q & 1) && !(s.ai >> q & 1)) canAct++;
  if (canAct == 1 && s.b[nxt] >= m - 1e-5f) { n->type = N_SHOW; return; }
  s.p = nxt; build(ni, s);
}
typedef struct { int type; float to; } Act;
static void build(int ni, St s) {
  int p = s.p; float m = maxb(&s), tocall = m - s.b[p], left = STK - s.b[p], pot = potof(&s);
  Act a[8]; int na = 0;
  if (tocall <= 1e-5f) {
    a[na++] = (Act){A_CHECK, s.b[p]};
    if (s.nbets == 0) {
      for (int i = 0; i < NSZ; i++) { float amt = SZ[i] * pot; if (amt < 1) amt = 1; if (amt >= ALLIN_THR * left) a[na++] = (Act){A_ALLIN, STK}; else a[na++] = (Act){A_BET, s.b[p] + amt}; }
      if (left <= ADD_ALLIN * pot) a[na++] = (Act){A_ALLIN, STK};
    }
  } else {
    a[na++] = (Act){A_FOLD, s.b[p]};
    a[na++] = (Act){A_CALL, left <= tocall + 1e-5f ? STK : m};
    if (left > tocall + 1e-5f && s.nbets < 2) a[na++] = (Act){A_ALLIN, STK};
  }
  int k2 = 0; for (int i = 0; i < na; i++) { int dup = 0; for (int j = 0; j < k2; j++) if (a[j].type == a[i].type && fabsf(a[j].to - a[i].to) < .01f) dup = 1; if (!dup) a[k2++] = a[i]; } na = k2;
  Node *n = &ND[ni]; n->type = N_ACT; n->player = (uint8_t)p; n->nact = (uint8_t)na; memcpy(n->c, s.b, sizeof s.b); n->in = s.in; strcpy(n->name, s.name);
  n->off = NF; NF += (size_t)na * NH[p];
  int first = NN; for (int k = 0; k < na; k++) new_node(); ND[ni].child = first;
  for (int k = 0; k < na; k++) {
    St t = s; int ci = first + k; ND[ci].acttype = (uint8_t)a[k].type; ND[ci].amt = a[k].to;
    char tok[8]; if (a[k].type == A_BET) snprintf(tok, sizeof tok, "b%d", (int)lroundf((a[k].to - s.b[p]) / pot * 100)); else snprintf(tok, sizeof tok, "%c", "fxcba"[a[k].type]);
    if (a[k].type == A_BET) tok[0] = 'b';
    snprintf(t.name, sizeof t.name, "%s%s%s", s.name, s.name[0] ? "-" : "", tok);
    switch (a[k].type) {
      case A_FOLD: t.in &= ~(1 << p); break;
      case A_CHECK: t.acted |= 1 << p; break;
      case A_CALL: t.b[p] = a[k].to; t.acted |= 1 << p; if (t.b[p] >= STK - 1e-5f) t.ai |= 1 << p; break;
      default: t.b[p] = a[k].to; t.nbets++; t.acted = 1 << p; if (t.b[p] >= STK - 1e-5f) t.ai |= 1 << p; break;
    }
    advance(ci, t);
  }
}

// ---------------- runouts ----------------
// every turn-river pair (no betting between them): per player the hands that do not touch it, sorted by strength
static int NR; static uint8_t (*RUN)[2]; static uint32_t *RSTR[3]; static uint16_t *RIDX[3]; static int *ROFF[3], *RN[3];
static void make_runouts(void) {
  NR = 0; RUN = malloc(sizeof *RUN * 1176);
  for (int t = 0; t < 52; t++) for (int r = t + 1; r < 52; r++) { int on = 0; for (int i = 0; i < 3; i++) if (BOARD[i] == t || BOARD[i] == r) on = 1; if (!on) { RUN[NR][0] = (uint8_t)t; RUN[NR][1] = (uint8_t)r; NR++; } }
  for (int p = 0; p < 3; p++) {
    ROFF[p] = malloc(sizeof(int) * NR); RN[p] = malloc(sizeof(int) * NR); RSTR[p] = malloc(sizeof(uint32_t) * (size_t)NR * NH[p]); RIDX[p] = malloc(sizeof(uint16_t) * (size_t)NR * NH[p]);
    size_t off = 0;
    for (int k = 0; k < NR; k++) {
      uint8_t c7[7] = {BOARD[0], BOARD[1], BOARD[2], RUN[k][0], RUN[k][1], 0, 0}; int n = 0; uint32_t *st = RSTR[p] + off; uint16_t *ix = RIDX[p] + off;
      for (int h = 0; h < NH[p]; h++) { int a = HC[p][2 * h], b = HC[p][2 * h + 1]; if (a == RUN[k][0] || a == RUN[k][1] || b == RUN[k][0] || b == RUN[k][1]) continue;
        c7[5] = (uint8_t)a; c7[6] = (uint8_t)b; uint32_t v = eval7(c7); int j = n++; while (j > 0 && st[j - 1] > v) { st[j] = st[j - 1]; ix[j] = ix[j - 1]; j--; } st[j] = v; ix[j] = (uint16_t)h; }
      ROFF[p][k] = (int)off; RN[p][k] = n; off += n;
    }
  }
}

// ---------------- terminal values ----------------
// joint mass of the two opponents' hands compatible with the hero's hand and with each other (flop level)
static void joint_mass(int A, const float *ra, int B, const float *rb, int t, float *out) {
  double TA = 0, TB = 0, CA[52] = {0}, CB[52] = {0}, OV = 0; static float zA[1326], zB[1326]; memset(zA, 0, sizeof zA); memset(zB, 0, sizeof zB);
  for (int g = 0; g < NH[A]; g++) { float v = ra[g]; if (v == 0) continue; TA += v; CA[HC[A][2 * g]] += v; CA[HC[A][2 * g + 1]] += v; zA[CI[A][g]] = v; }
  for (int g = 0; g < NH[B]; g++) { float v = rb[g]; if (v == 0) continue; TB += v; CB[HC[B][2 * g]] += v; CB[HC[B][2 * g + 1]] += v; zB[CI[B][g]] = v; }
  for (int c = 0; c < 52; c++) OV += CA[c] * CB[c]; for (int z = 0; z < 1326; z++) OV -= (double)zA[z] * zB[z];
  for (int h = 0; h < NH[t]; h++) { int a = HC[t][2 * h], b = HC[t][2 * h + 1], z = CI[t][h];
    double ta = TA - CA[a] - CA[b] + zA[z], tb = TB - CB[a] - CB[b] + zB[z]; double j = ta * tb - OV; out[h] = (float)(j > 0 ? j : 0); }
}
static float *ACC;   // per hero hand: sum over runouts of its pot share
static float LA[1326], EA[1326], LB[1326], EB[1326], ZB[1326];
// pot share of each hero hand at a showdown with both opponents in (sum over runouts, not yet divided)
static void share3(int t, int A, const float *ra, int B, const float *rb, float *acc) {
  for (int h = 0; h < NH[t]; h++) acc[h] = 0;
  for (int k = 0; k < NR; k++) {
    const uint32_t *st = RSTR[t] + ROFF[t][k], *sa = RSTR[A] + ROFF[A][k], *sb = RSTR[B] + ROFF[B][k];
    const uint16_t *it = RIDX[t] + ROFF[t][k], *ia = RIDX[A] + ROFF[A][k], *ib = RIDX[B] + ROFF[B][k];
    int nt = RN[t][k], na = RN[A][k], nb = RN[B][k];
    double TLA = 0, TEA = 0, TLB = 0, TEB = 0, OLL = 0, OLE = 0, OEL = 0, OEE = 0;
    double CLA[52] = {0}, CEA[52] = {0}, CLB[52] = {0}, CEB[52] = {0};
    int al = 0, ae = 0, bl = 0, be = 0;
    for (int i = 0; i < nt; ) {
      uint32_t s = st[i];
      for (; al < na && sa[al] < s; al++) { int g = ia[al]; float v = ra[g]; if (v == 0) continue; int p = HC[A][2 * g], q = HC[A][2 * g + 1], z = CI[A][g];
        OLL += v * (CLB[p] + CLB[q] - LB[z]); OLE += v * (CEB[p] + CEB[q] - EB[z]); CLA[p] += v; CLA[q] += v; TLA += v; LA[z] = v; }
      for (; ae < na && sa[ae] <= s; ae++) { int g = ia[ae]; float v = ra[g]; if (v == 0) continue; int p = HC[A][2 * g], q = HC[A][2 * g + 1], z = CI[A][g];
        OEL += v * (CLB[p] + CLB[q] - LB[z]); OEE += v * (CEB[p] + CEB[q] - EB[z]); CEA[p] += v; CEA[q] += v; TEA += v; EA[z] = v; }
      for (; bl < nb && sb[bl] < s; bl++) { int g = ib[bl]; float v = rb[g]; if (v == 0) continue; int p = HC[B][2 * g], q = HC[B][2 * g + 1], z = CI[B][g];
        OLL += v * (CLA[p] + CLA[q] - LA[z]); OEL += v * (CEA[p] + CEA[q] - EA[z]); CLB[p] += v; CLB[q] += v; TLB += v; LB[z] = v; }
      for (; be < nb && sb[be] <= s; be++) { int g = ib[be]; float v = rb[g]; if (v == 0) continue; int p = HC[B][2 * g], q = HC[B][2 * g + 1], z = CI[B][g];
        OLE += v * (CLA[p] + CLA[q] - LA[z]); OEE += v * (CEA[p] + CEA[q] - EA[z]); CEB[p] += v; CEB[q] += v; TEB += v; EB[z] = v; }
      for (; i < nt && st[i] == s; i++) { int h = it[i], a = HC[t][2 * h], b = HC[t][2 * h + 1], z = CI[t][h];
        double la = TLA - CLA[a] - CLA[b] + LA[z], ea = TEA - CEA[a] - CEA[b] + EA[z], lb = TLB - CLB[a] - CLB[b] + LB[z], eb = TEB - CEB[a] - CEB[b] + EB[z];
        double jll = la * lb - OLL, jle = la * eb - OLE, jel = ea * lb - OEL, jee = ea * eb - OEE;
        double sh = (jll + jee) * (1.0 / 3) + (jle + jel) * (1.0 / 6); if (sh > 0) acc[h] += (float)sh; }
    }
    for (int j = 0; j < na; j++) { int z = CI[A][ia[j]]; LA[z] = EA[z] = 0; }
    for (int j = 0; j < nb; j++) { int z = CI[B][ib[j]]; LB[z] = EB[z] = 0; }
  }
}
// pot share with one opponent A in (B folded: his hand only blocks cards)
static void share2(int t, int A, const float *ra, int B, const float *rb, float *acc) {
  for (int h = 0; h < NH[t]; h++) acc[h] = 0;
  for (int k = 0; k < NR; k++) {
    const uint32_t *st = RSTR[t] + ROFF[t][k], *sa = RSTR[A] + ROFF[A][k];
    const uint16_t *it = RIDX[t] + ROFF[t][k], *ia = RIDX[A] + ROFF[A][k], *ib = RIDX[B] + ROFF[B][k];
    int nt = RN[t][k], na = RN[A][k], nb = RN[B][k];
    double TB = 0, CB[52] = {0}, TLA = 0, TEA = 0, CLA[52] = {0}, CEA[52] = {0}, OL = 0, OE = 0;
    for (int j = 0; j < nb; j++) { int g = ib[j]; float v = rb[g]; if (v == 0) continue; TB += v; CB[HC[B][2 * g]] += v; CB[HC[B][2 * g + 1]] += v; ZB[CI[B][g]] = v; }
    int al = 0, ae = 0;
    for (int i = 0; i < nt; ) {
      uint32_t s = st[i];
      for (; al < na && sa[al] < s; al++) { int g = ia[al]; float v = ra[g]; if (v == 0) continue; int p = HC[A][2 * g], q = HC[A][2 * g + 1], z = CI[A][g];
        OL += v * (CB[p] + CB[q] - ZB[z]); CLA[p] += v; CLA[q] += v; TLA += v; LA[z] = v; }
      for (; ae < na && sa[ae] <= s; ae++) { int g = ia[ae]; float v = ra[g]; if (v == 0) continue; int p = HC[A][2 * g], q = HC[A][2 * g + 1], z = CI[A][g];
        OE += v * (CB[p] + CB[q] - ZB[z]); CEA[p] += v; CEA[q] += v; TEA += v; EA[z] = v; }
      for (; i < nt && st[i] == s; i++) { int h = it[i], a = HC[t][2 * h], b = HC[t][2 * h + 1], z = CI[t][h];
        double la = TLA - CLA[a] - CLA[b] + LA[z], ea = TEA - CEA[a] - CEA[b] + EA[z], tb = TB - CB[a] - CB[b] + ZB[z];
        double sh = .5 * ((la * tb - OL) + (ea * tb - OE)); if (sh > 0) acc[h] += (float)sh; }
    }
    for (int j = 0; j < na; j++) { int z = CI[A][ia[j]]; LA[z] = EA[z] = 0; }
    for (int j = 0; j < nb; j++) ZB[CI[B][ib[j]]] = 0;
  }
}
static const double RUNDIV = 903.0;   // runouts that avoid six given cards: C(43, 2)

// ---------------- CFR ----------------
static float *SCR; static size_t SCR_TOP, SCR_CAP;
static float *scr(size_t n) { float *p = SCR + SCR_TOP; SCR_TOP += (n + 3) & ~(size_t)3; if (SCR_TOP > SCR_CAP) { fprintf(stderr, "scratch\n"); exit(2); } return p; }
static int ITER; static double DPOS, DNEG, GAM;
// r[q]: reach of player q (the hero's own reach weights his strategy sums). mode 0: CFR update; 1: best response; 2: average
static void walk(int ni, int t, float **r, float *out, int mode) {
  const Node *n = &ND[ni]; int Ht = NH[t], A = (t + 1) % 3, B = (t + 2) % 3;
  size_t mark = SCR_TOP;
  if (n->type != N_ACT) {
    float *mass = scr(Ht); joint_mass(A, r[A], B, r[B], t, mass);
    float pot = P0 + n->c[0] + n->c[1] + n->c[2], ct = n->c[t];
    if (n->type == N_FOLD) { float v = (n->in >> t & 1) ? pot - ct : -ct; for (int h = 0; h < Ht; h++) out[h] = v * mass[h]; }
    else if (!(n->in >> t & 1)) { for (int h = 0; h < Ht; h++) out[h] = -ct * mass[h]; }
    else { float *sh = scr(Ht);
      if ((n->in >> A & 1) && (n->in >> B & 1)) share3(t, A, r[A], B, r[B], sh);
      else if (n->in >> A & 1) share2(t, A, r[A], B, r[B], sh); else share2(t, B, r[B], A, r[A], sh);
      for (int h = 0; h < Ht; h++) out[h] = (float)(pot * sh[h] / RUNDIV) - ct * mass[h]; }
    SCR_TOP = mark; return;
  }
  int p = n->player, na = n->nact, H = NH[p]; float *R = REG + n->off, *S = SUM + n->off, *sig = scr((size_t)na * H);
  for (int h = 0; h < H; h++) { double s = 0;
    if (mode == 0) { for (int a = 0; a < na; a++) if (R[a * H + h] > 0) s += R[a * H + h]; for (int a = 0; a < na; a++) sig[a * H + h] = s > 0 ? (R[a * H + h] > 0 ? (float)(R[a * H + h] / s) : 0) : 1.f / na; }
    else { for (int a = 0; a < na; a++) s += S[a * H + h]; for (int a = 0; a < na; a++) sig[a * H + h] = s > 0 ? (float)(S[a * H + h] / s) : 1.f / na; } }
  float *old = r[p], *r2 = scr(H);
  if (p == t) {
    float *cv = scr((size_t)na * H);
    for (int a = 0; a < na; a++) { for (int h = 0; h < H; h++) r2[h] = old[h] * sig[a * H + h]; r[p] = r2; walk(n->child + a, t, r, cv + (size_t)a * H, mode); }
    r[p] = old;
    if (mode == 1) for (int h = 0; h < H; h++) { float m = cv[h]; for (int a = 1; a < na; a++) if (cv[a * H + h] > m) m = cv[a * H + h]; out[h] = m; }
    else for (int h = 0; h < H; h++) { float v = 0; for (int a = 0; a < na; a++) v += sig[a * H + h] * cv[a * H + h]; out[h] = v; }
    if (mode == 0) for (int a = 0; a < na; a++) for (int h = 0; h < H; h++) { size_t i = (size_t)a * H + h; float x = R[i]; x *= x > 0 ? (float)DPOS : (float)DNEG; R[i] = x + cv[i] - out[h];
      S[i] = (float)(S[i] * GAM) + old[h] * sig[i]; }
  } else {
    float *tmp = scr(Ht); for (int h = 0; h < Ht; h++) out[h] = 0;
    for (int a = 0; a < na; a++) { float any = 0; for (int h = 0; h < H; h++) { r2[h] = old[h] * sig[a * H + h]; any += r2[h]; } if (any <= 0) continue;
      r[p] = r2; walk(n->child + a, t, r, tmp, mode); for (int h = 0; h < Ht; h++) out[h] += tmp[h]; }
    r[p] = old;
  }
  SCR_TOP = mark;
}
static void iterate(void) {
  ITER++; double x = ITER, xa = x * sqrt(x); DPOS = xa / (xa + 1); DNEG = .5; double q = x / (x + 1); GAM = q * q * q * q * q;
  for (int t = 0; t < 3; t++) { SCR_TOP = 0; float *out = scr(NH[t]); float *r[3] = {W[0], W[1], W[2]}; walk(0, t, r, out, 0); }
}
static double value(int t, int mode) {   // per hand of the range, in BB, averaged over the range
  SCR_TOP = 0; float *out = scr(NH[t]), *m = scr(NH[t]); float *r[3] = {W[0], W[1], W[2]}; walk(0, t, r, out, mode);
  joint_mass((t + 1) % 3, W[(t + 1) % 3], (t + 2) % 3, W[(t + 2) % 3], t, m);
  double v = 0, z = 0; for (int h = 0; h < NH[t]; h++) { v += (double)W[t][h] * out[h]; z += (double)W[t][h] * m[h]; } return z > 0 ? v / z : 0;
}

// ---------------- output ----------------
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(const unsigned char *p, size_t n) { for (size_t i = 0; i < n; i += 3) { unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
  putchar(B64[v >> 18 & 63]); putchar(B64[v >> 12 & 63]); putchar(i + 1 < n ? B64[v >> 6 & 63] : '='); putchar(i + 2 < n ? B64[v & 63] : '='); } }
static int NOUT;
static void out_nodes(int ni, float **r) {   // every decision: per hand class the average strategy, weighted by the actor's reach
  const Node *n = &ND[ni]; if (n->type != N_ACT) return;
  int p = n->player, na = n->nact, H = NH[p]; float *S = SUM + n->off;
  double acc[5][169] = {{0}}, tot[169] = {0}, all[5] = {0}, allw = 0; float *sig = malloc(sizeof(float) * na * H);
  for (int h = 0; h < H; h++) { double s = 0; for (int a = 0; a < na; a++) s += S[a * H + h]; for (int a = 0; a < na; a++) sig[a * H + h] = s > 0 ? (float)(S[a * H + h] / s) : 1.f / na;
    double w = r[p][h]; if (w <= 0) continue; int c = class_of(HC[p][2 * h], HC[p][2 * h + 1]); tot[c] += w; allw += w; for (int a = 0; a < na; a++) { acc[a][c] += w * sig[a * H + h]; all[a] += w * sig[a * H + h]; } }
  printf("%s\"%s\":{\"p\":%d,\"acts\":[", NOUT++ ? "," : "", n->name, p);
  for (int a = 0; a < na; a++) { const Node *c = &ND[n->child + a]; if (c->acttype == A_BET) printf("%s\"b%d\"", a ? "," : "", (int)lroundf((c->amt - n->c[p]) / (P0 + n->c[0] + n->c[1] + n->c[2]) * 100)); else printf("%s\"%s\"", a ? "," : "", ANM[c->acttype]); }
  printf("],\"tot\":["); for (int a = 0; a < na; a++) printf("%s%.1f", a ? "," : "", allw > 0 ? all[a] / allw * 100 : 0);
  printf("],\"reach\":%.4f,\"f\":[", allw);
  for (int a = 0; a < na; a++) { unsigned char b[169]; for (int c = 0; c < 169; c++) b[c] = (unsigned char)(tot[c] > 0 ? acc[a][c] / tot[c] * 100 + .5 : 0); printf("%s\"", a ? "," : ""); b64(b, 169); printf("\""); }
  printf("]}");
  for (int a = 0; a < na; a++) { float *r2 = malloc(sizeof(float) * H), *old = r[p]; for (int h = 0; h < H; h++) r2[h] = old[h] * sig[a * H + h]; r[p] = r2; out_nodes(n->child + a, r); r[p] = old; free(r2); }
  free(sig);
}

static int read_range(const char *json, const char *key, float *out) {
  char pat[64]; snprintf(pat, sizeof pat, "\"%s\":[", key); const char *p = strstr(json, pat); if (!p) return -1; p += strlen(pat);
  for (int i = 0; i < 169; i++) { out[i] = strtof(p, (char **)&p); while (*p == ',' || *p == ' ') p++; } return 0;
}
int main(int argc, char **argv) {
  if (argc < 11) { fprintf(stderr, "usage: pf3 ranges.json key0 key1 key2 pot stack flops.txt nflops part parts\n"); return 1; }
  FILE *f = fopen(argv[1], "r"); if (!f) { fprintf(stderr, "no file %s\n", argv[1]); return 1; } fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
  char *js = malloc(fsz + 1); size_t n = fread(js, 1, fsz, f); js[n] = 0; fclose(f);
  float R[3][169]; for (int p = 0; p < 3; p++) if (read_range(js, argv[2 + p], R[p])) { fprintf(stderr, "range key %s\n", argv[2 + p]); return 1; }
  P0 = atof(argv[5]); STK = atof(argv[6]); int nfl = atoi(argv[8]), part = atoi(argv[9]), parts = atoi(argv[10]);
  const char *sz = getenv("SIZES") ? getenv("SIZES") : "0.33"; NSZ = 0; while (*sz && NSZ < 4) { char *e; float v = strtof(sz, &e); if (e == sz) break; SZ[NSZ++] = v; sz = e; while (*sz == ',') sz++; }
  if (getenv("ADD_ALLIN")) ADD_ALLIN = atof(getenv("ADD_ALLIN"));
  int maxit = getenv("MAXIT") ? atoi(getenv("MAXIT")) : 300, MINIT = getenv("MINIT") ? atoi(getenv("MINIT")) : 100;   // at least 100 iterations: a few dozen already look converged by the measure
  FILE *fl = fopen(argv[7], "r"); char line[64]; int k = -1, done = 0;
  while (done < nfl && fgets(line, sizeof line, fl)) {
    char key[16]; int w; if (sscanf(line, "%15s %d", key, &w) < 1) continue; k++; if (k % parts != part) continue;
    for (int i = 0; i < 3; i++) BOARD[i] = (uint8_t)card_of(key + 2 * i);
    for (int p = 0; p < 3; p++) { int m = 0; HC[p] = malloc(2 * 1326); W[p] = malloc(sizeof(float) * 1326); CI[p] = malloc(sizeof(int16_t) * 1326); for (int i = 0; i < 1326; i++) POS[p][i] = -1;
      for (int b = 1; b < 52; b++) for (int a = 0; a < b; a++) { if (a == BOARD[0] || a == BOARD[1] || a == BOARD[2] || b == BOARD[0] || b == BOARD[1] || b == BOARD[2]) continue;
        float v = R[p][class_of(a, b)]; if (v <= 0) continue; HC[p][2 * m] = (uint8_t)a; HC[p][2 * m + 1] = (uint8_t)b; W[p][m] = v; CI[p][m] = (int16_t)combo_index(a, b); POS[p][CI[p][m]] = (int16_t)m; m++; }
      NH[p] = m; }
    NN = 0; NF = 0; St s; memset(&s, 0, sizeof s); s.in = 7; s.p = 0; new_node(); build(0, s);
    REG = calloc(NF, sizeof(float)); SUM = calloc(NF, sizeof(float));
    int hm = NH[0] > NH[1] ? NH[0] : NH[1]; if (NH[2] > hm) hm = NH[2]; SCR_CAP = (size_t)64 * 8 * (hm + 8); SCR = malloc(sizeof(float) * SCR_CAP);
    make_runouts(); ITER = 0;
    if (getenv("PF3_TEST")) {   // pot shares of player 0's hands at the 3-way and 2-way showdowns with the full ranges, for the brute-force check
      float *a3 = malloc(sizeof(float) * NH[0]), *a2 = malloc(sizeof(float) * NH[0]), *m = malloc(sizeof(float) * NH[0]);
      share3(0, 1, W[1], 2, W[2], a3); share2(0, 1, W[1], 2, W[2], a2); joint_mass(1, W[1], 2, W[2], 0, m);
      for (int h = 0; h < NH[0]; h++) printf("%d %d %.6f %.6f %.6f\n", HC[0][2 * h], HC[0][2 * h + 1], a3[h] / RUNDIV, a2[h] / RUNDIV, m[h]);
      return 0;
    }
    double ex = 0; int it = 0;
    while (it < maxit) { iterate(); it++; if (it % 25 == 0 || it == maxit) { double s = 0; for (int t = 0; t < 3; t++) s += value(t, 1) - value(t, 2); ex = s / 3 / P0 * 100; fprintf(stderr, "%s it %d expl %.2f%%\n", key, it, ex); if (it >= MINIT && ex < 0.3) break; } }
    printf("{\"flop\":\"%s\",\"w\":%d,\"iters\":%d,\"expl\":%.2f,\"ev\":[%.3f,%.3f,%.3f],\"nodes\":{", key, w, it, ex, value(0, 2), value(1, 2), value(2, 2));
    NOUT = 0; float *r[3] = {W[0], W[1], W[2]}; out_nodes(0, r); printf("}}\n"); fflush(stdout); done++;
    for (int p = 0; p < 3; p++) { free(HC[p]); free(W[p]); free(CI[p]); free(ROFF[p]); free(RN[p]); free(RSTR[p]); free(RIDX[p]); }
    free(REG); free(SUM); free(SCR); free(RUN);
  }
  return 0;
}
