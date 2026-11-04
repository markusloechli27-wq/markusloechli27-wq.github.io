// Min c-bet strategies for a minraised pot: OOP checks, IP checks / bets 1 BB / goes all-in, OOP answers fold / call / all-in.
// Writes one JSON line per flop with per-class frequencies (0-100) for the IP c-bet node and the OOP node facing the min bet.
// usage: ./cbet ranges.json key_oop key_ip pot stack flops.txt nflops part parts > out.jsonl
// FULLTREE: flop sizes from IPSIZES (default "1bb,0.33,0.66"); OOPSIZES (e.g. "1bb,0.33,0.66") lets OOP bet first on the flop
//   (OOP aggressor or limped pot): then the root is written as "lead" and IP facing each lead as "vsl<size>".
#define NATIVE
#include "pfsolve.c"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int card_of(const char *s) { const char *R = "23456789TJQKA", *S = "cdhs"; const char *r = strchr(R, s[0]), *u = strchr(S, s[1]); return r && u ? (int)(r - R) * 4 + (int)(u - S) : -1; }
static int class_of(int a, int b) {
  int ra = 12 - (a >> 2), rb = 12 - (b >> 2); if (ra > rb) { int t = ra; ra = rb; rb = t; }
  if (ra == rb) return ra * 13 + ra; return ((a & 3) == (b & 3)) ? ra * 13 + rb : rb * 13 + ra;
}
static int read_range(const char *json, const char *key, float *out) {
  char pat[64]; snprintf(pat, sizeof pat, "\"%s\":[", key); const char *p = strstr(json, pat); if (!p) return -1; p += strlen(pat);
  for (int i = 0; i < 169; i++) { out[i] = strtof(p, (char **)&p); while (*p == ',' || *p == ' ') p++; } return 0;
}
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(FILE *f, const unsigned char *p, size_t n) {
  for (size_t i = 0; i < n; i += 3) { unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    fputc(B64[v >> 18 & 63], f); fputc(B64[v >> 12 & 63], f); fputc(i + 1 < n ? B64[v >> 6 & 63] : '=', f); fputc(i + 2 < n ? B64[v & 63] : '=', f); }
}
static const char *ANM[] = {"fold", "check", "call", "bet", "raise", "allin"};
static int sizes(const char *s, float pot, float *out) { // "1bb,0.33,0.66" -> pot fractions
  int n = 0; while (s && *s && n < 4) { char *e; float v = strtof(s, &e); if (e == s) break; s = e; if (*s == 'b') { v /= pot; while (*s == 'b') s++; } out[n++] = v; while (*s == ',' || *s == ' ') s++; } return n; }
