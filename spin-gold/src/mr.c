// Spin & Gold early levels: preflop solver with fold / limp / minraise / all-in (3-handed and heads-up), ChipEV.
// Hands are the 169 classes; card removal through P(opponent class | own class); all-in equity from eq.bin / p3.bin.
// Pots that see a flop are valued by equity realization: share = equity * r / sum(equity * r) with r from position,
// stack-to-pot ratio, preflop initiative and hand type. Discounted CFR (alpha 1.5, beta 0, gamma 2), simultaneous updates.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <omp.h>
#include "pk.h"

long TOFF[NC][NC];
static double E[NC][NC], Ncomp[NC][NC], PJ[NC][NC];
static float *W[3];   // W[p][(a*NC+b)*NC+c] = P(player p finishes first) for classes a,b,c of players 0,1,2
#define WI(a,b,c) ((((size_t)(a)) * NC + (b)) * NC + (c))

enum { A_F, A_X, A_C, A_L, A_R2, A_R3, A_J };
static const char *ANAME[] = { "fold", "check", "call", "limp", "raise2", "raise3", "allin" };
#define MAXN 600
#define MAXA 4
typedef struct {
  int term, player, nact, act[MAXA], child[MAXA], parent;
  double inv[3]; int active[3], np, aggr; double pot, cur;
  char path[64];
  double *reg, *avg;   // [nact][NC]
} Node;
static Node N[MAXN]; static int NN;
static int NPL;          // 3 or 2
static double S;         // stack in BB
static int POSTORD[3];   // postflop order (first to act first)

typedef struct { double inv[3]; int active[3], acted[3]; double cur; int aggr, limpers, jam; char path[64]; } St;
static int next_to_act(const St *s, int last) {
  for (int d = 1; d <= NPL; d++) { int p = (last + d) % NPL; if (!s->active[p]) continue; if (s->inv[p] >= S - 1e-9 && s->jam) continue; // all-in
    if (!s->acted[p] || s->inv[p] < s->cur - 1e-9) return p; }
  return -1;
}
static int nactive(const St *s) { int n = 0; for (int p = 0; p < NPL; p++) n += s->active[p]; return n; }
static int build(St s, int last, int parent) {
  int id = NN++; Node *n = &N[id]; memset(n, 0, sizeof *n); n->parent = parent; strcpy(n->path, s.path);
  for (int p = 0; p < 3; p++) { n->inv[p] = s.inv[p]; n->active[p] = p < NPL ? s.active[p] : 0; } n->cur = s.cur; n->aggr = s.aggr;
  n->pot = 0; for (int p = 0; p < NPL; p++) n->pot += s.inv[p]; n->np = nactive(&s);
  int p = nactive(&s) <= 1 ? -1 : next_to_act(&s, last);
  if (p < 0) { n->term = 1; return id; }
  n->player = p;
  int opts[MAXA], no = 0; const int bb = NPL - 1, isBB = p == bb;
  if (s.jam) { opts[no++] = A_F; opts[no++] = A_C; }
  else if (s.cur <= 1 + 1e-9) {
    if (!isBB) { opts[no++] = A_F; opts[no++] = A_L; if (s.limpers == 0) opts[no++] = A_R2; opts[no++] = A_J; }
    else { opts[no++] = A_X; if (s.limpers == 1 && nactive(&s) == 2) opts[no++] = A_R3; opts[no++] = A_J; }
  } else { opts[no++] = A_F; opts[no++] = A_C; opts[no++] = A_J; }
  n->nact = no;
  for (int k = 0; k < no; k++) {
    St c = s; int a = opts[k]; n->act[k] = a; c.acted[p] = 1;
    char tag[8]; snprintf(tag, sizeof tag, "%s%c", s.path[0] ? "-" : "", "FXCLRrJ"[a]); strncat(c.path, tag, 63 - strlen(c.path));
    switch (a) {
      case A_F: c.active[p] = 0; break;
      case A_X: break;
      case A_C: c.inv[p] = c.cur; break;
      case A_L: c.inv[p] = 1; c.limpers++; break;
      case A_R2: c.inv[p] = 2; c.cur = 2; c.aggr = p; for (int q = 0; q < NPL; q++) if (q != p) c.acted[q] = 0; break;
      case A_R3: c.inv[p] = 3; c.cur = 3; c.aggr = p; for (int q = 0; q < NPL; q++) if (q != p) c.acted[q] = 0; break;
      case A_J: c.inv[p] = S; c.cur = S; c.jam = 1; c.aggr = p; for (int q = 0; q < NPL; q++) if (q != p) c.acted[q] = 0; break;
    }
    n = &N[id]; n->child[k] = build(c, p, id); n = &N[id];
  }
  n->reg = calloc((size_t)no * NC, sizeof(double)); n->avg = calloc((size_t)no * NC, sizeof(double));
  return id;
}

