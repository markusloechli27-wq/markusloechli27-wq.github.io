// Equity realization from real flop solves: for a preflop spot (two 169-class ranges, pot, stack),
// solve a set of flops and report, per hand class and player, the share of the pot it wins on average.
// usage: ./calib ranges.json key_oop key_ip pot stack flops.txt nflops > out.txt
#define NATIVE
#include "pfsolve.c"
#include <stdio.h>
#include <string.h>

static int card_of(const char *s) { const char *R = "23456789TJQKA", *S = "cdhs"; const char *r = strchr(R, s[0]), *u = strchr(S, s[1]); return r && u ? (int)(r - R) * 4 + (int)(u - S) : -1; }
static int class_of(int a, int b) { // 0..168, row = high rank (A=0), suited above diagonal
  int ra = 12 - (a >> 2), rb = 12 - (b >> 2); if (ra > rb) { int t = ra; ra = rb; rb = t; }
  if (ra == rb) return ra * 13 + ra; return ((a & 3) == (b & 3)) ? ra * 13 + rb : rb * 13 + ra;
}
static int read_range(const char *json, const char *key, float *out) {
  char pat[64]; snprintf(pat, sizeof pat, "\"%s\":[", key); const char *p = strstr(json, pat); if (!p) return -1; p += strlen(pat);
  for (int i = 0; i < 169; i++) { out[i] = strtof(p, (char **)&p); while (*p == ',' || *p == ' ') p++; } return 0;
}
int main(int argc, char **argv) {
  if (argc < 8) { fprintf(stderr, "usage\n"); return 1; }
  HEAP_CAP = (size_t)1500 << 20;
  FILE *f = fopen(argv[1], "r"); static char js[1 << 20]; size_t n = fread(js, 1, sizeof js - 1, f); js[n] = 0; fclose(f);
  float R0[169], R1[169]; if (read_range(js, argv[2], R0) || read_range(js, argv[3], R1)) { fprintf(stderr, "range key\n"); return 1; }
  float pot = atof(argv[4]), stack = atof(argv[5]); int nfl = atoi(argv[7]), oopbet = argc > 8 ? atoi(argv[8]) : 0;
  FILE *fl = fopen(argv[6], "r"); char line[64];
  static double sh[2][169], wt[2][169]; double tw = 0; int done = 0;
  while (done < nfl && fgets(line, sizeof line, fl)) {
    char key[16]; int w; if (sscanf(line, "%15s %d", key, &w) != 2) continue;
    pf_reset(); float *cfg = pf_cfg_ptr(), *win = pf_win_ptr(); memset(cfg, 0, sizeof(float) * 64);
    cfg[0] = pot; cfg[1] = stack; cfg[2] = 0; cfg[3] = 3; for (int i = 0; i < 3; i++) cfg[4 + i] = (float)card_of(key + 2 * i);
    cfg[9] = 0; cfg[10] = 0; cfg[11] = 0.67f; cfg[12] = 4;
    float *s = cfg + 13; s[0] = 2; s[1] = oopbet; s[2] = 1.0f / pot; s[6] = 1; s[7] = 1.0f / pot; s[11] = 0;            // flop: OOP checks, IP min bet (1 BB) or all-in
    s = cfg + 29; s[0] = 1; s[1] = 0; s[6] = 0; s[11] = 0;                                         // turn: check or all-in
    s = cfg + 45; s[0] = 1; s[1] = 0; s[6] = 0; s[11] = 0;                                         // river: check or all-in
    for (int b = 1; b < 52; b++) for (int a = 0; a < b; a++) { int c = class_of(a, b), ix = combo_index(a, b); win[ix] = R0[c]; win[1326 + ix] = R1[c]; }
    if (pf_setup() < 0) { fprintf(stderr, "setup failed %s\n", key); continue; }
    int it = 0; float ex = 100; while (it < 50) { pf_iterate(); it++; if (it % 10 == 0) { ex = pf_exploitability(); if (ex < 1.0f) break; } }
    for (int trav = 0; trav < 2; trav++) {
      SCR_TOP = 0; float *out = scr(NH[trav]); walk(0, trav, W[1 - trav], out, 2);
      int o = 1 - trav; float cs[52], tot; card_sums(W[o], o, cs, &tot);
      for (int h = 0; h < NH[trav]; h++) { int sm = SAME[trav][h]; double c = tot - cs[HC[trav][2 * h]] - cs[HC[trav][2 * h + 1]] + (sm >= 0 ? W[o][sm] : 0);
        if (c <= 0 || W[trav][h] <= 0) continue; double ev = out[h] / c; int cl = class_of(HC[trav][2 * h], HC[trav][2 * h + 1]);
        sh[trav][cl] += w * W[trav][h] * ev / pot; wt[trav][cl] += w * W[trav][h]; }
    }
    tw += w; done++;
    for (int p = 0; p < 2; p++) { printf("P%d", p); for (int c = 0; c < 169; c++) printf(" %.4f", wt[p][c] > 0 ? sh[p][c] / wt[p][c] : -1); printf("\n"); } fflush(stdout);
    fprintf(stderr, "%s nodes %d it %d expl %.2f%%  EV oop %.3f ip %.3f\n", key, NNODES, it, ex, pf_ev(0), pf_ev(1)); fflush(stderr);
  }
  return 0;
}
