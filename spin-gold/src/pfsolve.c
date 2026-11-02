// pfsolve.c — heads-up NLHE postflop solver (Discounted CFR, vectorised over hands).
// Builds natively (-DNATIVE, for tests) and freestanding for wasm32 (clang --target=wasm32).
// Cards: 0..51 = rank*4 + suit, rank 0 = deuce .. 12 = ace. Players: 0 = OOP, 1 = IP.
#include <stdint.h>
#include <stddef.h>
#ifdef NATIVE
#include <stdlib.h>
#include <string.h>
#define EXPORT(n)
#else
#define EXPORT(n) __attribute__((export_name(n)))
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
#endif

// ---------------- memory ----------------
#ifdef NATIVE
static unsigned char *HEAP = 0; static size_t HEAP_CAP = 0, HEAP_TOP = 0;
static void heap_init(void) { if (!HEAP) { HEAP_CAP = (size_t)5 << 30; HEAP = malloc(HEAP_CAP); } }
static void *xalloc(size_t n) { heap_init(); n = (n + 15) & ~(size_t)15; if (HEAP_TOP + n > HEAP_CAP) return 0; void *p = HEAP + HEAP_TOP; HEAP_TOP += n; return p; }
static void heap_reset(void) { HEAP_TOP = 0; }
static size_t heap_used(void) { return HEAP_TOP; }
#else
extern unsigned char __heap_base;
static uintptr_t HTOP = 0;
static void *xalloc(size_t n) {
  if (!HTOP) HTOP = ((uintptr_t)&__heap_base + 15) & ~(uintptr_t)15;
  n = (n + 15) & ~(size_t)15;
  uintptr_t end = HTOP + n, have = (uintptr_t)__builtin_wasm_memory_size(0) << 16;
  if (end < HTOP) return 0;
  if (end > have) { size_t pages = (end - have + 65535) >> 16; if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return 0; }
  void *p = (void *)HTOP; HTOP = end; return p;
}
static void heap_reset(void) { HTOP = ((uintptr_t)&__heap_base + 15) & ~(uintptr_t)15; }
static size_t heap_used(void) { return HTOP ? HTOP - (uintptr_t)&__heap_base : 0; }
#endif

// ---------------- hand evaluation (7 cards) ----------------
static int top_bit(uint32_t m) { return 31 - __builtin_clz(m); }
static uint32_t straight_hi(uint32_t m) { // 13-bit rank mask -> (index of the straight's top card in the ace-low-extended mask) or 0
  uint32_t e = (m << 1) | ((m >> 12) & 1);
  uint32_t s = e & (e >> 1) & (e >> 2) & (e >> 3) & (e >> 4);
  return s ? (uint32_t)top_bit(s) + 4 : 0;
}
static uint32_t kick(uint32_t m, int k, uint32_t acc) { for (int i = 0; i < k; i++) { int b = top_bit(m); acc = (acc << 4) | (uint32_t)b; m &= ~(1u << b); } return acc; }
static uint32_t eval7(const uint8_t *c) {
  uint32_t sm[4] = {0, 0, 0, 0}; int cnt[13] = {0};
  for (int i = 0; i < 7; i++) { sm[c[i] & 3] |= 1u << (c[i] >> 2); cnt[c[i] >> 2]++; }
  for (int s = 0; s < 4; s++) if (__builtin_popcount(sm[s]) >= 5) {
    uint32_t st = straight_hi(sm[s]); if (st) return (8u << 20) | st;
    return (5u << 20) | kick(sm[s], 5, 0);
  }
  uint32_t all = sm[0] | sm[1] | sm[2] | sm[3];
  int q = -1, t1 = -1, t2 = -1, p1 = -1, p2 = -1;
  for (int r = 12; r >= 0; r--) { int n = cnt[r];
    if (n == 4) q = r; else if (n == 3) { if (t1 < 0) t1 = r; else if (t2 < 0) t2 = r; } else if (n == 2) { if (p1 < 0) p1 = r; else if (p2 < 0) p2 = r; } }
  if (q >= 0) return (7u << 20) | ((uint32_t)q << 4) | (uint32_t)top_bit(all & ~(1u << q));
  if (t1 >= 0 && (t2 >= 0 || p1 >= 0)) { int pr = t2 > p1 ? t2 : p1; return (6u << 20) | ((uint32_t)t1 << 4) | (uint32_t)pr; }
  uint32_t st = straight_hi(all); if (st) return (4u << 20) | st;
  if (t1 >= 0) return (3u << 20) | kick(all & ~(1u << t1), 2, (uint32_t)t1);
  if (p1 >= 0 && p2 >= 0) return (2u << 20) | ((uint32_t)p1 << 8) | ((uint32_t)p2 << 4) | (uint32_t)top_bit(all & ~(1u << p1) & ~(1u << p2));
  if (p1 >= 0) return (1u << 20) | kick(all & ~(1u << p1), 3, (uint32_t)p1);
  return kick(all, 5, 0);
}