// ---- equity realization ----
static double HF[NC];
static void hand_factors(void) {
  for (int i = 0; i < NC; i++) { int r = i / 13, c = i % 13; double f = 0;
    if (r == c) f = 0.0;
    else { int hi = r < c ? r : c, lo = r < c ? c : r, gap = lo - hi - 1, suited = r < c;
      if (suited) f += 0.05;
      if (gap == 0) f += 0.03; else if (gap == 1) f += 0.015; else if (gap >= 3 && !suited) f -= 0.04;
      if (hi >= 4 && lo >= 7 && !suited) f -= 0.02; }   // weak offsuit hands
    HF[i] = f; }
}
// calibrated realization (from real flop solves, see pf/calib.c): per class, for raised pots (IP aggressor / OOP caller)
// and limped pots (IP / OOP), measured at SPR 2.9 and 7; other roles use the geometric mean, other SPRs scale the log
static double RIPA[NC], ROOPC[NC], RIPL[NC], ROOPL[NC]; static int HAVE_EQR = 0;
static const double SPR_R = 13.0 / 4.5, SPR_L = 7.0;
static double realize(const Node *t, int p, int h) { // factor for player p holding class h at a flop terminal
  double spr = (S - t->cur) / t->pot;
  int first = -1, last = -1; for (int k = 0; k < 3; k++) { int q = POSTORD[k]; if (q < NPL && t->active[q]) { if (first < 0) first = q; last = q; } }
  if (HAVE_EQR) {
    int raised = t->cur >= 2 - 1e-9, aggr = p == t->aggr; double b;
    if (raised) b = p == last ? (aggr ? RIPA[h] : sqrt(RIPA[h] * ROOPC[h])) : p == first ? (aggr ? sqrt(RIPA[h] * ROOPC[h]) : ROOPC[h]) : sqrt(RIPA[h] * ROOPC[h]);
    else b = p == last ? RIPL[h] : p == first ? ROOPL[h] : sqrt(RIPL[h] * ROOPL[h]);
    double k = sqrt(spr / (raised ? SPR_R : SPR_L)); k = fmin(1.6, fmax(0.5, k));
    return exp(log(b) * k);
  }
  double g = fmin(1, fmax(0, (spr - 0.5) / 6));
  double pos = p == last ? 1.0 + 0.04 * g : p == first ? 1.0 - 0.10 * g : 1.0 - 0.05 * g;
  if (p == t->aggr) pos += 0.04 * g;
  return pos * (1 + g * HF[h]);
}
static void load_eqr(void) {
  FILE *f = fopen("eqr.txt", "r"); if (!f) return; double *T[4] = {RIPA, ROOPC, RIPL, ROOPL};
  for (int k = 0; k < 4; k++) for (int h = 0; h < NC; h++) if (fscanf(f, "%lf", &T[k][h]) != 1) { fclose(f); return; }
  fclose(f); HAVE_EQR = 1; fprintf(stderr, "calibrated realization loaded\n");
}

