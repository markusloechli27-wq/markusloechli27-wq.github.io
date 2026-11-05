// Spin & Gold early levels: preflop solver with fold / limp / minraise / all-in (3-handed and heads-up),
// any starting stacks, ChipEV or ICM (x10 8/2, x50-x100 60/30/10, jackpot 50/30/20).
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
static float *P3;     // finish orders for classes a,b,c of players 0,1,2: [WI*6 + o], o indexes PERM
static double X[3];   // starting stacks (BB)
static int MODEL = 0; // 0 ChipEV, 1 x10, 2 x50/x100, 3 jackpot
static const double PAY[4][3] = {{1, 0, 0}, {.8, .2, 0}, {.6, .3, .1}, {.5, .3, .2}};
#define WI(a,b,c) ((((size_t)(a)) * NC + (b)) * NC + (c))

enum { A_F, A_X, A_C, A_L, A_R2, A_R3, A_J, A_3B };
static const char *ANAME[] = { "fold", "check", "call", "limp", "raise2", "raise3", "allin", "threebet" };
#define MAXN 600
#define MAXA 5
typedef struct {
  int term, player, nact, act[MAXA], child[MAXA], parent;
  double inv[3]; int active[3], np, aggr, jam; double pot, cur;
  char path[64];
  double *reg, *avg;   // [nact][NC]
} Node;
static Node N[MAXN]; static int NN;
static int NPL;          // 3 or 2
static double S;         // stack in BB
static int POSTORD[3];   // postflop order (first to act first)