// ---------------- tree ----------------
enum { A_FOLD = 0, A_CHECK = 1, A_CALL = 2, A_BET = 3, A_RAISE = 4, A_ALLIN = 5 };
enum { N_ACTION = 0, N_CHANCE = 1, N_FOLD = 2, N_SHOW = 3 };
typedef struct {
  uint8_t type, player, nact, acttype; // player: actor (action) / folder (fold)
  int32_t child;  // first child (children are contiguous)
  int32_t data;   // action: float offset into ARENA; showdown: table id; chance: number of cards
  float actamt;   // bet total on this street after the action that led here
  float c0, c1;   // chips put in by OOP / IP since the start of the solve (incl. current street)
  int16_t card;   // card dealt by the parent chance node, -1 otherwise
  int16_t street; // 0 flop, 1 turn, 2 river
} Node;

static float P0, STK, RAKE_R, RAKE_CAP, ALLIN_THR = .67f, ADD_ALLIN = 1.5f;
static int START_ST, NB0; static uint8_t BOARD0[5];
static float BETS[3][2][4]; static int NBETS[3][2];
static float RAISES[3][4]; static int NRAISES[3];
static int MAXBETS[3];

static int NH[2]; static uint8_t *HC[2]; static float *W[2]; static int32_t *SAME[2]; static int HMAX;
static Node *NODES; static int NN, NNODES; static float *ARENA; static size_t NF, NFLOATS; static int PASS;
static int32_t TBLMAP[52 * 52]; static int NTBL; static uint8_t (*TBLBOARD)[5];
static int32_t *TBL_IDX[2]; static uint32_t *TBL_STR[2]; static int32_t *TBL_OFF[2], *TBL_N[2];
static float *SCRATCH; static size_t SCR_TOP, SCR_CAP;
static float *scr(size_t n) { float *p = SCRATCH + SCR_TOP; SCR_TOP += (n + 3) & ~(size_t)3; return p; }