// ---- CFR ----
static double (*RCH)[3][NC];   // reach per node per player
static double (*CFV)[NC];      // scratch per node
static void strategy(const Node *n, double *out /* nact*NC */) {
  for (int h = 0; h < NC; h++) { double t = 0; for (int k = 0; k < n->nact; k++) { double r = n->reg[k * NC + h]; t += r > 0 ? r : 0; }
    for (int k = 0; k < n->nact; k++) { double r = n->reg[k * NC + h]; out[k * NC + h] = t > 0 ? (r > 0 ? r : 0) / t : 1.0 / n->nact; } }
}
static void avg_strategy(const Node *n, double *out) {
  for (int h = 0; h < NC; h++) { double t = 0; for (int k = 0; k < n->nact; k++) t += n->avg[k * NC + h];
    for (int k = 0; k < n->nact; k++) out[k * NC + h] = t > 0 ? n->avg[k * NC + h] / t : 1.0 / n->nact; }
}
static void mvec(const double *pi, double *m) { for (int h = 0; h < NC; h++) { double s = 0; for (int j = 0; j < NC; j++) s += PJ[h][j] * pi[j]; m[h] = s; } }

// value of terminal t for player p, per class h (net chips), weighted by opponents' reach
static void term_value(const Node *t, int p, const double R[3][NC], double *v) {
  int act[3], na = 0; for (int q = 0; q < NPL; q++) if (t->active[q]) act[na++] = q;
  int opp[2], no = 0; for (int q = 0; q < NPL; q++) if (q != p) opp[no++] = q;
  static double m0[NC], m1[NC];
  int allin = t->cur >= S - 1e-9;
  if (na == 1) { // everyone else folded
    double u = -t->inv[p] + (act[0] == p ? t->pot : 0);
    mvec(R[opp[0]], m0); if (no > 1) mvec(R[opp[1]], m1);
    for (int h = 0; h < NC; h++) v[h] = u * m0[h] * (no > 1 ? m1[h] : 1);
    return;
  }
  if (na == 2) {
    int a = act[0], b = act[1];
    if (!t->active[p]) { // p folded earlier: loses what he put in
      mvec(R[a], m0); mvec(R[b], m1); for (int h = 0; h < NC; h++) v[h] = -t->inv[p] * m0[h] * m1[h]; return;
    }
    int q = p == a ? b : a, r = -1; for (int k = 0; k < no; k++) if (opp[k] != q) r = opp[k];
    if (r >= 0) mvec(R[r], m1);
    static double rp[NC], rq[NC];
    if (!allin) for (int h = 0; h < NC; h++) { rp[h] = realize(t, p, h); rq[h] = realize(t, q, h); }
    #pragma omp parallel for schedule(static)
    for (int h = 0; h < NC; h++) {
      double s = 0;
      for (int j = 0; j < NC; j++) { double w = PJ[h][j] * R[q][j]; if (w == 0) continue;
        double e = E[h][j], share = allin ? e : e * rp[h] / (e * rp[h] + (1 - e) * rq[j] + 1e-12);
        s += w * (t->pot * share - t->inv[p]); }
      v[h] = s * (r >= 0 ? m1[h] : 1);
    }
    return;
  }
  // three players see the flop or go all-in
  int q = opp[0], r = opp[1];
  static double rr[3][NC];
  if (!allin) for (int k = 0; k < 3; k++) for (int h = 0; h < NC; h++) rr[k][h] = realize(t, k, h);
  #pragma omp parallel for schedule(dynamic, 4)
  for (int h = 0; h < NC; h++) {
    double s = 0;
    for (int j = 0; j < NC; j++) { double wj = PJ[h][j] * R[q][j]; if (wj < 1e-12) continue; double sk = 0;
      for (int k = 0; k < NC; k++) { double wk = PJ[h][k] * R[r][k]; if (wk < 1e-12) continue;
        int c[3]; c[p] = h; c[q] = j; c[r] = k; size_t ix = WI(c[0], c[1], c[2]);
        double share;
        if (allin) share = W[p][ix];
        else { double a0 = rr[0][c[0]] * W[0][ix], a1 = rr[1][c[1]] * W[1][ix], a2 = rr[2][c[2]] * W[2][ix]; share = (p == 0 ? a0 : p == 1 ? a1 : a2) / (a0 + a1 + a2 + 1e-12); }
        sk += wk * (t->pot * share - t->inv[p]); }
      s += wj * sk; }
    v[h] = s;
  }
}