typedef struct { double inv[3]; int active[3], acted[3]; double cur; int aggr, limpers, jam, raises; char path[64]; } St;
static int next_to_act(const St *s, int last) {
  for (int d = 1; d <= NPL; d++) { int p = (last + d) % NPL; if (!s->active[p]) continue; if (s->inv[p] >= X[p] - 1e-9) continue; // all-in
    if (!s->acted[p] || s->inv[p] < s->cur - 1e-9) return p; }
  return -1;
}
static int nactive(const St *s) { int n = 0; for (int p = 0; p < NPL; p++) n += s->active[p]; return n; }
static int build(St s, int last, int parent) {
  int id = NN++; Node *n = &N[id]; memset(n, 0, sizeof *n); n->parent = parent; strcpy(n->path, s.path);
  for (int p = 0; p < 3; p++) { n->inv[p] = s.inv[p]; n->active[p] = p < NPL ? s.active[p] : 0; } n->cur = s.cur; n->aggr = s.aggr; n->jam = s.jam;
  n->pot = 0; for (int p = 0; p < NPL; p++) n->pot += s.inv[p]; n->np = nactive(&s);
  int p = nactive(&s) <= 1 ? -1 : next_to_act(&s, last);
  if (p < 0) { n->term = 1; return id; }
  n->player = p;
  int opts[MAXA], no = 0; const int bb = NPL - 1, isBB = p == bb;
  // a player whose stack does not cover the current bet can only fold or call all-in
  if (s.jam || X[p] <= s.cur + 1e-9) { opts[no++] = A_F; opts[no++] = A_C; }
  else if (s.cur <= 1 + 1e-9) {
    if (!isBB) { opts[no++] = A_F; opts[no++] = A_L; if (s.limpers == 0 && X[p] > 3) opts[no++] = A_R2;
      if (NPL == 3 && p == 1 && s.limpers == 0 && !s.active[0] && X[p] >= 12) opts[no++] = A_R3;   // SB opens to 3 BB after the BTN folds
      opts[no++] = A_J; }
    else { opts[no++] = A_X; if (s.limpers == 1 && nactive(&s) == 2 && X[p] > 4.5) opts[no++] = A_R3; opts[no++] = A_J; }
  } else { opts[no++] = A_F; opts[no++] = A_C;
    if (s.raises == 1 && 2.25 * s.cur <= 0.4 * X[p] + 1e-9) opts[no++] = A_3B;   // small 3-bet to 2.25x the raise while it is at most 40 % of the stack
    opts[no++] = A_J; }
  n->nact = no;
  for (int k = 0; k < no; k++) {
    St c = s; int a = opts[k]; n->act[k] = a; c.acted[p] = 1;
    char tag[8]; snprintf(tag, sizeof tag, "%s%c", s.path[0] ? "-" : "", "FXCLRrJT"[a]); strncat(c.path, tag, 63 - strlen(c.path));
    switch (a) {
      case A_F: c.active[p] = 0; break;
      case A_X: break;
      case A_C: c.inv[p] = fmin(X[p], c.cur); break;
      case A_L: c.inv[p] = 1; c.limpers++; break;
      case A_R2: c.inv[p] = 2; c.cur = 2; c.aggr = p; c.raises++; for (int q = 0; q < NPL; q++) if (q != p) c.acted[q] = 0; break;
      case A_R3: c.inv[p] = 3; c.cur = 3; c.aggr = p; c.raises++; for (int q = 0; q < NPL; q++) if (q != p) c.acted[q] = 0; break;
      case A_3B: c.inv[p] = 2.25 * s.cur; c.cur = 2.25 * s.cur; c.aggr = p; c.raises++; for (int q = 0; q < NPL; q++) if (q != p) c.acted[q] = 0; break;
      case A_J: c.inv[p] = X[p]; if (X[p] > c.cur) c.cur = X[p]; c.jam = 1; c.aggr = p; for (int q = 0; q < NPL; q++) if (q != p) c.acted[q] = 0; break;
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
// limped pots depend on who limped: lines 3/4 = limper in position vs BB out of position (heads-up, BTN limp),
// lines 5/6 (optional) = BB in position vs the SB limper out of position (3-handed after a BTN fold)
static double RIPA[NC], ROOPC[NC], RIPL[NC], ROOPL[NC], RIPLB[NC], ROOPLB[NC]; static int HAVE_EQR = 0, HAVE_LB = 0;
static const double SPR_R = 13.0 / 4.5, SPR_L = 7.0;
static double realize(const Node *t, int p, int h) { // factor for player p holding class h at a flop terminal
  double xs[3]; int na = 0; for (int q = 0; q < NPL; q++) if (t->active[q]) xs[na++] = X[q];
  double eff = na == 2 ? fmin(xs[0], xs[1]) : fmax(fmin(xs[0], xs[1]), fmin(fmax(xs[0], xs[1]), xs[2])); // second largest
  double spr = fmax(0.2, (eff - t->cur) / t->pot);
  int first = -1, last = -1; for (int k = 0; k < 3; k++) { int q = POSTORD[k]; if (q < NPL && t->active[q]) { if (first < 0) first = q; last = q; } }
  if (HAVE_EQR) {
    int raised = t->cur >= 2 - 1e-9, aggr = p == t->aggr; double b;
    if (raised) b = p == last ? (aggr ? RIPA[h] : sqrt(RIPA[h] * ROOPC[h])) : p == first ? (aggr ? sqrt(RIPA[h] * ROOPC[h]) : ROOPC[h]) : sqrt(RIPA[h] * ROOPC[h]);
    else if (HAVE_LB && last == NPL - 1) b = p == last ? RIPLB[h] : p == first ? ROOPLB[h] : sqrt(RIPLB[h] * ROOPLB[h]);
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
  HAVE_EQR = 1; HAVE_LB = 1;
  for (int k = 0; k < 2 && HAVE_LB; k++) for (int h = 0; h < NC; h++) if (fscanf(f, "%lf", k ? &ROOPLB[h] : &RIPLB[h]) != 1) { HAVE_LB = 0; break; }
  fclose(f); fprintf(stderr, "calibrated realization loaded%s\n", HAVE_LB ? " (limped pots by limper)" : "");
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

// ---- payoffs: final stacks for each outcome, then ChipEV (net chips) or ICM (share of the prize pool) ----
static double util(const double *s, int p) {
  if (MODEL == 0) return s[p] - X[p];
  const double *pay = PAY[MODEL]; int alive[3], na = 0;
  for (int i = 0; i < 3; i++) { alive[i] = s[i] > 1e-9; na += alive[i]; }
  if (na == 3) { double T = s[0] + s[1] + s[2], p1 = s[p] / T, p2 = 0;
    for (int j = 0; j < 3; j++) if (j != p) p2 += s[j] / T * s[p] / (T - s[j]);
    return pay[0] * p1 + pay[1] * p2 + pay[2] * (1 - p1 - p2); }
  if (na == 2) { if (!alive[p]) return pay[2]; int o = -1; for (int j = 0; j < 3; j++) if (j != p && alive[j]) o = j; double p1 = s[p] / (s[p] + s[o]); return pay[0] * p1 + pay[1] * (1 - p1); }
  if (alive[p]) return pay[0];
  int o = -1; for (int j = 0; j < 3; j++) if (j != p && !alive[j]) o = j;   // busted together: bigger starting stack places higher
  if (X[p] > X[o] + 1e-9) return pay[1]; if (X[p] < X[o] - 1e-9) return pay[2]; return (pay[1] + pay[2]) / 2;
}
// pot distribution with side pots: order[] lists the active players best first
static void settle(const Node *t, const int *order, int nord, double *fin) {
  double c[3] = {0, 0, 0}; for (int q = 0; q < NPL; q++) { c[q] = t->inv[q]; fin[q] = X[q] - c[q]; } if (NPL == 2) fin[2] = 0;
  double lv[3]; int nl = 0; for (int k = 0; k < nord; k++) lv[nl++] = c[order[k]];
  for (int a = 0; a < nl; a++) for (int b = a + 1; b < nl; b++) if (lv[b] < lv[a]) { double x = lv[a]; lv[a] = lv[b]; lv[b] = x; }
  double prev = 0;
  for (int L = 0; L < nl; L++) { double top = lv[L]; if (top <= prev + 1e-12) continue; double pot = 0;
    for (int q = 0; q < NPL; q++) pot += fmax(0, fmin(c[q], top) - fmin(c[q], prev));
    if (L == nl - 1) for (int q = 0; q < NPL; q++) if (!t->active[q]) pot += fmax(0, c[q] - top); // dead money above the last level
    for (int k = 0; k < nord; k++) { int q = order[k]; if (c[q] >= top - 1e-12) { fin[q] += pot; break; } }
    prev = top; }
}
static double m0[NC], m1[NC];
static void term_value(const Node *t, int p, const double R[3][NC], double *v) {
  int act[3], na = 0; for (int q = 0; q < NPL; q++) if (t->active[q]) act[na++] = q;
  int opp[2], no = 0; for (int q = 0; q < NPL; q++) if (q != p) opp[no++] = q;
  double fin[3];
  if (na == 1) { // everyone else folded
    settle(t, act, 1, fin); double u = util(fin, p);
    mvec(R[opp[0]], m0); if (no > 1) mvec(R[opp[1]], m1);
    for (int h = 0; h < NC; h++) v[h] = u * m0[h] * (no > 1 ? m1[h] : 1);
    return;
  }
  if (na == 2) {
    int a = act[0], b = act[1], oa[2] = {a, b}, ob[2] = {b, a}; double fa[3], fb[3]; settle(t, oa, 2, fa); settle(t, ob, 2, fb);
    static double ra[NC], rb[NC];
    if (!t->jam) for (int h = 0; h < NC; h++) { ra[h] = realize(t, a, h); rb[h] = realize(t, b, h); }
    if (!t->active[p]) { // p folded earlier: his value depends only on who wins between a and b
      double ua = util(fa, p), ub = util(fb, p); mvec(R[a], m0); mvec(R[b], m1);
      double pa = 0.5;
      if (fabs(ua - ub) > 1e-12) { double s = 0, z = 0; // average win chance of a, over both reach ranges
        for (int j = 0; j < NC; j++) { double wj = R[a][j] * ncombo[j]; if (wj <= 0) continue; for (int k = 0; k < NC; k++) { double w = wj * R[b][k] * ncombo[k]; if (w <= 0) continue;
          double e = E[j][k]; if (!t->jam) e = e * ra[j] / (e * ra[j] + (1 - e) * rb[k] + 1e-12); s += w * e; z += w; } }
        pa = z > 0 ? s / z : 0.5; }
      double u = pa * ua + (1 - pa) * ub; for (int h = 0; h < NC; h++) v[h] = u * m0[h] * m1[h]; return;
    }
    int q = p == a ? b : a, r = -1; for (int k = 0; k < no; k++) if (opp[k] != q) r = opp[k];
    if (r >= 0) mvec(R[r], m1);
    double uw = util(p == a ? fa : fb, p), ul = util(p == a ? fb : fa, p);
    const double *rp = p == a ? ra : rb, *rq = p == a ? rb : ra;
    #pragma omp parallel for schedule(static)
    for (int h = 0; h < NC; h++) {
      double s = 0;
      for (int j = 0; j < NC; j++) { double w = PJ[h][j] * R[q][j]; if (w == 0) continue;
        double e = E[h][j], share = t->jam ? e : e * rp[h] / (e * rp[h] + (1 - e) * rq[j] + 1e-12);
        s += w * (share * uw + (1 - share) * ul); }
      v[h] = s * (r >= 0 ? m1[h] : 1);
    }
    return;
  }
  // three players see the flop or go all-in
  int q = opp[0], r = opp[1];
  static double rr[3][NC]; double U[6];
  if (t->jam) for (int o = 0; o < 6; o++) { int ord[3] = {PERM[o][0], PERM[o][1], PERM[o][2]}; settle(t, ord, 3, fin); U[o] = util(fin, p); }
  else { for (int w = 0; w < 3; w++) { int ord[3] = {w, (w + 1) % 3, (w + 2) % 3}; settle(t, ord, 3, fin); U[w] = util(fin, p); }
    for (int k = 0; k < 3; k++) for (int h = 0; h < NC; h++) rr[k][h] = realize(t, k, h); }
  #pragma omp parallel for schedule(dynamic, 4)
  for (int h = 0; h < NC; h++) {
    double s = 0;
    for (int j = 0; j < NC; j++) { double wj = PJ[h][j] * R[q][j]; if (wj < 1e-12) continue; double sk = 0;
      for (int k = 0; k < NC; k++) { double wk = PJ[h][k] * R[r][k]; if (wk < 1e-12) continue;
        int c[3]; c[p] = h; c[q] = j; c[r] = k; size_t ix = WI(c[0], c[1], c[2]);
        double u;
        if (t->jam) { const float *po = P3 + ix * 6; u = po[0] * U[0] + po[1] * U[1] + po[2] * U[2] + po[3] * U[3] + po[4] * U[4] + po[5] * U[5]; }
        else { double a0 = rr[0][c[0]] * W[0][ix], a1 = rr[1][c[1]] * W[1][ix], a2 = rr[2][c[2]] * W[2][ix], z = a0 + a1 + a2 + 1e-12; u = (a0 * U[0] + a1 * U[1] + a2 * U[2]) / z; }
        sk += wk * u; }
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
      if (MODEL) fprintf(stderr, "  it %d  exploitability %.4f %% of the prize pool\n", it, ex * 100); else fprintf(stderr, "  it %d  exploitability %.4f BB\n", it, ex);
      for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[0][q][h] = 1;
    }
  }
}

// ---- lines the equilibrium never takes (PATCH=1) ----
// When a player (almost) never takes an action, the numbers behind it mean nothing: his own hands have no weight there,
// and the others answer a range of next to nothing. With PATCH set, such an action is given the hands for which it is
// worth most (PATCH_SHARE of the range that gets there, default 10 %; a value-heavy range, the way people deviate in
// practice), and the subtree behind it is solved again with that range while everything above stays as solved. Two rounds (a short one, then the full one), so the choice of hands
// sees the solved continuation.
// Without PATCH the program and its output are unchanged.
static double *LOCK[MAXN];   // patched child: factor on the range of the player who acts at the parent
static int PATCHOF[MAXN];    // nearest patched node on the path to this node (itself included), -1 if none
static void reach_avg(void) {   // reach of every node with the average strategies and the patches (children come after parents)
  for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[0][q][h] = 1;
  for (int id = 0; id < NN; id++) { const Node *n = &N[id]; if (n->term) continue; double st[MAXA * NC]; avg_strategy(n, st);
    for (int k = 0; k < n->nact; k++) { int c = n->child[k];
      for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[c][q][h] = RCH[id][q][h] * (q != n->player ? 1 : LOCK[c] ? LOCK[c][h] : st[k * NC + h]); } }
}
static void sub_reset(int id) { Node *n = &N[id]; if (n->term) return;
  memset(n->reg, 0, sizeof(double) * n->nact * NC); memset(n->avg, 0, sizeof(double) * n->nact * NC); for (int k = 0; k < n->nact; k++) sub_reset(n->child[k]); }
static void patch(int rounds, int subit, double share, double minf) {
  int npatch = 0;
  for (int r = 0; r < rounds; r++) { reach_avg();
    for (int id = 0; id < NN; id++) { Node *n = &N[id]; if (n->term) continue; const int p = n->player;
      int ok = 1; for (int q = 0; q < NPL; q++) { double t = 0; for (int h = 0; h < NC; h++) t += RCH[id][q][h] * ncombo[h]; if (t < 1e-9) ok = 0; }
      if (!ok) continue;   // this node itself does not happen (only below a fold nobody makes)
      double st[MAXA * NC], tot = 0, f[MAXA]; avg_strategy(n, st); int need = 0;
      for (int h = 0; h < NC; h++) tot += RCH[id][p][h] * ncombo[h];
      for (int k = 0; k < n->nact; k++) { f[k] = 0; for (int h = 0; h < NC; h++) f[k] += RCH[id][p][h] * ncombo[h] * st[k * NC + h];
        if (n->act[k] != A_F && f[k] < minf * tot) need = 1; }
      if (!need) continue;
      backward(0, p, 0, 0, 1);   // values of p's actions here, with the average strategies and the patches so far
      double norm[NC], mq[NC]; for (int h = 0; h < NC; h++) norm[h] = 1;
      for (int q = 0; q < NPL; q++) if (q != p) { mvec(RCH[id][q], mq); for (int h = 0; h < NC; h++) norm[h] *= mq[h]; }
      for (int k = 0; k < n->nact; k++) { const int c = n->child[k]; if (n->act[k] == A_F || f[k] >= minf * tot) continue;
        double loss[NC]; int ord[NC];   // sorted by the value of this action, best first
        for (int h = 0; h < NC; h++) { loss[h] = norm[h] > 1e-15 ? -CFV[c][h] / norm[h] : 1e300; ord[h] = h; }
        for (int i = 1; i < NC; i++) { int x = ord[i], j = i; while (j > 0 && loss[ord[j - 1]] > loss[x]) { ord[j] = ord[j - 1]; j--; } ord[j] = x; }
        if (!LOCK[c]) LOCK[c] = malloc(sizeof(double) * NC);
        double acc = 0, want = share * tot; for (int h = 0; h < NC; h++) LOCK[c][h] = 0;
        for (int i = 0; i < NC && acc < want; i++) { int h = ord[i]; double w = RCH[id][p][h] * ncombo[h]; if (w <= 0) continue;
          double fr = fmin(1, (want - acc) / w); LOCK[c][h] = fr; acc += w * fr; }
        for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[c][q][h] = RCH[id][q][h] * (q == p ? LOCK[c][h] : 1);
        sub_reset(c);
        for (int it = 1, ni = r ? subit : subit / 2; it <= ni; it++) { forward(c, it); for (int q = 0; q < NPL; q++) backward(c, q, it, 1, 0); }
        if (r == 0) npatch++;
        if (getenv("PATCH_DEBUG") && r) { const char *RKs = "AKQJT98765432"; fprintf(stderr, "  %s:", N[c].path);
          for (int i = 0; i < NC; i++) { int h = ord[i]; if (LOCK[c][h] <= 0) continue; int a = h / 13, b = h % 13;
            if (a == b) fprintf(stderr, " %c%c", RKs[a], RKs[a]); else if (b > a) fprintf(stderr, " %c%cs", RKs[a], RKs[b]); else fprintf(stderr, " %c%co", RKs[b], RKs[a]); }
          fprintf(stderr, "\n"); }
      }
      reach_avg();
    }
  }
  for (int id = 0; id < NN; id++) PATCHOF[id] = LOCK[id] ? id : N[id].parent >= 0 ? PATCHOF[N[id].parent] : -1;
  reach_avg();
  fprintf(stderr, "  patched %d lines nobody plays\n", npatch);
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
    fprintf(o, "]");
    if (PATCHOF[id] >= 0) {   // behind a patched action: which one, and the actor's own reach (the app cannot get it from the frequencies)
      fprintf(o, ",\"lk\":\"%s\",\"r\":[", N[PATCHOF[id]].path);
      for (int h = 0; h < NC; h++) fprintf(o, "%s%d", h ? "," : "", (int)lrint(fmin(1, RCH[id][n->player][h]) * 1000)); fprintf(o, "]"); }
    fprintf(o, "}");
  }
  fprintf(o, "]}\n");
}

int main(int argc, char **argv) {
  const char *game = argc > 1 ? argv[1] : "3h"; S = argc > 2 ? atof(argv[2]) : 15; int iters = argc > 3 ? atoi(argv[3]) : 400; int verbose = argc > 4 ? atoi(argv[4]) : 100;
  X[0] = X[1] = X[2] = S;
  if (getenv("STACKS")) sscanf(getenv("STACKS"), "%lf,%lf,%lf", &X[0], &X[1], &X[2]);   // 3-handed: BTN, SB, BB
  if (getenv("MODEL")) MODEL = atoi(getenv("MODEL"));
  if (!strcmp(game, "hu")) MODEL = 0;   // heads-up both are in the money: prize equity is linear in chips
  build_classes(); hand_factors(); if (!getenv("NOEQR")) load_eqr();
  long n = 0; for (int i = 0; i < NC; i++) for (int j = i; j < NC; j++) { TOFF[i][j] = n; n += NC - j; }
  FILE *f = fopen("eq.bin", "rb"); if (!f || fread(E, sizeof(E), 1, f) != 1 || fread(Ncomp, sizeof(Ncomp), 1, f) != 1) { fprintf(stderr, "eq.bin\n"); return 1; } fclose(f);
  for (int h = 0; h < NC; h++) { double t = 0; for (int j = 0; j < NC; j++) t += Ncomp[h][j]; for (int j = 0; j < NC; j++) PJ[h][j] = Ncomp[h][j] / t; }
  NPL = strcmp(game, "hu") ? 3 : 2;
  if (NPL == 3) {
    float *canon = malloc(sizeof(float) * n * 6); f = fopen("p3.bin", "rb"); if (!f || fread(canon, sizeof(float), n * 6, f) != (size_t)n * 6) { fprintf(stderr, "p3.bin\n"); return 1; } fclose(f);
    for (int p = 0; p < 3; p++) W[p] = malloc(sizeof(float) * (size_t)NC * NC * NC);
    P3 = malloc(sizeof(float) * (size_t)NC * NC * NC * 6);
    #pragma omp parallel for
    for (int a = 0; a < NC; a++) for (int b = 0; b < NC; b++) for (int c = 0; c < NC; c++) {
      int h[3] = {a, b, c}, pl[3] = {0, 1, 2};
      for (int x = 0; x < 3; x++) for (int y = x + 1; y < 3; y++) if (h[pl[y]] < h[pl[x]]) { int t = pl[x]; pl[x] = pl[y]; pl[y] = t; }
      const float *src = canon + tri_index(h[pl[0]], h[pl[1]], h[pl[2]]) * 6; double first[3] = {0, 0, 0};
      float *dst = P3 + WI(a, b, c) * 6; for (int o = 0; o < 6; o++) dst[o] = 0;
      for (int oc = 0; oc < 6; oc++) { first[pl[PERM[oc][0]]] += src[oc]; dst[perm_index(pl[PERM[oc][0]], pl[PERM[oc][1]])] += src[oc]; }
      for (int p = 0; p < 3; p++) W[p][WI(a, b, c)] = (float)first[p];
    }
    free(canon);
    POSTORD[0] = 1; POSTORD[1] = 2; POSTORD[2] = 0;   // SB, BB, BTN
  } else { POSTORD[0] = 1; POSTORD[1] = 0; POSTORD[2] = 2; }   // heads-up: BB first, SB (button) last
  St s; memset(&s, 0, sizeof s);
  for (int p = 0; p < NPL; p++) s.active[p] = 1;
  if (NPL == 3) { s.inv[1] = 0.5; s.inv[2] = 1; } else { s.inv[0] = 0.5; s.inv[1] = 1; X[0] = X[1] = S; }
  s.cur = 1; s.aggr = -1;
  NN = 0; build(s, NPL == 3 ? 2 : 1, -1);
  RCH = calloc(NN, sizeof *RCH); CFV = calloc(NN, sizeof *CFV);
  int dec = 0; for (int i = 0; i < NN; i++) dec += !N[i].term;
  fprintf(stderr, "%s %g BB: %d nodes, %d decisions\n", game, S, NN, dec);
  double t0 = omp_get_wtime(); solve(iters, verbose); fprintf(stderr, "  %.1f s\n", omp_get_wtime() - t0);
  for (int i = 0; i < NN; i++) PATCHOF[i] = -1;
  if (getenv("PATCH")) { t0 = omp_get_wtime(); patch(2, iters / 2, getenv("PATCH_SHARE") ? atof(getenv("PATCH_SHARE")) : 0.10, 0.005); fprintf(stderr, "  patch %.1f s\n", omp_get_wtime() - t0); }
  if (getenv("DEBUG_EV")) { // EV of each root action per hand for the first player, with average strategies
    for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[0][q][h] = 1;
    int stk[MAXN], sp = 0; stk[sp++] = 0;
    while (sp) { int id = stk[--sp]; Node *nd = &N[id]; if (nd->term) continue; double st[MAXA * NC]; avg_strategy(nd, st);
      for (int k = 0; k < nd->nact; k++) { int c = nd->child[k]; for (int q = 0; q < NPL; q++) for (int h = 0; h < NC; h++) RCH[c][q][h] = RCH[id][q][h] * (q == nd->player ? st[k * NC + h] : 1); stk[sp++] = c; } }
    backward(0, N[0].player, 0, 0, 1);
    const char *RKs = "AKQJT98765432"; const char *list = getenv("DEBUG_EV");
    for (const char *p = list; *p; ) { char hn[4] = {0}; int L = 0; while (*p && *p != ',' && L < 3) hn[L++] = *p++; if (*p == ',') p++;
      int a = strchr(RKs, hn[0]) - RKs, b = strchr(RKs, hn[1]) - RKs, h = a == b ? a * 13 + a : (hn[2] == 's' ? (a < b ? a * 13 + b : b * 13 + a) : (a < b ? b * 13 + a : a * 13 + b));
      fprintf(stderr, "%-4s", hn); for (int k = 0; k < N[0].nact; k++) fprintf(stderr, "  %s %+.3f", ANAME[N[0].act[k]], CFV[N[0].child[k]][h]); fprintf(stderr, "\n"); }
  }
  dump(stdout, S, game);
  return 0;
}