typedef struct { int street, player, nbets, checked; float b[2], c[2]; uint8_t board[5]; int nb; } St;
static int combo_index(int a, int b) { if (a > b) { int t = a; a = b; b = t; } return b * (b - 1) / 2 + a; }
static int on_board(const uint8_t *bd, int nb, int c) { for (int i = 0; i < nb; i++) if (bd[i] == c) return 1; return 0; }
static float pot_of(const St *s) { return P0 + s->c[0] + s->c[1] + s->b[0] + s->b[1]; }
static float stack_left(const St *s, int p) { return STK - s->c[p] - s->b[p]; }
static int table_for(const uint8_t *bd) {
  int key = START_ST == 0 ? combo_index(bd[3], bd[4]) : START_ST == 1 ? bd[4] : 0;
  int id = TBLMAP[key];
  if (id < 0) { id = TBLMAP[key] = NTBL++; if (PASS == 1) memcpy(TBLBOARD[id], bd, 5); }
  return id;
}
static void build(int ni, St s);
static void set_term(int ni, int type, int player, const St *s) {
  if (PASS == 1) { Node *n = &NODES[ni]; n->type = (uint8_t)type; n->player = (uint8_t)player; n->nact = 0; n->child = 0; n->data = 0; n->c0 = s->c[0] + s->b[0]; n->c1 = s->c[1] + s->b[1]; n->street = (int16_t)s->street; }
}
static void end_street(int ni, St s) {
  s.c[0] += s.b[0]; s.c[1] += s.b[1]; s.b[0] = s.b[1] = 0;
  if (s.street == 2) { set_term(ni, N_SHOW, 0, &s); int id = table_for(s.board); if (PASS == 1) NODES[ni].data = id; return; }
  int ncard = 0; uint8_t cards[52];
  for (int c = 0; c < 52; c++) if (!on_board(s.board, s.nb, c)) cards[ncard++] = (uint8_t)c;
  int first = NN; NN += ncard;
  if (PASS == 1) { Node *n = &NODES[ni]; n->type = N_CHANCE; n->player = 0; n->nact = 0; n->child = first; n->c0 = s.c[0]; n->c1 = s.c[1]; n->street = (int16_t)s.street; n->data = ncard; }
  for (int k = 0; k < ncard; k++) {
    St t = s; t.board[t.nb++] = cards[k]; t.street++; t.player = 0; t.nbets = 0; t.checked = 0;
    int ci = first + k;
    if (PASS == 1) { NODES[ci].card = cards[k]; NODES[ci].acttype = 255; NODES[ci].actamt = 0; }
    if (stack_left(&t, 0) <= 1e-4f || stack_left(&t, 1) <= 1e-4f) end_street(ci, t); else build(ci, t);
  }
}
typedef struct { int type; float amt; } Act;
static int gen_actions(const St *s, Act *out) {
  int p = s->player, o = 1 - p, n = 0;
  float tocall = s->b[o] - s->b[p], left = stack_left(s, p), pot = pot_of(s);
  if (tocall <= 1e-6f) {
    out[n++] = (Act){A_CHECK, s->b[p]};
    if (s->nbets < MAXBETS[s->street]) {
      for (int i = 0; i < NBETS[s->street][p]; i++) {
        float amt = BETS[s->street][p][i] * pot; if (amt < 1) amt = 1;
        if (amt >= ALLIN_THR * left) out[n++] = (Act){A_ALLIN, s->b[p] + left}; else out[n++] = (Act){A_BET, s->b[p] + amt};
      }
      if (left <= ADD_ALLIN * pot) out[n++] = (Act){A_ALLIN, s->b[p] + left};
    }
  } else {
    out[n++] = (Act){A_FOLD, s->b[p]};
    if (left <= tocall + 1e-6f) out[n++] = (Act){A_CALL, s->b[p] + left};
    else {
      out[n++] = (Act){A_CALL, s->b[o]};
      if (s->nbets < MAXBETS[s->street]) {
        float minto = s->b[o] + tocall;
        for (int i = 0; i < NRAISES[s->street]; i++) {
          float to = RAISES[s->street][i] * s->b[o]; if (to < minto) to = minto;
          if (to - s->b[p] >= ALLIN_THR * left) out[n++] = (Act){A_ALLIN, s->b[p] + left}; else out[n++] = (Act){A_RAISE, to};
        }
        if (left <= ADD_ALLIN * (pot + tocall)) out[n++] = (Act){A_ALLIN, s->b[p] + left};
      }
    }
  }
  int m = 0;
  for (int i = 0; i < n; i++) { int dup = 0;
    for (int j = 0; j < m && !dup; j++) {
      if (out[i].type < A_BET || out[j].type < A_BET) dup = out[i].type == out[j].type;
      else dup = (out[i].type == A_ALLIN && out[j].type == A_ALLIN) || (out[j].amt > out[i].amt - .01f && out[j].amt < out[i].amt + .01f);
    }
    if (!dup) out[m++] = out[i]; }
  return m;
}
static void build(int ni, St s) {
  Act acts[16]; int na = gen_actions(&s, acts), p = s.player;
  int first = NN; NN += na;
  size_t off = NF; NF += (size_t)2 * na * NH[p];
  if (PASS == 1) { Node *n = &NODES[ni]; n->type = N_ACTION; n->player = (uint8_t)p; n->nact = (uint8_t)na; n->child = first; n->data = (int32_t)off; n->c0 = s.c[0] + s.b[0]; n->c1 = s.c[1] + s.b[1]; n->street = (int16_t)s.street; }
  for (int k = 0; k < na; k++) {
    int ci = first + k; St t = s; Act a = acts[k];
    if (PASS == 1) { NODES[ci].acttype = (uint8_t)a.type; NODES[ci].actamt = a.amt; NODES[ci].card = -1; }
    switch (a.type) {
      case A_FOLD: set_term(ci, N_FOLD, p, &t); break;
      case A_CHECK: if (p == 0) { t.player = 1; t.checked = 1; build(ci, t); } else end_street(ci, t); break;
      case A_CALL: t.b[p] = a.amt; end_street(ci, t); break;
      default: t.b[p] = a.amt; t.nbets++; t.player = 1 - p; build(ci, t); break;
    }
  }
}