static double *STR;  // scratch strategy
static void forward(int id, int it) {
  Node *n = &N[id]; if (n->term) return;
  double st[MAXA * NC]; strategy(n, st);
  for (int k = 0; k < n->nact; k++) { int c = n->child[k];
    for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[c][q][h] = RCH[id][q][h] * (q == n->player ? st[k * NC + h] : 1);
    forward(c, it); }
}
// returns cfv for player p at node id into CFV[id]; updates regrets of p
static void backward(int id, int p, int it, int update, int br) {
  Node *n = &N[id];
  if (n->term) { term_value(n, p, RCH[id], CFV[id]); return; }
  for (int k = 0; k < n->nact; k++) backward(n->child[k], p, it, update, br);
  if (n->player != p) { for (int h = 0; h < NC; h++) { double s = 0; for (int k = 0; k < n->nact; k++) s += CFV[n->child[k]][h]; CFV[id][h] = s; } return; }
  double st[MAXA * NC]; if (br) avg_strategy(n, st); else strategy(n, st);
  for (int h = 0; h < NC; h++) {
    double v = 0;
    if (br == 2) { v = -1e300; for (int k = 0; k < n->nact; k++) v = fmax(v, CFV[n->child[k]][h]); }
    else for (int k = 0; k < n->nact; k++) v += st[k * NC + h] * CFV[n->child[k]][h];
    CFV[id][h] = v;
    if (update) for (int k = 0; k < n->nact; k++) n->reg[k * NC + h] += CFV[n->child[k]][h] - v;
  }
  if (update) { // discount (DCFR) and average
    double ta = pow(it, 1.5), pos = ta / (ta + 1), neg = 0.5, wavg = pow((double)it / (it + 1), 2);
    for (int k = 0; k < n->nact; k++) for (int h = 0; h < NC; h++) {
      double *r = &n->reg[k * NC + h]; *r *= *r > 0 ? pos : neg;
      n->avg[k * NC + h] = n->avg[k * NC + h] * wavg + RCH[id][p][h] * st[k * NC + h]; }
  }
}
static double game_value(int p, int br) { // average strategy (br=1) or best response (br=2), in BB per hand, averaged over p's classes
  // reach with average strategies
  for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[0][q][h] = 1;
  // forward with average strategies
  int stack[MAXN], sp = 0; stack[sp++] = 0;
  while (sp) { int id = stack[--sp]; Node *n = &N[id]; if (n->term) continue; double st[MAXA * NC]; avg_strategy(n, st);
    for (int k = 0; k < n->nact; k++) { int c = n->child[k]; for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[c][q][h] = RCH[id][q][h] * (q == n->player ? st[k * NC + h] : 1); stack[sp++] = c; } }
  backward(0, p, 0, 0, br);
  double v = 0; for (int h = 0; h < NC; h++) v += ncombo[h] / 1326.0 * CFV[0][h]; return v;
}

static void solve(int iters, int verbose) {
  for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[0][q][h] = 1;
  for (int it = 1; it <= iters; it++) {
    forward(0, it);
    for (int p = 0; p < NPL; p++) backward(0, p, it, 1, 0);
    if (it == iters || (verbose && it % verbose == 0)) {
      double ex = 0, ev[3]; for (int p = 0; p < NPL; p++) { ev[p] = game_value(p, 1); ex += game_value(p, 2) - ev[p]; }
      fprintf(stderr, "  it %d  exploitability %.4f BB\n", it, ex);
      for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[0][q][h] = 1;
    }
  }
}

static void dump(FILE *o, double stack, const char *game) {
  fprintf(o, "{\"game\":\"%s\",\"S\":%g,\"nodes\":[", game, stack);
  int first = 1;
  for (int id = 0; id < NN; id++) { Node *n = &N[id]; if (n->term) continue;
    double st[MAXA * NC]; avg_strategy(n, st);
    // reach of this node for the acting player (how often the line happens with each hand)
    fprintf(o, "%s{\"path\":\"%s\",\"p\":%d,\"acts\":[", first ? "" : ",", n->path, n->player); first = 0;
    for (int k = 0; k < n->nact; k++) fprintf(o, "%s\"%s\"", k ? "," : "", ANAME[n->act[k]]);
    fprintf(o, "],\"f\":[");
    for (int k = 0; k < n->nact; k++) { fprintf(o, "%s[", k ? "," : ""); for (int h = 0; h < NC; h++) fprintf(o, "%s%d", h ? "," : "", (int)lrint(st[k * NC + h] * 1000)); fprintf(o, "]"); }
    fprintf(o, "]}");
  }
  fprintf(o, "]}\n");
}

