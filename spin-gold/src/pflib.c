// pflib.c — computes flop-library entries on a Raspberry Pi (or any Linux/Mac) for the poker review app.
// Build: gcc -O3 -march=native -o pflib pflib.c -lm      (pfsolve.c must be in the same folder)
// Run:   ./pflib pi-auftrag.txt <part> <parts> ergebnis-<part>.jsonl
// The job file comes from the app ("Pi-Auftrag erstellen"); the result file is imported in the app.
// Resumes automatically: flops already in the result file are skipped.
#define NATIVE
#include "pfsolve.c"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#define MAXSPOT 16
typedef struct { char id[32]; float cfg[64]; float w[2 * 1326]; } Spot;
static Spot SP[MAXSPOT]; static int NSP;
typedef struct { int spot; char flop[8]; } Task;
static Task *TASKS; static int NT, CAPT;

static double now_ms(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; }
static int card_of(const char *s) { const char *R = "23456789TJQKA", *S = "cdhs"; const char *r = strchr(R, s[0]), *u = strchr(S, s[1]); return r && u ? (int)(r - R) * 4 + (int)(u - S) : -1; }
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(FILE *f, const unsigned char *p, size_t n) {
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    fputc(B64[v >> 18 & 63], f); fputc(B64[v >> 12 & 63], f); fputc(i + 1 < n ? B64[v >> 6 & 63] : '=', f); fputc(i + 2 < n ? B64[v & 63] : '=', f);
  }
}
static int read_job(const char *fn) {
  FILE *f = fopen(fn, "r"); if (!f) { perror(fn); return -1; }
  static char line[200000]; int cur = -1;
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, "SPOT ", 5)) { if (NSP >= MAXSPOT) break; cur = NSP++; sscanf(line + 5, "%31s", SP[cur].id); }
    else if (!strncmp(line, "CFG ", 4) && cur >= 0) { char *p = line + 4; for (int i = 0; i < 64; i++) SP[cur].cfg[i] = strtof(p, &p); }
    else if (!strncmp(line, "W0 ", 3) && cur >= 0) { char *p = line + 3; for (int i = 0; i < 1326; i++) SP[cur].w[i] = strtof(p, &p); }
    else if (!strncmp(line, "W1 ", 3) && cur >= 0) { char *p = line + 3; for (int i = 0; i < 1326; i++) SP[cur].w[1326 + i] = strtof(p, &p); }
    else if (!strncmp(line, "F ", 2)) {
      char sid[32], fl[16]; if (sscanf(line + 2, "%31s %15s", sid, fl) != 2) continue;
      int si = -1; for (int k = 0; k < NSP; k++) if (!strcmp(SP[k].id, sid)) si = k; if (si < 0) continue;
      if (NT == CAPT) { CAPT = CAPT ? 2 * CAPT : 1024; TASKS = realloc(TASKS, sizeof(Task) * CAPT); }
      TASKS[NT].spot = si; strncpy(TASKS[NT].flop, fl, 7); TASKS[NT].flop[7] = 0; NT++;
    }
  }
  fclose(f); return 0;
}
static int done_has(char **keys, int nk, const char *k) { for (int i = 0; i < nk; i++) if (!strcmp(keys[i], k)) return 1; return 0; }