// ---------------- setup ----------------
// cfg (floats): [0] pot, [1] stack, [2] start street, [3] nboard, [4..8] board, [9] rake rate, [10] rake cap,
// [11] all-in threshold, [12] add-all-in ratio; per street s (16 floats from 13+16s):
// maxbets, n_oop, oop[4], n_ip, ip[4], n_raise, raise[4]
static float *CFG, *WIN;
EXPORT("pf_cfg_ptr") float *pf_cfg_ptr(void) { if (!CFG) CFG = xalloc(sizeof(float) * 64); return CFG; }
EXPORT("pf_win_ptr") float *pf_win_ptr(void) { if (!WIN) WIN = xalloc(sizeof(float) * 2 * 1326); return WIN; }
static void read_cfg(void) {
  P0 = CFG[0]; STK = CFG[1]; START_ST = (int)CFG[2]; NB0 = (int)CFG[3];
  for (int i = 0; i < 5; i++) BOARD0[i] = i < NB0 ? (uint8_t)CFG[4 + i] : 0;
  RAKE_R = CFG[9]; RAKE_CAP = CFG[10]; ALLIN_THR = CFG[11] > 0 ? CFG[11] : .67f; ADD_ALLIN = CFG[12];
  for (int s = 0; s < 3; s++) { const float *c = CFG + 13 + 16 * s;
    MAXBETS[s] = (int)c[0]; NBETS[s][0] = (int)c[1]; for (int i = 0; i < 4; i++) BETS[s][0][i] = c[2 + i];
    NBETS[s][1] = (int)c[6]; for (int i = 0; i < 4; i++) BETS[s][1][i] = c[7 + i];
    NRAISES[s] = (int)c[11]; for (int i = 0; i < 4; i++) RAISES[s][i] = c[12 + i]; }
}
static St root_state(void) { St s; memset(&s, 0, sizeof s); s.street = START_ST; s.player = 0; memcpy(s.board, BOARD0, 5); s.nb = NB0; return s; }
static void shell_sort(uint32_t *str, int32_t *idx, int n) {
  static const int gaps[] = {701, 301, 132, 57, 23, 10, 4, 1};
  for (int g = 0; g < 8; g++) { int gap = gaps[g]; for (int i = gap; i < n; i++) { uint32_t s = str[i]; int32_t x = idx[i]; int j = i; while (j >= gap && str[j - gap] > s) { str[j] = str[j - gap]; idx[j] = idx[j - gap]; j -= gap; } str[j] = s; idx[j] = x; } }
}
static void count_tree(void) { for (int i = 0; i < 52 * 52; i++) TBLMAP[i] = -1; NTBL = 0; PASS = 0; NN = 1; NF = 0; build(0, root_state()); }
static int valid_combo(int p, int a, int b) { return WIN[p * 1326 + combo_index(a, b)] > 0 && !on_board(BOARD0, NB0, a) && !on_board(BOARD0, NB0, b); }
static int make_hands(void) {
  for (int p = 0; p < 2; p++) {
    int n = 0; for (int b = 1; b < 52; b++) for (int a = 0; a < b; a++) n += valid_combo(p, a, b);
    NH[p] = n; HC[p] = xalloc((size_t)2 * n + 16); W[p] = xalloc(sizeof(float) * n + 16); SAME[p] = xalloc(sizeof(int32_t) * n + 16);
    if (!HC[p] || !W[p] || !SAME[p]) return -1;
    n = 0;
    for (int b = 1; b < 52; b++) for (int a = 0; a < b; a++) if (valid_combo(p, a, b)) { HC[p][2 * n] = (uint8_t)a; HC[p][2 * n + 1] = (uint8_t)b; W[p][n] = WIN[p * 1326 + combo_index(a, b)]; n++; }
  }
  static int32_t pos[1326];
  for (int p = 0; p < 2; p++) {
    for (int i = 0; i < 1326; i++) pos[i] = -1;
    for (int g = 0; g < NH[1 - p]; g++) pos[combo_index(HC[1 - p][2 * g], HC[1 - p][2 * g + 1])] = g;
    for (int h = 0; h < NH[p]; h++) SAME[p][h] = pos[combo_index(HC[p][2 * h], HC[p][2 * h + 1])];
  }
  HMAX = NH[0] > NH[1] ? NH[0] : NH[1];
  return 0;
}
static double EST_BYTES; static int EST_NODES;
EXPORT("pf_estimate") double pf_estimate(void) {
  read_cfg();
  for (int p = 0; p < 2; p++) { int n = 0; for (int b = 1; b < 52; b++) for (int a = 0; a < b; a++) n += valid_combo(p, a, b); NH[p] = n; }
  count_tree(); EST_NODES = NN;
  EST_BYTES = (double)NN * sizeof(Node) + (double)NF * 4 + (double)NTBL * (NH[0] + NH[1]) * 8 + 8e6;
  return EST_BYTES;
}
EXPORT("pf_est_nodes") int pf_est_nodes(void) { return EST_NODES; }
EXPORT("pf_est_floats") double pf_est_floats(void) { return (double)NF; }
EXPORT("pf_reset") void pf_reset(void) { heap_reset(); CFG = 0; WIN = 0; NODES = 0; ARENA = 0; }
static int ITER; static float *QBUF; static size_t QCAP;
EXPORT("pf_setup") int pf_setup(void) {
  QBUF = 0; QCAP = 0;
  read_cfg();
  if (make_hands()) return -1;
  count_tree(); NNODES = NN; NFLOATS = NF;
  NODES = xalloc(sizeof(Node) * (size_t)NNODES); if (!NODES) return -2;
  ARENA = xalloc(sizeof(float) * NFLOATS + 16); if (!ARENA) return -3;
  memset(ARENA, 0, sizeof(float) * NFLOATS);
  TBLBOARD = xalloc(5 * (size_t)(NTBL + 1)); if (!TBLBOARD) return -4;
  for (int i = 0; i < 52 * 52; i++) TBLMAP[i] = -1; NTBL = 0;
  PASS = 1; NN = 1; NF = 0;
  NODES[0].acttype = 255; NODES[0].card = -1; NODES[0].actamt = 0;
  build(0, root_state());
  for (int p = 0; p < 2; p++) {
    TBL_OFF[p] = xalloc(sizeof(int32_t) * (size_t)(NTBL + 1)); TBL_N[p] = xalloc(sizeof(int32_t) * (size_t)(NTBL + 1));
    TBL_IDX[p] = xalloc(sizeof(int32_t) * (size_t)NTBL * NH[p] + 16); TBL_STR[p] = xalloc(sizeof(uint32_t) * (size_t)NTBL * NH[p] + 16);
    if (!TBL_OFF[p] || !TBL_N[p] || !TBL_IDX[p] || !TBL_STR[p]) return -5;
    int off = 0;
    for (int t = 0; t < NTBL; t++) {
      uint8_t c7[7]; memcpy(c7, TBLBOARD[t], 5); int n = 0;
      for (int h = 0; h < NH[p]; h++) { int a = HC[p][2 * h], b = HC[p][2 * h + 1]; if (on_board(TBLBOARD[t], 5, a) || on_board(TBLBOARD[t], 5, b)) continue;
        c7[5] = (uint8_t)a; c7[6] = (uint8_t)b; TBL_STR[p][off + n] = eval7(c7); TBL_IDX[p][off + n] = h; n++; }
      shell_sort(TBL_STR[p] + off, TBL_IDX[p] + off, n);
      TBL_OFF[p][t] = off; TBL_N[p][t] = n; off += n;
    }
  }
  SCR_CAP = (size_t)64 * 16 * (HMAX + 8); SCRATCH = xalloc(sizeof(float) * SCR_CAP); if (!SCRATCH) return -6;
  ITER = 0;
  return NNODES;
}