int main(int argc, char **argv) {
  const char *game = argc > 1 ? argv[1] : "3h"; S = argc > 2 ? atof(argv[2]) : 15; int iters = argc > 3 ? atoi(argv[3]) : 400; int verbose = argc > 4 ? atoi(argv[4]) : 100;
  build_classes(); hand_factors(); if (!getenv("NOEQR")) load_eqr();
  long n = 0; for (int i = 0; i < NC; i++) for (int j = i; j < NC; j++) { TOFF[i][j] = n; n += NC - j; }
  FILE *f = fopen("eq.bin", "rb"); if (!f || fread(E, sizeof(E), 1, f) != 1 || fread(Ncomp, sizeof(Ncomp), 1, f) != 1) { fprintf(stderr, "eq.bin\n"); return 1; } fclose(f);
  for (int h = 0; h < NC; h++) { double t = 0; for (int j = 0; j < NC; j++) t += Ncomp[h][j]; for (int j = 0; j < NC; j++) PJ[h][j] = Ncomp[h][j] / t; }
  NPL = strcmp(game, "hu") ? 3 : 2;
  if (NPL == 3) {
    float *canon = malloc(sizeof(float) * n * 6); f = fopen("p3.bin", "rb"); if (!f || fread(canon, sizeof(float), n * 6, f) != (size_t)n * 6) { fprintf(stderr, "p3.bin\n"); return 1; } fclose(f);
    for (int p = 0; p < 3; p++) W[p] = malloc(sizeof(float) * (size_t)NC * NC * NC);
    #pragma omp parallel for
    for (int a = 0; a < NC; a++) for (int b = 0; b < NC; b++) for (int c = 0; c < NC; c++) {
      int h[3] = {a, b, c}, pl[3] = {0, 1, 2};
      for (int x = 0; x < 3; x++) for (int y = x + 1; y < 3; y++) if (h[pl[y]] < h[pl[x]]) { int t = pl[x]; pl[x] = pl[y]; pl[y] = t; }
      const float *src = canon + tri_index(h[pl[0]], h[pl[1]], h[pl[2]]) * 6; double first[3] = {0, 0, 0};
      for (int oc = 0; oc < 6; oc++) first[pl[PERM[oc][0]]] += src[oc];
      for (int p = 0; p < 3; p++) W[p][WI(a, b, c)] = (float)first[p];
    }
    free(canon);
    POSTORD[0] = 1; POSTORD[1] = 2; POSTORD[2] = 0;   // SB, BB, BTN
  } else { POSTORD[0] = 1; POSTORD[1] = 0; POSTORD[2] = 2; }   // heads-up: BB first, SB (button) last
  St s; memset(&s, 0, sizeof s);
  for (int p = 0; p < NPL; p++) s.active[p] = 1;
  if (NPL == 3) { s.inv[1] = 0.5; s.inv[2] = 1; } else { s.inv[0] = 0.5; s.inv[1] = 1; }
  s.cur = 1; s.aggr = -1;
  NN = 0; build(s, NPL == 3 ? 2 : 1, -1);
  RCH = calloc(NN, sizeof *RCH); CFV = calloc(NN, sizeof *CFV);
  int dec = 0; for (int i = 0; i < NN; i++) dec += !N[i].term;
  fprintf(stderr, "%s %g BB: %d nodes, %d decisions\n", game, S, NN, dec);
  double t0 = omp_get_wtime(); solve(iters, verbose); fprintf(stderr, "  %.1f s\n", omp_get_wtime() - t0);
  dump(stdout, S, game);
  return 0;
}