typedef struct { int node; char path[96]; float *r[2]; } QItem;
static void solve_one(const Spot *sp, const char *flop, FILE *out, int *iters_out, double *expl_out) {
  pf_reset();
  float *cfg = pf_cfg_ptr(), *w = pf_win_ptr();
  memcpy(cfg, sp->cfg, sizeof sp->cfg); memcpy(w, sp->w, sizeof sp->w);
  cfg[2] = 0; cfg[3] = 3; for (int i = 0; i < 3; i++) cfg[4 + i] = (float)card_of(flop + 2 * i);
  double t0 = now_ms();
  if (pf_setup() < 0) { fprintf(stderr, "setup failed (memory?) for %s %s\n", sp->id, flop); return; }
  int it = 0; float ex = 100;
  while (it < 120) { pf_iterate(); it++; if (it % 10 == 0) { ex = pf_exploitability(); if (ex <= 1.0f) break; } }
  if (it % 10) ex = pf_exploitability();
  double ms = now_ms() - t0;
  // extract every flop decision: average strategy and EV of each action per hand
  fprintf(out, "{\"key\":\"%s|normal|%s\",\"spot\":\"%s\",\"tree\":\"normal\",\"flop\":\"%s\",\"pot\":%g,\"stack\":%g,\"H\":[%d,%d],\"nodes\":[", sp->id, flop, sp->id, flop, sp->cfg[0], sp->cfg[1], NH[0], NH[1]);
  QItem *q = malloc(sizeof(QItem) * 256); int qh = 0, qt = 0, first = 1;
  q[qt].node = 0; q[qt].path[0] = 0; for (int p = 0; p < 2; p++) { q[qt].r[p] = malloc(sizeof(float) * NH[p]); memcpy(q[qt].r[p], W[p], sizeof(float) * NH[p]); } qt++;
  float *st = malloc(sizeof(float) * 16 * 1326);
  while (qh < qt) {
    QItem it2 = q[qh++]; const Node *n = &NODES[it2.node];
    if (n->type != N_ACTION) { free(it2.r[0]); free(it2.r[1]); continue; }
    int p = n->player, H = NH[p], na = n->nact;
    memcpy(st, pf_strategy(it2.node), sizeof(float) * na * H);
    float *rb = pf_reach_ptr(); memcpy(rb, it2.r[1 - p], sizeof(float) * NH[1 - p]);
    float *v = pf_values(it2.node, p);
    unsigned char *s8 = malloc(na * H); int16_t *e16 = malloc(sizeof(int16_t) * na * H);
    for (int i = 0; i < na * H; i++) { s8[i] = (unsigned char)(st[i] * 255 + .5f); float e = v ? v[i] * 50 : 0; e16[i] = (int16_t)(e > 32767 ? 32767 : e < -32767 ? -32767 : (e >= 0 ? e + .5f : e - .5f)); }
    fprintf(out, "%s{\"path\":\"%s\",\"p\":%d,\"acts\":[", first ? "" : ",", it2.path, p); first = 0;
    for (int k = 0; k < na; k++) { const Node *c = &NODES[n->child + k]; fprintf(out, "%s[%d,%g]", k ? "," : "", c->acttype, (double)((int)(c->actamt * 100 + .5f)) / 100); }
    fprintf(out, "],\"c\":[%g,%g],\"s\":\"", (double)((int)(n->c0 * 100 + .5f)) / 100, (double)((int)(n->c1 * 100 + .5f)) / 100);
    b64(out, s8, na * H); fputs("\",\"e\":\"", out); b64(out, (unsigned char *)e16, sizeof(int16_t) * na * H); fputs("\"}", out);
    free(s8); free(e16);
    for (int k = 0; k < na; k++) {
      if (qt >= 256) break;
      QItem c; c.node = n->child + k; snprintf(c.path, sizeof c.path, it2.path[0] ? "%s.%d" : "%s%d", it2.path, k);
      for (int pp = 0; pp < 2; pp++) { c.r[pp] = malloc(sizeof(float) * NH[pp]); memcpy(c.r[pp], it2.r[pp], sizeof(float) * NH[pp]); }
      for (int h = 0; h < H; h++) c.r[p][h] *= st[k * H + h];
      q[qt++] = c;
    }
    free(it2.r[0]); free(it2.r[1]);
  }
  free(st); free(q);
  fprintf(out, "],\"iters\":%d,\"expl\":%.3f,\"ms\":%.0f,\"src\":\"pi\",\"at\":%.0f}\n", it, ex, ms, (double)time(0) * 1000);
  fflush(out);
  *iters_out = it; *expl_out = ex;
}
int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage: %s pi-auftrag.txt <part> <parts> ergebnis.jsonl\n", argv[0]); return 1; }
  const char *env = getenv("PF_HEAP_MB"); HEAP_CAP = (size_t)(env ? atoi(env) : 1600) << 20;
  int part = atoi(argv[2]), parts = atoi(argv[3]);
  if (read_job(argv[1]) || !NT) { fprintf(stderr, "no tasks\n"); return 1; }
  // already done (resume)
  char **keys = 0; int nk = 0, ck = 0; FILE *in = fopen(argv[4], "r");
  if (in) { char *ln = 0; size_t cap = 0; ssize_t len;
    while ((len = getline(&ln, &cap, in)) > 0) { char *k = strstr(ln, "\"key\":\""); if (!k) continue; k += 7; char *e = strchr(k, '"'); if (!e || ln[len - 1] != '\n') continue; *e = 0;
      if (nk == ck) { ck = ck ? 2 * ck : 256; keys = realloc(keys, sizeof(char *) * ck); } keys[nk++] = strdup(k); }
    free(ln); fclose(in); }
  FILE *out = fopen(argv[4], "a"); if (!out) { perror(argv[4]); return 1; }
  int mine = 0; for (int i = 0; i < NT; i++) if (i % parts == part) mine++;
  fprintf(stderr, "Teil %d/%d: %d Flops, %d schon fertig\n", part + 1, parts, mine, nk);
  int done = 0; double t0 = now_ms();
  for (int i = 0; i < NT; i++) {
    if (i % parts != part) continue;
    char key[64]; snprintf(key, sizeof key, "%s|normal|%s", SP[TASKS[i].spot].id, TASKS[i].flop);
    if (done_has(keys, nk, key)) { done++; continue; }
    int it; double ex; double ts = now_ms();
    solve_one(&SP[TASKS[i].spot], TASKS[i].flop, out, &it, &ex);
    done++;
    double el = now_ms() - t0, per = (now_ms() - ts) / 1000;
    fprintf(stderr, "[%d/%d] %s %s: %d Iterationen, %.2f %% vom Pot, %.0f s (läuft seit %.1f h)\n", done, mine, SP[TASKS[i].spot].id, TASKS[i].flop, it, ex, per, el / 3600000);
  }
  fclose(out);
  fprintf(stderr, "fertig\n");
  return 0;
}