// ---------------- CFR ----------------
static float DPOS, DNEG, GAM;
static void card_sums(const float *reach, int o, float *cs, float *tot) {
  for (int c = 0; c < 52; c++) cs[c] = 0; float t = 0;
  for (int g = 0; g < NH[o]; g++) { float r = reach[g]; if (r == 0) continue; t += r; cs[HC[o][2 * g]] += r; cs[HC[o][2 * g + 1]] += r; }
  *tot = t;
}
static float rake_of(float pot) { float r = pot * RAKE_R; return r > RAKE_CAP ? RAKE_CAP : r; }
static void eval_fold(const Node *n, int trav, const float *reach, float *out) {
  int o = 1 - trav; float cs[52], tot; card_sums(reach, o, cs, &tot);
  float ct = trav ? n->c1 : n->c0, co = trav ? n->c0 : n->c1, mn = n->c0 < n->c1 ? n->c0 : n->c1;
  float v = n->player == trav ? -ct : (P0 + co - rake_of(P0 + 2 * mn));
  for (int h = 0; h < NH[trav]; h++) { int sm = SAME[trav][h]; float c = tot - cs[HC[trav][2 * h]] - cs[HC[trav][2 * h + 1]] + (sm >= 0 ? reach[sm] : 0); out[h] = v * c; }
}
static void eval_show(const Node *n, int trav, const float *reach, float *out) {
  int o = 1 - trav, t = n->data;
  const int32_t *it = TBL_IDX[trav] + TBL_OFF[trav][t], *io = TBL_IDX[o] + TBL_OFF[o][t];
  const uint32_t *st = TBL_STR[trav] + TBL_OFF[trav][t], *so = TBL_STR[o] + TBL_OFF[o][t];
  int nt = TBL_N[trav][t], no = TBL_N[o][t];
  float c = trav ? n->c1 : n->c0, rk = rake_of(P0 + n->c0 + n->c1);
  float vw = P0 + c - rk, vl = -c, vt = (P0 - rk) * .5f;
  size_t mark = SCR_TOP; float *win = scr(NH[trav]);
  for (int h = 0; h < NH[trav]; h++) out[h] = 0;
  float cs[52], tot = 0, cst[52], tott; for (int k = 0; k < 52; k++) cs[k] = 0;
  card_sums(reach, o, cst, &tott);
  int j = 0;
  for (int i = 0; i < nt; ) {
    uint32_t s = st[i]; while (j < no && so[j] < s) { int g = io[j]; float r = reach[g]; tot += r; cs[HC[o][2 * g]] += r; cs[HC[o][2 * g + 1]] += r; j++; }
    for (; i < nt && st[i] == s; i++) { int h = it[i]; win[h] = tot - cs[HC[trav][2 * h]] - cs[HC[trav][2 * h + 1]]; }
  }
  tot = 0; for (int k = 0; k < 52; k++) cs[k] = 0; j = no - 1;
  for (int i = nt - 1; i >= 0; ) {
    uint32_t s = st[i]; while (j >= 0 && so[j] > s) { int g = io[j]; float r = reach[g]; tot += r; cs[HC[o][2 * g]] += r; cs[HC[o][2 * g + 1]] += r; j--; }
    for (; i >= 0 && st[i] == s; i--) { int h = it[i]; int a = HC[trav][2 * h], b = HC[trav][2 * h + 1], sm = SAME[trav][h];
      float lose = tot - cs[a] - cs[b], all = tott - cst[a] - cst[b] + (sm >= 0 ? reach[sm] : 0), tie = all - lose - win[h]; if (tie < 0) tie = 0;
      out[h] = vw * win[h] + vl * lose + vt * tie; }
  }
  SCR_TOP = mark;
}
static void regret_match(const float *R, int na, int H, float *sig) {
  for (int h = 0; h < H; h++) { float s = 0; for (int a = 0; a < na; a++) { float r = R[a * H + h]; if (r > 0) s += r; }
    if (s > 0) { float inv = 1 / s; for (int a = 0; a < na; a++) { float r = R[a * H + h]; sig[a * H + h] = r > 0 ? r * inv : 0; } }
    else { float u = 1.f / na; for (int a = 0; a < na; a++) sig[a * H + h] = u; } }
}
static void avg_strat(const float *S, int na, int H, float *sig) {
  for (int h = 0; h < H; h++) { float s = 0; for (int a = 0; a < na; a++) s += S[a * H + h];
    if (s > 0) { float inv = 1 / s; for (int a = 0; a < na; a++) sig[a * H + h] = S[a * H + h] * inv; }
    else { float u = 1.f / na; for (int a = 0; a < na; a++) sig[a * H + h] = u; } }
}
// mode 0: CFR update; 1: best response (traverser maximises, opponent average); 2: value of the average strategies
static void walk(int ni, int trav, const float *reach, float *out, int mode) {
  const Node *n = &NODES[ni]; int o = 1 - trav, Ht = NH[trav], Ho = NH[o];
  if (n->type == N_FOLD) { eval_fold(n, trav, reach, out); return; }
  if (n->type == N_SHOW) { eval_show(n, trav, reach, out); return; }
  size_t mark = SCR_TOP;
  if (n->type == N_CHANCE) {
    int nc = n->data; float *r2 = scr(Ho), *tmp = scr(Ht);
    for (int h = 0; h < Ht; h++) out[h] = 0;
    float f = 1.f / (float)(52 - (3 + n->street) - 4);
    for (int k = 0; k < nc; k++) {
      int ci = n->child + k, c = NODES[ci].card;
      for (int g = 0; g < Ho; g++) r2[g] = (HC[o][2 * g] == c || HC[o][2 * g + 1] == c) ? 0 : reach[g];
      walk(ci, trav, r2, tmp, mode);
      for (int h = 0; h < Ht; h++) if (HC[trav][2 * h] != c && HC[trav][2 * h + 1] != c) out[h] += tmp[h];
    }
    for (int h = 0; h < Ht; h++) out[h] *= f;
    SCR_TOP = mark; return;
  }
  int na = n->nact, p = n->player, H = NH[p];
  float *R = ARENA + n->data, *S = R + (size_t)na * H;
  float *sig = scr((size_t)na * H);
  if (mode == 0) regret_match(R, na, H, sig); else avg_strat(S, na, H, sig);
  if (p == trav) {
    float *cv = scr((size_t)na * H);
    for (int a = 0; a < na; a++) walk(n->child + a, trav, reach, cv + (size_t)a * H, mode);
    if (mode == 1) { for (int h = 0; h < H; h++) { float m = cv[h]; for (int a = 1; a < na; a++) if (cv[a * H + h] > m) m = cv[a * H + h]; out[h] = m; } }
    else { for (int h = 0; h < H; h++) { float v = 0; for (int a = 0; a < na; a++) v += sig[a * H + h] * cv[a * H + h]; out[h] = v; } }
    if (mode == 0)
      for (int a = 0; a < na; a++) { float *Ra = R + (size_t)a * H, *Sa = S + (size_t)a * H, *ca = cv + (size_t)a * H, *sa = sig + (size_t)a * H;
        for (int h = 0; h < H; h++) { float r = Ra[h]; r *= r > 0 ? DPOS : DNEG; Ra[h] = r + ca[h] - out[h]; Sa[h] = Sa[h] * GAM + sa[h]; } }
  } else {
    float *r2 = scr(Ho), *tmp = scr(Ht);
    for (int h = 0; h < Ht; h++) out[h] = 0;
    for (int a = 0; a < na; a++) {
      const float *sa = sig + (size_t)a * Ho; float any = 0;
      for (int g = 0; g < Ho; g++) { float r = reach[g] * sa[g]; r2[g] = r; any += r; }
      if (any <= 0) continue;
      walk(n->child + a, trav, r2, tmp, mode);
      for (int h = 0; h < Ht; h++) out[h] += tmp[h];
    }
  }
  SCR_TOP = mark;
}
EXPORT("pf_iterate") void pf_iterate(void) {
  ITER++; float t = (float)ITER, ta = t * __builtin_sqrtf(t);
  DPOS = ta / (ta + 1); DNEG = .5f; GAM = (t / (t + 1)) * (t / (t + 1));
  for (int trav = 0; trav < 2; trav++) { SCR_TOP = 0; float *out = scr(NH[trav]); walk(0, trav, W[1 - trav], out, 0); }
}
EXPORT("pf_iterations") int pf_iterations(void) { return ITER; }
static double value_of(int trav, int mode) {
  SCR_TOP = 0; float *out = scr(NH[trav]); walk(0, trav, W[1 - trav], out, mode);
  int o = 1 - trav; float cs[52], tot; card_sums(W[o], o, cs, &tot);
  double v = 0, z = 0;
  for (int h = 0; h < NH[trav]; h++) { int sm = SAME[trav][h]; double c = tot - cs[HC[trav][2 * h]] - cs[HC[trav][2 * h + 1]] + (sm >= 0 ? W[o][sm] : 0); v += (double)W[trav][h] * out[h]; z += (double)W[trav][h] * c; }
  return z > 0 ? v / z : 0;
}
static double EVS[2];
EXPORT("pf_exploitability") float pf_exploitability(void) {
  double br0 = value_of(0, 1), br1 = value_of(1, 1); EVS[0] = value_of(0, 2); EVS[1] = value_of(1, 2);
  return (float)(((br0 - EVS[0]) + (br1 - EVS[1])) * .5 / P0 * 100);
}
EXPORT("pf_ev") float pf_ev(int p) { return (float)EVS[p]; }