static const char *lab(int ci, float pot) { // bet sizes on the flop: 1 BB (min), 33 % or 66 % of the pot
  const Node *c = &NODES[ci]; if (c->acttype != A_BET) return ANM[c->acttype];
  if (fabsf(c->actamt - 1.0f) < 0.02f) return "bmin"; return c->actamt / pot < 0.5f ? "b33" : "b66";
}
// per-class average strategy at node ni for its player, weighted by that player's reach w (combos)
static void node_out(FILE *o, const char *name, int ni, const float *w, double *aggr) {
  const Node *n = &NODES[ni]; int p = n->player, na = n->nact, H = NH[p];
  float *st = pf_strategy(ni);
  double acc[6][169] = {{0}}, tot[169] = {0}, all[6] = {0}, allw = 0;
  for (int h = 0; h < H; h++) { int c = class_of(HC[p][2 * h], HC[p][2 * h + 1]); double wh = w[h]; if (wh <= 0) continue; tot[c] += wh; allw += wh;
    for (int a = 0; a < na; a++) { acc[a][c] += wh * st[a * H + h]; all[a] += wh * st[a * H + h]; } }
  fprintf(o, "\"%s\":{\"acts\":[", name);
  for (int a = 0; a < na; a++) fprintf(o, "%s\"%s\"", a ? "," : "", lab(n->child + a, P0));
  fprintf(o, "],\"tot\":[");
  for (int a = 0; a < na; a++) { fprintf(o, "%s%.1f", a ? "," : "", allw > 0 ? all[a] / allw * 100 : 0); if (aggr) aggr[a] += allw > 0 ? all[a] / allw : 0; }
  fprintf(o, "],\"f\":[");
  for (int a = 0; a < na; a++) { unsigned char b[169]; for (int c = 0; c < 169; c++) b[c] = (unsigned char)(tot[c] > 0 ? acc[a][c] / tot[c] * 100 + .5 : 0); fprintf(o, "%s\"", a ? "," : ""); b64(o, b, 169); fprintf(o, "\""); }
  fprintf(o, "]}");
}
int main(int argc, char **argv) {
  if (argc < 10) { fprintf(stderr, "usage\n"); return 1; }
  HEAP_CAP = (size_t)(getenv("PF_HEAP_MB") ? atoi(getenv("PF_HEAP_MB")) : 3000) << 20;
  FILE *f = fopen(argv[1], "r"); static char js[1 << 20]; size_t n = fread(js, 1, sizeof js - 1, f); js[n] = 0; fclose(f);
  float R0[169], R1[169]; if (read_range(js, argv[2], R0) || read_range(js, argv[3], R1)) { fprintf(stderr, "range key\n"); return 1; }
  float pot = atof(argv[4]), stack = atof(argv[5]); int nfl = atoi(argv[7]), part = atoi(argv[8]), parts = atoi(argv[9]);
  FILE *fl = fopen(argv[6], "r"); char line[64]; int k = -1, done = 0;
  while (done < nfl && fgets(line, sizeof line, fl)) {
    char key[16]; int w; if (sscanf(line, "%15s %d", key, &w) != 2) continue; k++; if (k % parts != part) continue;
    pf_reset(); float *cfg = pf_cfg_ptr(), *win = pf_win_ptr(); memset(cfg, 0, sizeof(float) * 64);
    cfg[0] = pot; cfg[1] = stack; cfg[2] = 0; cfg[3] = 3; for (int i = 0; i < 3; i++) cfg[4 + i] = (float)card_of(key + 2 * i);
    cfg[9] = 0; cfg[10] = 0; cfg[11] = 0.67f; cfg[12] = 4;
    float *s = cfg + 13;
    if (getenv("FULLTREE")) { // flop: IP min bet (1 BB), 33 %, 66 %; turn and river: 33 % and 66 % for both; raises go all-in
      s[0] = 2; s[1] = sizes(getenv("OOPSIZES"), pot, s + 2); s[6] = sizes(getenv("IPSIZES") ? getenv("IPSIZES") : "1bb,0.33,0.66", pot, s + 7); s[11] = 0;
      const char *tr = getenv("TRSIZES") ? getenv("TRSIZES") : "0.33,0.66";   // turn and river, both players
      s = cfg + 29; s[0] = 2; s[1] = sizes(tr, pot, s + 2); s[6] = sizes(tr, pot, s + 7); s[11] = 0;
      s = cfg + 45; s[0] = 2; s[1] = sizes(tr, pot, s + 2); s[6] = sizes(tr, pot, s + 7); s[11] = 0;
    } else {
      s[0] = 2; s[1] = 0; s[6] = 1; s[7] = 1.0f / pot; s[11] = 0;
      s = cfg + 29; s[0] = 1; s[1] = 0; s[6] = 0; s[11] = 0;
      s = cfg + 45; s[0] = 1; s[1] = 0; s[6] = 0; s[11] = 0;
    }
    for (int b = 1; b < 52; b++) for (int a = 0; a < b; a++) { int c = class_of(a, b), ix = combo_index(a, b); win[ix] = R0[c]; win[1326 + ix] = R1[c]; }
    if (getenv("ESTIMATE")) { double est = pf_estimate(); fprintf(stderr, "%s estimate %.0f MB, %d nodes\n", key, est / 1048576.0, pf_est_nodes()); done++; continue; }
    if (pf_setup() < 0) { fprintf(stderr, "setup failed %s\n", key); continue; }
    int it = 0, maxit = getenv("MAXIT") ? atoi(getenv("MAXIT")) : 60; float ex = 100; while (it < maxit) { pf_iterate(); it++; if (it % 10 == 0) { ex = pf_exploitability(); if (ex < 1.0f) break; } }
    // root (OOP) -> check -> IP node (c-bet) -> bet -> OOP node (facing the min bet)
    int root = 0, chk = -1, bet = -1;
    for (int a = 0; a < NODES[root].nact; a++) if (NODES[NODES[root].child + a].acttype == A_CHECK) chk = NODES[root].child + a;
    if (chk < 0 || NODES[chk].type != N_ACTION) { fprintf(stderr, "no check node %s\n", key); continue; }
    printf("{\"flop\":\"%s\",\"w\":%d,\"iters\":%d,\"expl\":%.2f,\"ev\":[%.3f,%.3f],", key, w, it, ex, pf_ev(0), pf_ev(1));
    node_out(stdout, "cbet", chk, W[1], NULL);
    int leads = 0; for (int a = 0; a < NODES[root].nact; a++) leads += NODES[NODES[root].child + a].acttype == A_BET;
    if (leads) { // OOP may bet first: its flop strategy and IP facing each bet size
      printf(","); node_out(stdout, "lead", root, W[0], NULL);
      for (int a = 0; a < NODES[root].nact; a++) { int c = NODES[root].child + a; if (NODES[c].acttype != A_BET || NODES[c].type != N_ACTION) continue;
        char nm[16]; snprintf(nm, sizeof nm, "vsl%s", lab(c, P0) + 1); printf(","); node_out(stdout, nm, c, W[1], NULL); }
    }
    { // OOP facing each flop bet size; his reach: range times his check frequency at the root
      float *st0 = pf_strategy(root); int H0 = NH[0], ci = -1; for (int a = 0; a < NODES[root].nact; a++) if (NODES[root].child + a == chk) ci = a;
      float *w0 = malloc(sizeof(float) * H0); for (int h = 0; h < H0; h++) w0[h] = W[0][h] * st0[ci * H0 + h];
      for (int a = 0; a < NODES[chk].nact; a++) { bet = NODES[chk].child + a; if (NODES[bet].acttype != A_BET || NODES[bet].type != N_ACTION) continue;
        char nm[16]; snprintf(nm, sizeof nm, "vs%s", lab(bet, P0)); printf(","); node_out(stdout, nm, bet, w0, NULL); }
      free(w0);
    }
    printf("}\n"); fflush(stdout); done++;
    fprintf(stderr, "%s it %d expl %.2f%%\n", key, it, ex); fflush(stderr);
  }
  return 0;
}