// ---------------- queries ----------------
EXPORT("pf_num_hands") int pf_num_hands(int p) { return NH[p]; }
EXPORT("pf_hand_cards") uint8_t *pf_hand_cards(int p) { return HC[p]; }
EXPORT("pf_hand_weights") float *pf_hand_weights(int p) { return W[p]; }
EXPORT("pf_num_nodes") int pf_num_nodes(void) { return NNODES; }
EXPORT("pf_mem_used") double pf_mem_used(void) { return (double)heap_used(); }
EXPORT("pf_node_type") int pf_node_type(int i) { return NODES[i].type; }
EXPORT("pf_node_player") int pf_node_player(int i) { return NODES[i].player; }
EXPORT("pf_node_nact") int pf_node_nact(int i) { return NODES[i].type == N_CHANCE ? NODES[i].data : NODES[i].nact; }
EXPORT("pf_node_child") int pf_node_child(int i) { return NODES[i].child; }
EXPORT("pf_node_acttype") int pf_node_acttype(int i) { return NODES[i].acttype; }
EXPORT("pf_node_actamt") float pf_node_actamt(int i) { return NODES[i].actamt; }
EXPORT("pf_node_card") int pf_node_card(int i) { return NODES[i].card; }
EXPORT("pf_node_street") int pf_node_street(int i) { return NODES[i].street; }
EXPORT("pf_node_c0") float pf_node_c0(int i) { return NODES[i].c0; }
EXPORT("pf_node_c1") float pf_node_c1(int i) { return NODES[i].c1; }
EXPORT("pf_pot") float pf_pot(void) { return P0; }
EXPORT("pf_strategy") float *pf_strategy(int i) {
  const Node *n = &NODES[i]; if (n->type != N_ACTION) return 0;
  int na = n->nact, H = NH[n->player]; size_t need = (size_t)na * H;
  if (need > QCAP) { QBUF = xalloc(sizeof(float) * need); QCAP = need; }
  avg_strat(ARENA + n->data + (size_t)na * H, na, H, QBUF); return QBUF;
}
EXPORT("pf_eval7") uint32_t pf_eval7(int a, int b, int c, int d, int e, int f, int g) { uint8_t x[7] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d, (uint8_t)e, (uint8_t)f, (uint8_t)g}; return eval7(x); }
