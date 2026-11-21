/* SPDX-License-Identifier: GPL-3.0-only */
/* ORBITAL for the M-VAVE FM-1, ported from the ORBITAL HTML toy: a poly-rhythmic FM sequencer.
 *
 *   8 lanes       one per scale degree (lane 8 = an octave up), each looping its own length (2-24 steps)
 *   steps         the white keys. Tap: off -> short -> medium -> long -> longest -> off (note length)
 *                 Hold a step: KNOB1-4 lock PITCH / VELOCITY / FM INDEX / SEND to it. EDIT: lock on/off
 *   OCT- / OCT+   steps 1-16 / steps 17-24 on the white keys
 *
 *   black keys    F#3 G#3 A#3 C#4 D#4 F#4 G#4 A#4  lane select (hold one + KNOB1: its length)
 *                 C#5 RANDOM the lane (hold 1 s: every lane)   D#5 CLEAR the lane (with steps held: their locks)
 *                 F#5 hold 1 s: INIT
 *   pages         EDIT voice  ENV envelope  SEL tone  FX space  GLO global. SELECT cycles them
 *   PLAY          start / stop        SAVE (stopped)  store in flash        ALGORITHM  tempo
 *
 * For the chip: up to 12 notes at once (the quietest gives way), fewer when the CPU meter runs high; the
 * echo field is mono, its taps panned apart; the screen shows all eight lanes falling at once.
 * Integers only: the chip has no FPU. */
#include "fm1.h"

#define LANES 8
#define MAXSTEP 24
#define MAXV 12
#define SUB 16
#define BUF_A 32768u /* input history for the three long taps */
#define BUF_B 16384u /* the fed-back tap */

/* ================================================================ settings */

enum { K_RATIO, K_INDEX, K_FB, K_ALGO, K_ATK, K_DECAY, K_SUBLV, K_CUT, K_CONTOUR, K_RESO,
       K_SPACE, K_GRAV, K_TRAIL, K_SCALE, K_ROOT, NK };
enum { U_DIV10, U_PCT, U_ALGO, U_MS, U_SEC100, U_HZ, U_SCALE, U_NOTE };
typedef struct {
  int16_t min, max, step;
  uint8_t unit;
  const char *label;
} kdef_t;
static const kdef_t KD[NK] = {
  {5, 80, 1, U_DIV10, "RATIO"},  {0, 120, 1, U_DIV10, "INDEX"},  {0, 100, 1, U_PCT, "FEEDBK"},
  {0, 1, 1, U_ALGO, "ALGO"},     {1, 300, 1, U_MS, "ATTACK"},    {5, 250, 1, U_SEC100, "DECAY"},
  {0, 100, 1, U_PCT, "SUB"},     {0, 100, 1, U_HZ, "CUTOFF"},    {0, 100, 1, U_PCT, "CONTOUR"},
  {3, 140, 1, U_DIV10, "RESO"},  {0, 100, 1, U_PCT, "SPACE"},    {0, 100, 1, U_PCT, "GRAVITY"},
  {0, 100, 1, U_PCT, "TRAIL"},   {0, 3, 1, U_SCALE, "SCALE"},    {0, 11, 1, U_NOTE, "ROOT"},
};
static const int16_t KDEFAULT[NK] = {20, 35, 15, 0, 6, 60, 20, 65, 50, 15, 40, 55, 60, 0, 0};

static const uint8_t SCALE_LEN[4] = {7, 7, 7, 5};
static const uint8_t SCALES[4][7] = {
  {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 4, 7, 9, 0, 0},
};
static const char *const SCALE_NAME[4] = {"MINOR", "DORIAN", "PHRYG", "PENT"};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static const uint16_t LANE_COL[LANES] = {RGB(124, 92, 255), RGB(138, 108, 255), RGB(152, 120, 255),
                                         RGB(166, 131, 255), RGB(176, 144, 255), RGB(74, 245, 196),
                                         RGB(95, 240, 208),  RGB(116, 235, 218)};
static const uint16_t DUR_COL[4] = {RGB(58, 58, 86), RGB(91, 79, 191), RGB(124, 92, 255), RGB(195, 175, 255)};
static const uint16_t DUR_MULT[4] = {50, 100, 180, 320}; /* x100 */

typedef struct {
  uint8_t dur;    /* 0 off, 1..4 */
  uint8_t locked;
  int8_t pitch;   /* -12..12 */
  uint8_t vel;    /* 10..100 */
  int8_t index;   /* x10, -80..80 */
  uint8_t send;   /* 0..100 */
} step_t;

#define MAGIC 0x4F520001u
typedef struct {
  uint32_t magic;
  uint8_t bpm, sync, pad[2];
  uint8_t len[LANES];
  int16_t k[NK];
  step_t step[LANES][MAXSTEP];
} store_t;
static store_t S;

static void clear_step(step_t *st) {
  st->dur = 0;
  st->locked = 0;
  st->pitch = 0;
  st->vel = 85;
  st->index = 0;
  st->send = 50;
}
static void init_all(void) {
  S.magic = MAGIC;
  S.bpm = 120;
  S.sync = 0;
  for (int i = 0; i < NK; i++) S.k[i] = KDEFAULT[i];
  for (int l = 0; l < LANES; l++) {
    S.len[l] = (uint8_t)(8 + l % 3 * 2);
    for (int s = 0; s < MAXSTEP; s++) clear_step(&S.step[l][s]);
  }
}

/* ================================================================ tables */

static const int16_t TANH10[1025] = {0,320,640,960,1279,1599,1918,2236,2555,2873,3190,3506,3822,4138,4452,4766,5079,5390,5701,6011,6320,6627,6933,7238,7542,7845,8145,8445,8743,9039,9334,9627,9919,10209,10497,10783,11067,11350,11631,11909,12186,12461,12733,13004,13273,13539,13803,14065,14325,14583,14839,15092,15343,15591,15838,16082,16324,16563,16800,17035,17267,17497,17725,17950,18173,18393,18611,18826,19040,19250,19459,19665,19868,20069,20268,20464,20658,20850,21039,21226,21411,21593,21773,21950,22126,22299,22469,22638,22804,22968,23129,23289,23446,23601,23754,23905,24053,24200,24344,24487,24627,24765,24901,25035,25168,25298,25426,25553,25677,25799,25920,26039,26156,26271,26385,26496,26606,26714,26821,26925,27029,27130,27230,27328,27425,27520,27613,27705,27796,27885,27972,28058,28143,28226,28308,28389,28468,28545,28622,28697,28771,28844,28915,28985,29054,29122,29189,29254,29319,29382,29444,29505,29565,29624,29682,29739,29795,29850,29904,29957,30009,30060,30110,30160,30208,30256,30302,30348,30393,30438,30481,30524,30566,30607,30647,30687,30726,30764,30802,30839,30875,30911,30945,30980,31013,31046,31079,31111,31142,31173,31203,31232,31261,31290,31318,31345,31372,31398,31424,31450,31475,31499,31523,31547,31570,31593,31615,31637,31658,31680,31700,31721,31740,31760,31779,31798,31817,31835,31852,31870,31887,31904,31920,31936,31952,31968,31983,31998,32013,32027,32041,32055,32069,32082,32095,32108,32121,32133,32145,32157,32169,32180,32192,32203,32214,32224,32235,32245,32255,32265,32274,32284,32293,32302,32311,32320,32328,32337,32345,32353,32361,32369,32377,32384,32391,32399,32406,32413,32420,32426,32433,32439,32446,32452,32458,32464,32470,32475,32481,32486,32492,32497,32502,32507,32512,32517,32522,32527,32531,32536,32540,32545,32549,32553,32557,32561,32565,32569,32573,32577,32581,32584,32588,32591,32595,32598,32601,32604,32607,32611,32614,32617,32619,32622,32625,32628,32630,32633,32636,32638,32641,32643,32646,32648,32650,32652,32655,32657,32659,32661,32663,32665,32667,32669,32671,32673,32675,32676,32678,32680,32682,32683,32685,32686,32688,32689,32691,32692,32694,32695,32697,32698,32699,32701,32702,32703,32704,32706,32707,32708,32709,32710,32711,32712,32713,32715,32716,32717,32717,32718,32719,32720,32721,32722,32723,32724,32725,32725,32726,32727,32728,32729,32729,32730,32731,32731,32732,32733,32733,32734,32735,32735,32736,32737,32737,32738,32738,32739,32739,32740,32740,32741,32742,32742,32742,32743,32743,32744,32744,32745,32745,32746,32746,32746,32747,32747,32748,32748,32748,32749,32749,32749,32750,32750,32750,32751,32751,32751,32752,32752,32752,32753,32753,32753,32753,32754,32754,32754,32754,32755,32755,32755,32755,32756,32756,32756,32756,32756,32757,32757,32757,32757,32757,32758,32758,32758,32758,32758,32758,32759,32759,32759,32759,32759,32759,32760,32760,32760,32760,32760,32760,32760,32761,32761,32761,32761,32761,32761,32761,32761,32761,32762,32762,32762,32762,32762,32762,32762,32762,32762,32762,32763,32763,32763,32763,32763,32763,32763,32763,32763,32763,32763,32763,32763,32764,32764,32764,32764,32764,32764,32764,32764,32764,32764,32764,32764,32764,32764,32764,32764,32764,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32765,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32766,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767,32767}; /* tanh(i * 10 / 1024), Q15 */
static const uint32_t EXP2[257] = {32768,32857,32946,33035,33125,33215,33305,33395,33486,33576,33667,33759,33850,33942,34034,34126,34219,34312,34405,34498,34591,34685,34779,34874,34968,35063,35158,35253,35349,35445,35541,35637,35734,35831,35928,36025,36123,36221,36319,36417,36516,36615,36715,36814,36914,37014,37114,37215,37316,37417,37518,37620,37722,37824,37927,38030,38133,38236,38340,38444,38548,38653,38757,38863,38968,39074,39180,39286,39392,39499,39606,39714,39821,39929,40037,40146,40255,40364,40473,40583,40693,40804,40914,41025,41136,41248,41360,41472,41584,41697,41810,41923,42037,42151,42265,42380,42495,42610,42726,42841,42958,43074,43191,43308,43425,43543,43661,43780,43898,44017,44137,44256,44376,44497,44617,44738,44859,44981,45103,45225,45348,45471,45594,45718,45842,45966,46091,46216,46341,46467,46593,46719,46846,46973,47100,47228,47356,47484,47613,47742,47871,48001,48131,48262,48393,48524,48655,48787,48920,49052,49185,49319,49452,49586,49721,49856,49991,50126,50262,50399,50535,50672,50810,50947,51085,51224,51363,51502,51642,51782,51922,52063,52204,52346,52488,52630,52773,52916,53059,53203,53347,53492,53637,53782,53928,54074,54221,54368,54515,54663,54811,54960,55109,55258,55408,55558,55709,55860,56012,56163,56316,56468,56622,56775,56929,57083,57238,57393,57549,57705,57861,58018,58176,58333,58491,58650,58809,58968,59128,59289,59449,59611,59772,59934,60097,60260,60423,60587,60751,60916,61081,61247,61413,61579,61746,61914,62081,62250,62419,62588,62757,62928,63098,63269,63441,63613,63785,63958,64132,64306,64480,64655,64830,65006,65182,65359,65536};     /* 2^(i / 256), Q15 */
static const int32_t LOG2_1P7C[101] = {0,6397,12389,18023,23340,28374,33154,37704,42044,46194,50170,53985,57652,61183,64586,67871,71045,74117,77091,79976,82774,85492,88135,90705,93207,95645,98022,100340,102603,104813,106972,109084,111149,113170,115148,117086,118986,120847,122673,124464,126222,127948,129643,131308,132944,134553,136134,137690,139220,140726,142208,143668,145105,146521,147915,149290,150645,151981,153298,154597,155878,157142,158390,159621,160837,162037,163222,164392,165548,166691,167819,168934,170036,171126,172203,173268,174321,175362,176392,177411,178420,179417,180404,181381,182348,183305,184253,185191,186120,187039,187950,188853,189747,190632,191509,192378,193240,194093,194939,195777,196608}; /* log2(1 + 7 c / 100), Q16 */
#define LOG2_250 522046
#define LOG2_12000 888062
#define REL_K 1854286438u /* -ln(0.001), Q28: fall 60 dB over the time given */

static uint32_t exp2_q16(int32_t l) { /* 2^(l / 65536) */
  int i = l >> 16;
  uint32_t v = EXP2[(l >> 8) & 255];
  return i >= 15 ? v << (i - 15) : v >> (15 - i);
}

/* ================================================================ shared */

static volatile uint8_t running;
static volatile int8_t play_pos[LANES] = {-1, -1, -1, -1, -1, -1, -1, -1};
static volatile uint32_t pulses;
static volatile uint16_t load_pct;
static volatile uint8_t vcap = MAXV;

/* ================================================================ the sound */

typedef struct {
  uint8_t active, stage, four;
  uint32_t cph, cinc, mph, minc, m3ph, m3inc, sph, sinc;
  int32_t dev, dev3;          /* deviation at full envelope, in phase-step units */
  int32_t denv, d3env, d2env; /* modulation envelopes, Q24 */
  int32_t dcoef, d3coef, d2coef; /* per slice, Q28 */
  int32_t amp, peak, ainc, rcoef; /* Q24, Q28 */
  int32_t sgain;              /* Q15 */
  int32_t kq, inv;            /* shaper: drive scale, 1 / tanh(drive) Q15 */
  int32_t lfc, lbase, lstep;  /* filter cutoff, log2 Hz Q16, falling per slice */
  int32_t f, q, low, band;
  int16_t gl, gr;             /* pan */
  int32_t send;               /* Q15 */
} voice_t;
static voice_t V[MAXV];

static int16_t buf_a[BUF_A] POOL, buf_b[BUF_B] POOL;
static uint32_t wa, wb;
static int32_t damp_lp;
static int32_t lim_env = 26000;

static int32_t frames(int32_t ms) { return ms * 441 / 10; }
static int32_t coef_slice(int32_t n) { /* decay to 0.001 over n frames, applied once per 16 */
  int32_t sl = n / SUB;
  if (sl < 1) sl = 1;
  return (int32_t)((1u << 28) - REL_K / (uint32_t)sl);
}

static void filter_coefs(voice_t *v) {
  uint32_t fc = exp2_q16(v->lfc);
  if (fc > 12000) fc = 12000;
  v->f = sine(fc * 24348u); /* 2 sin(pi fc / 88200), Q14 (the filter runs twice a frame) */
}

static void fire(int lane, const step_t *st) {
  const int16_t *k = S.k;
  int pick = -1;
  for (int i = 0; i < vcap; i++)
    if (!V[i].active) {
      pick = i;
      break;
    }
  if (pick < 0) { /* the quietest gives way */
    pick = 0;
    for (int i = 1; i < vcap; i++)
      if (V[i].amp < V[pick].amp) pick = i;
  }
  voice_t *v = &V[pick];
  v->active = 0;
  int sc = k[K_SCALE], sl = SCALE_LEN[sc];
  int pitch = st->locked ? st->pitch : 0;
  int vel = st->locked ? st->vel : 85;
  int idx = k[K_INDEX] + (st->locked ? st->index : 0);
  int send = st->locked ? st->send : 50;
  int note = 48 + k[K_ROOT] + SCALES[sc][lane % sl] + 12 * (lane / sl) + pitch;
  v->cinc = note_inc(note);
  v->sinc = note_inc(note - 12);
  v->minc = (uint32_t)(((uint64_t)v->cinc * (uint32_t)k[K_RATIO] * 6554u) >> 16); /* x ratio / 10 */
  int32_t decay = k[K_DECAY] * DUR_MULT[st->dur - 1] / 10; /* ms */
  if (decay < 60) decay = 60;
  int32_t nd = frames(decay);
  v->four = (uint8_t)k[K_ALGO];
  if (!v->four) { /* modulator deviation = freq x index x 0.02 */
    v->dev = (int32_t)((v->cinc >> 8) * idx / 500) << 8; /* 32-bit maths: no 64-bit division on the chip */
    v->dcoef = coef_slice(nd * 9 / 10);
  } else { /* op2 -> carrier: freq x index x 0.018; op3 -> op2: freq x ratio x index x 0.6 x 0.018 */
    int32_t ci = (int32_t)(v->cinc >> 8) * idx / 1000;
    v->dev = (ci * 18) << 8;
    v->dev3 = (ci * k[K_RATIO] * 108 / 100) << 8;
    v->m3inc = (uint32_t)(((uint64_t)v->minc * 3u) >> 1);
    v->dcoef = coef_slice(nd * 85 / 100);
    v->d3coef = coef_slice(nd * 7 / 10);
  }
  v->denv = v->d3env = 1 << 24;
  v->sgain = k[K_SUBLV] * 22938 / 100; /* x 0.7 */
  /* shaper: tanh(x drive) / tanh(drive), drive = 1 + 9 feedback */
  int32_t drive100 = 100 + k[K_FB] * 9;
  v->kq = drive100 * 2097; /* index Q8 = |x| x drive x 102.4 / 32768 = |x| kq >> 18 */
  int32_t di = drive100 * 1024 / 1000;
  v->inv = (int32_t)(1073709056 / TANH10[di > 1024 ? 1024 : di]); /* 32767 x 32768 / tanh */
  /* tone: from base x (1 + 7 contour) down to base over 0.65 decay (min 30 ms) */
  int32_t lbase = LOG2_250 + k[K_CUT] * 3277; /* 250 x 32^v: + 5 v octaves */
  int32_t lpeak = lbase + LOG2_1P7C[k[K_CONTOUR]];
  if (lpeak > LOG2_12000) lpeak = LOG2_12000;
  if (lbase > LOG2_12000) lbase = LOG2_12000;
  int32_t sweep = frames(decay * 65 / 100 > 30 ? decay * 65 / 100 : 30) / SUB;
  v->lfc = lpeak;
  v->lbase = lbase;
  v->lstep = (lpeak - lbase) / (sweep > 0 ? sweep : 1);
  int32_t q = 163840 / k[K_RESO]; /* 1 / Q in Q14 */
  v->q = q > 31130 ? 31130 : q < 1170 ? 1170 : q;
  v->low = v->band = 0;
  filter_coefs(v);
  /* envelope: up in attack, then 60 dB down over decay */
  int32_t peak15 = vel * 16384 / 100; /* vel x 0.5 */
  v->peak = peak15 << 9;
  v->amp = 0;
  int32_t atk = frames(k[K_ATK]);
  v->ainc = v->peak / (atk > 0 ? atk : 1) + 1;
  v->rcoef = (int32_t)((1u << 28) - REL_K / (uint32_t)(nd > 0 ? nd : 1));
  v->stage = 0;
  /* pan by lane, -0.4..0.4, equal power */
  int32_t pan = (lane * 32768 / 7 - 16384) * 8 / 10;
  uint32_t ang = (uint32_t)(pan + 32768) << 14; /* (pan + 1) pi / 4 */
  v->gl = (int16_t)sine(ang + 1073741824u);
  v->gr = (int16_t)sine(ang);
  v->send = send * 19661 / 100; /* x 0.6 */
  v->active = 1;
}

static uint32_t clock_ticks, pulse;

static void do_pulse(void) {
  for (int l = 0; l < LANES; l++) {
    int len = S.len[l] < 2 ? 2 : S.len[l];
    int s = (int)(pulse % (uint32_t)len);
    play_pos[l] = (int8_t)s;
    const step_t *st = &S.step[l][s];
    if (st->dur) fire(l, st);
  }
  pulse++;
  pulses = pulse;
}

static void on_clock(int what) {
  if (what == CLOCK_START) {
    clock_ticks = pulse = 0;
    running = 1;
  } else if (what == CLOCK_CONTINUE) {
    running = 1;
  } else if (what == CLOCK_STOP) {
    running = 0;
    for (int l = 0; l < LANES; l++) play_pos[l] = -1;
  } else if (what == CLOCK_TICK && running) {
    if (clock_ticks % 6 == 0) do_pulse();
    clock_ticks++;
  }
}

static inline int32_t shaper(const voice_t *v, int32_t x) {
  if (x > 32767) x = 32767;
  if (x < -32767) x = -32767;
  int32_t ax = x < 0 ? -x : x;
  int32_t iq = (int32_t)(((int64_t)ax * v->kq) >> 18);
  int32_t i = iq >> 8, fr = iq & 255;
  int32_t t = i >= 1024 ? 32767 : TANH10[i] + (((TANH10[i + 1] - TANH10[i]) * fr) >> 8);
  t = (t * v->inv) >> 15;
  return x < 0 ? -t : t;
}

static void render(int32_t *out, int32_t *send, uint32_t n) {
  for (int vi = 0; vi < MAXV; vi++) {
    voice_t *v = &V[vi];
    if (!v->active) continue;
    /* once a slice: modulation envelopes and the filter sweep */
    v->denv = (int32_t)(((int64_t)v->denv * v->dcoef) >> 28);
    int32_t dev = (int32_t)(((int64_t)v->dev * (v->denv >> 9)) >> 15);
    int32_t dev3 = 0;
    if (v->four) {
      v->d3env = (int32_t)(((int64_t)v->d3env * v->d3coef) >> 28);
      dev3 = (int32_t)(((int64_t)v->dev3 * (v->d3env >> 9)) >> 15);
    }
    if (v->lfc > v->lbase) {
      v->lfc -= v->lstep;
      if (v->lfc < v->lbase) v->lfc = v->lbase;
      filter_coefs(v);
    }
    int32_t amp = v->amp;
    for (uint32_t f = 0; f < n; f++) {
      if (v->stage == 0) {
        amp += v->ainc;
        if (amp >= v->peak) amp = v->peak, v->stage = 1;
      } else {
        amp = (int32_t)(((int64_t)amp * v->rcoef) >> 28);
        if (amp < 4096) {
          v->active = 0;
          break;
        }
      }
      uint32_t minc = v->minc;
      if (v->four) {
        minc += (uint32_t)((sine(v->m3ph) * (int64_t)dev3) >> 15);
        v->m3ph += v->m3inc;
      }
      int32_t m = sine(v->mph);
      v->mph += minc;
      v->cph += v->cinc + (uint32_t)(int32_t)(((int64_t)m * dev) >> 15);
      int32_t x = sine(v->cph);
      if (v->sgain) {
        x += (sine(v->sph) * v->sgain) >> 15;
        v->sph += v->sinc;
      }
      x = shaper(v, x) >> 4; /* +-2048 into the filter */
      for (int o = 0; o < 2; o++) {
        v->low += (v->f * v->band) >> 14;
        int32_t high = x - v->low - ((v->q * v->band) >> 14);
        v->band += (v->f * high) >> 14;
      }
      int32_t y = v->low << 4;
      if (y > 65534) y = 65534;
      if (y < -65534) y = -65534;
      y = (y * (amp >> 9)) >> 15;
      int32_t l = (y * v->gl) >> 15, r = (y * v->gr) >> 15;
      out[2 * f] += l;
      out[2 * f + 1] += r;
      send[f] += (((l + r) >> 1) * v->send) >> 15;
    }
    v->amp = amp;
  }
}

static int32_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

void audio(int32_t *out, uint32_t nframes) {
  uint32_t t0 = micros();
  const clock_event_t *ev;
  int nev = clock_events(&ev), e = 0;
  const int16_t *k = S.k;
  static int32_t sendb[256];
  /* the echo field: four taps of one send; the shortest is fed back through a damping low-pass */
  int32_t gsend = 8192 + k[K_TRAIL] * 164;  /* 0.25 + 0.5 trail */
  int32_t gout = 8192 + k[K_SPACE] * 229;   /* 0.25 + 0.7 space */
  int32_t gfb = 9830 + k[K_GRAV] * 203;     /* 0.3 + 0.62 gravity */
  int32_t fc = 600 + (100 - k[K_TRAIL]) * 40;
  int32_t w = fc * 4669 / 1000;             /* 2 pi fc / 44100, Q15 */
  int32_t a = w - ((w * w) >> 16) + ((((w * w) >> 15) * w) >> 15) / 6; /* 1 - e^-w */

  for (uint32_t at = 0; at < nframes; at += SUB) {
    uint32_t n = nframes - at < SUB ? nframes - at : SUB;
    while (e < nev && ev[e].at < at + n) on_clock(ev[e++].what);
    for (uint32_t f = 0; f < n; f++) sendb[f] = 0;
    int32_t *o = out + 2 * at;
    render(o, sendb, n);
    for (uint32_t f = 0; f < n; f++) {
      buf_a[wa & (BUF_A - 1)] = (int16_t)((clamp16(sendb[f]) * gsend) >> 15);
      int32_t y0 = buf_b[(wb - 9305u) & (BUF_B - 1)];
      int32_t y1 = buf_a[(wa - 14862u) & (BUF_A - 1)];
      int32_t y2 = buf_a[(wa - 18478u) & (BUF_A - 1)];
      int32_t y3 = buf_a[(wa - 23064u) & (BUF_A - 1)];
      int32_t sum = y0 + y1 + y2 + y3;
      damp_lp += (int32_t)(((int64_t)((((sum >> 2) * gfb) >> 13) - damp_lp) * a) >> 15);
      buf_b[wb & (BUF_B - 1)] = (int16_t)clamp16(buf_a[wa & (BUF_A - 1)] + damp_lp);
      wa++;
      wb++;
      int32_t fl = (((y0 * 3 + y1 + y2 * 3 + y3) >> 2) * gout) >> 15; /* taps leaning left, right, left, right */
      int32_t fr = (((y0 + y1 * 3 + y2 + y3 * 3) >> 2) * gout) >> 15;
      int32_t l = (int32_t)((((((int64_t)o[2 * f] * 27853) >> 15) + fl) * 29491) >> 15); /* dry 0.85, master 0.9 */
      int32_t r = (int32_t)((((((int64_t)o[2 * f + 1] * 27853) >> 15) + fr) * 29491) >> 15);
      int32_t pk = l < 0 ? -l : l, pr = r < 0 ? -r : r;
      if (pr > pk) pk = pr;
      if (pk > lim_env) lim_env = pk;
      else lim_env -= (lim_env >> 13) + 1;
      if (lim_env < 26000) lim_env = 26000;
      int32_t g = (26000 * 32768) / lim_env;
      o[2 * f] = (l * g) >> 15;
      o[2 * f + 1] = (r * g) >> 15;
    }
  }
  uint32_t used = micros() - t0, budget = nframes * 22676u / 1000u;
  load_pct = (uint16_t)((load_pct * 7u + (budget ? used * 100u / budget : 0)) / 8u);
}

/* ================================================================ the panel */

static int8_t white_of[KEY_COUNT], black_of[KEY_COUNT];
enum { BK_RND = 8, BK_CLR = 9, BK_INIT = 10 };
enum { PG_VOICE, PG_ENV, PG_TONE, PG_SPACE, PG_GLO, PAGES };
static const char *const PAGE_NAME[PAGES] = {"VOICE", "ENV", "TONE", "SPACE", "GLO"};
#define X_BPM 100
#define X_SYNC 101
static const uint8_t PAGE_K[PAGES][4] = {
  {K_RATIO, K_INDEX, K_FB, K_ALGO}, {K_ATK, K_DECAY, K_SUBLV, 255}, {K_CUT, K_CONTOUR, K_RESO, 255},
  {K_SPACE, K_GRAV, K_TRAIL, 255},  {X_BPM, K_SCALE, K_ROOT, X_SYNC},
};

static uint8_t lane, page, spage, playing;
static uint32_t held; /* steps held (0..23) */
static uint8_t held_used;
static uint32_t rnd_since, init_since;
static char msg[28];
static uint32_t msg_until;
enum { D_HEAD = 1, D_LANES = 2, D_PAGES = 4, D_CTX = 8, D_PARAMS = 16 };
static uint8_t dirty = 0xFF;

#define C_BG RGB(7, 7, 13)
#define C_SURF RGB(20, 20, 31)
#define C_SURF2 RGB(28, 28, 43)
#define C_LINE RGB(40, 40, 64)
#define C_TEXT RGB(232, 230, 240)
#define C_DIM RGB(139, 138, 158)
#define C_ACCENT RGB(124, 92, 255)
#define C_LOCK RGB(74, 245, 196)
#define C_WARN RGB(255, 107, 157)

static void say(const char *s) {
  int i = 0;
  for (; s[i] && i < 27; i++) msg[i] = s[i];
  msg[i] = 0;
  msg_until = millis() + 1400;
  dirty |= D_CTX;
}
static char *put(char *d, const char *s) {
  while (*s) *d++ = *s++;
  *d = 0;
  return d;
}
static char *put_int(char *d, int32_t v) {
  char b[12];
  format_int(b, v);
  return put(d, b);
}
static char *put_dec(char *d, int32_t v, int places) { /* v / 10^places */
  int32_t div = places == 1 ? 10 : 100;
  if (v < 0) *d++ = '-', v = -v;
  d = put_int(d, v / div);
  *d++ = '.';
  if (places == 2) *d++ = (char)('0' + v / 10 % 10);
  *d++ = (char)('0' + v % 10);
  *d = 0;
  return d;
}
static void center(int x, int w, int y, const char *s, uint16_t c, int font) {
  text(x + (w - text_width(s, font)) / 2, y, s, c, font);
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void fmt_k(int i, int v, char *val, const char **unit) {
  *unit = "";
  switch (KD[i].unit) {
    case U_DIV10: put_dec(val, v, 1); break;
    case U_PCT: put_int(val, v), *unit = "%"; break;
    case U_ALGO: put(val, v ? "4-OP" : "2-OP"); break;
    case U_MS: put_int(val, v), *unit = "MS"; break;
    case U_SEC100: put_dec(val, v, 2), *unit = "S"; break;
    case U_HZ: {
      uint32_t hz = exp2_q16(LOG2_250 + v * 3277);
      if (hz >= 1000) put_dec(val, (int32_t)(hz / 100), 1), *unit = "KHZ";
      else put_int(val, (int32_t)hz), *unit = "HZ";
      break;
    }
    case U_SCALE: put(val, SCALE_NAME[clampi(v, 0, 3)]); break;
    case U_NOTE: put(val, NOTE_NAME[clampi(v, 0, 11)]); break;
    default: val[0] = 0;
  }
}

/* ---- screen: eight lanes falling, all at once */
#define LANE_Y 40
#define CELL_P 5
static int8_t drawn_ph[LANES] = {-1, -1, -1, -1, -1, -1, -1, -1};

static void draw_cell(int l, int s) {
  int x = l * 30 + 2, y = LANE_Y + s * CELL_P;
  uint16_t bg = l == lane ? C_SURF : C_BG;
  rect(x, y, 26, CELL_P - 1, bg);
  if (s >= S.len[l]) return;
  const step_t *st = &S.step[l][s];
  int ph = playing ? play_pos[l] : -1;
  rect(x, y, 26, CELL_P - 1, s == ph ? C_LINE : C_SURF2);
  if (st->dur) {
    static const uint8_t W[4] = {9, 14, 19, 25};
    int w = W[st->dur - 1];
    rect(x + (26 - w) / 2, y, w, CELL_P - 1, s == ph ? C_TEXT : DUR_COL[st->dur - 1]);
  }
  if (st->locked && st->dur) rect(x + 24, y, 2, CELL_P - 1, C_LOCK);
  if (l == lane && (held >> s & 1)) frame(x - 1, y - 1, 28, CELL_P + 1, C_WARN);
}

static void draw_lane(int l) {
  int x = l * 30 + 1;
  uint16_t bg = l == lane ? C_SURF : C_BG;
  rect(x, 20, 28, LANE_Y + MAXSTEP * CELL_P - 18, bg);
  char b[6];
  put_int(b, S.len[l]);
  rect(x + 2, 24, 5, 5, LANE_COL[l]);
  text(x + 9, 19, b, l == lane ? C_TEXT : C_DIM, FONT_SMALL);
  for (int s = 0; s < MAXSTEP; s++) draw_cell(l, s);
  if (l == lane) { /* which steps the white keys reach now */
    int s0 = spage ? 16 : 0, s1 = spage ? MAXSTEP : 16;
    rect(x, LANE_Y + s0 * CELL_P, 1, (s1 - s0) * CELL_P - 1, C_WARN);
  }
  drawn_ph[l] = (int8_t)(playing ? play_pos[l] : -1);
}

static void draw_head(void) {
  char b[24], *d;
  rect(0, 0, SCREEN_W, 18, C_BG);
  text(4, 1, "ORBITAL", C_TEXT, FONT_SMALL);
  text(64, 1, spage ? "17-24" : "1-16", C_WARN, FONT_SMALL);
  d = put(b, "CPU ");
  d = put_int(d, load_pct);
  put(d, "%");
  text(112, 1, b, load_pct > 80 ? C_WARN : C_DIM, FONT_SMALL);
  put_int(b, S.bpm);
  text(SCREEN_W - 4 - text_width(b, FONT_SMALL), 1, b, playing ? C_ACCENT : C_DIM, FONT_SMALL);
}

static void draw_pages(void) {
  rect(0, 162, SCREEN_W, 18, C_BG);
  for (int i = 0; i < PAGES; i++) {
    int x = i * 48;
    if (i == page) frame(x + 2, 162, 44, 18, C_ACCENT);
    center(x + 2, 44, 163, PAGE_NAME[i], i == page ? C_TEXT : C_DIM, FONT_SMALL);
  }
}

static int first_held(void) {
  for (int s = 0; s < MAXSTEP; s++)
    if (held >> s & 1) return s;
  return -1;
}
static int lane_key_held(void) {
  for (int k = 0; k < KEY_COUNT; k++)
    if (black_of[k] >= 0 && black_of[k] < LANES && key_held(k)) return black_of[k];
  return -1;
}

static void draw_ctx(void) {
  char b[32], *d;
  rect(0, 182, SCREEN_W, 18, C_BG);
  if (msg[0] && (int32_t)(msg_until - millis()) > 0) {
    center(0, SCREEN_W, 183, msg, C_LOCK, FONT_SMALL);
    return;
  }
  msg[0] = 0;
  int s = first_held(), lk = lane_key_held();
  if (lk >= 0) {
    d = put(b, "LANE ");
    d = put_int(d, lk + 1);
    put(d, ": KNOB1 = LENGTH");
    center(0, SCREEN_W, 183, b, C_DIM, FONT_SMALL);
  } else if (s >= 0) {
    d = put(b, "LANE ");
    d = put_int(d, lane + 1);
    d = put(d, " STEP ");
    d = put_int(d, s + 1);
    put(d, S.step[lane][s].locked ? " LOCKED" : " FREE");
    center(0, SCREEN_W, 183, b, S.step[lane][s].locked ? C_LOCK : C_DIM, FONT_SMALL);
  } else {
    int sc = S.k[K_SCALE];
    d = put(b, "LANE ");
    d = put_int(d, lane + 1);
    d = put(d, "  ");
    d = put(d, NOTE_NAME[(S.k[K_ROOT] + SCALES[sc][lane % SCALE_LEN[sc]]) % 12]);
    put(d, lane / SCALE_LEN[sc] ? " +OCT" : "");
    center(0, SCREEN_W, 183, b, LANE_COL[lane], FONT_SMALL);
  }
}

static void draw_param(int i, const char *label, const char *val, const char *unit, uint16_t c) {
  int x = i * 60;
  rect(x, 202, 60, 38, C_BG);
  if (!label[0]) return;
  center(x, 60, 202, label, C_DIM, FONT_SMALL);
  char b[20], *d = put(b, val);
  put(d, unit);
  center(x, 60, 220, b, c, FONT_SMALL);
}

static void draw_params(void) {
  char v[16];
  const char *u;
  int s = first_held(), lk = lane_key_held();
  if (lk >= 0) {
    put_int(v, S.len[lk]);
    draw_param(0, "LENGTH", v, "", LANE_COL[lk]);
    for (int i = 1; i < 4; i++) draw_param(i, "", "", "", 0);
    return;
  }
  if (s >= 0) {
    const step_t *st = &S.step[lane][s];
    uint16_t c = st->locked ? C_LOCK : C_DIM;
    char *d = v;
    if (st->pitch > 0) *d++ = '+';
    put_int(d, st->pitch);
    draw_param(0, "PITCH", v, "ST", c);
    put_dec(v, st->vel, 2);
    draw_param(1, "VEL", v, "", c);
    d = v;
    if (st->index >= 0) *d++ = '+';
    put_dec(d, st->index, 1);
    draw_param(2, "INDEX", v, "", c);
    put_dec(v, st->send, 2);
    draw_param(3, "SEND", v, "", c);
    return;
  }
  for (int i = 0; i < 4; i++) {
    int id = PAGE_K[page][i];
    if (id == 255) draw_param(i, "", "", "", 0);
    else if (id == X_BPM) put_int(v, S.bpm), draw_param(i, "TEMPO", v, "", C_ACCENT);
    else if (id == X_SYNC) draw_param(i, "SYNC", S.sync ? "EXT" : "INT", "", C_ACCENT);
    else fmt_k(id, S.k[id], v, &u), draw_param(i, KD[id].label, v, u, C_ACCENT);
  }
}

/* ---- edits */
static int accel(int e, int32_t det) {
  static uint32_t last[ENC_COUNT];
  uint32_t now = millis(), dt = now - last[e];
  if (!det) return 0;
  last[e] = now;
  return det * (dt < 25 ? 4 : dt < 60 ? 2 : 1);
}

static void edit_k(int id, int d) {
  if (id == 255 || !d) return;
  if (id == X_BPM) {
    S.bpm = (uint8_t)clampi(S.bpm + d, 40, 220);
    clock_tempo((uint32_t)S.bpm * 100u);
    dirty |= D_HEAD;
  } else if (id == X_SYNC) {
    S.sync = d > 0;
    clock_follow(S.sync ? MIDI_ANY : 0);
  } else {
    int dd = (KD[id].unit == U_ALGO || KD[id].unit == U_SCALE || KD[id].unit == U_NOTE) ? (d > 0 ? 1 : -1) : d;
    S.k[id] = (int16_t)clampi(S.k[id] + dd * KD[id].step, KD[id].min, KD[id].max);
    if (id == K_SCALE || id == K_ROOT) dirty |= D_CTX;
  }
  dirty |= D_PARAMS;
}

static void edit_lock(int k, int d) {
  for (int s = 0; s < MAXSTEP; s++) {
    if (!(held >> s & 1)) continue;
    step_t *st = &S.step[lane][s];
    if (!st->dur) continue;
    if (k == 0) st->pitch = (int8_t)clampi(st->pitch + (d > 0 ? 1 : -1), -12, 12);
    if (k == 1) st->vel = (uint8_t)clampi(st->vel + d, 10, 100);
    if (k == 2) st->index = (int8_t)clampi(st->index + d, -80, 80);
    if (k == 3) st->send = (uint8_t)clampi(st->send + d, 0, 100);
    st->locked = 1;
  }
  held_used = 1;
  dirty |= D_LANES | D_PARAMS | D_CTX;
}

static void randomize(int l) { /* downbeats likely, other steps sparse; random lengths, longer ones rarer */
  int len = S.len[l];
  for (int s = 0; s < MAXSTEP; s++) clear_step(&S.step[l][s]);
  for (int s = 0; s < len; s++) {
    int w = len % 4 == 0 ? (s % 4 == 0 ? 70 : s % 2 == 0 ? 35 : 12) : (s % 2 == 0 ? 40 : 15);
    if ((int)(random32() % 100u) < w) {
      uint32_t r = random32() % 100u;
      S.step[l][s].dur = (uint8_t)(r < 45 ? 1 : r < 75 ? 2 : r < 92 ? 3 : 4);
    }
  }
}

static void step_tapped(int s) {
  step_t *st = &S.step[lane][s];
  if (st->dur < 4 && st->dur) st->dur++;
  else if (!st->dur) st->dur = 1;
  else clear_step(st);
}

static void set_page(int p) {
  page = (uint8_t)((p + PAGES) % PAGES);
  dirty |= D_PAGES | D_PARAMS | D_CTX;
}

static void black_down(int bk) {
  if (bk < LANES) {
    int old = lane;
    lane = (uint8_t)bk;
    held = 0;
    draw_lane(old);
    draw_lane(lane);
    dirty |= D_CTX | D_PARAMS;
    return;
  }
  if (bk == BK_RND) {
    randomize(lane);
    rnd_since = millis() | 1;
    say("RANDOM (HOLD: ALL LANES)");
  } else if (bk == BK_CLR) {
    if (held) {
      for (int s = 0; s < MAXSTEP; s++)
        if (held >> s & 1) {
          step_t *st = &S.step[lane][s];
          uint8_t dur = st->dur;
          clear_step(st);
          st->dur = dur;
        }
      held_used = 1;
      say("LOCKS CLEARED");
    } else {
      for (int s = 0; s < MAXSTEP; s++) clear_step(&S.step[lane][s]);
      say("LANE CLEARED");
    }
  } else if (bk == BK_INIT) {
    init_since = millis() | 1;
    say("HOLD FOR INIT");
  }
  dirty |= D_LANES | D_PARAMS;
}

void setup(void) {
  static const uint8_t IS_BLACK[12] = {0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0}; /* from F */
  int w = 0, b = 0;
  for (int k = 0; k < KEY_COUNT; k++) {
    white_of[k] = black_of[k] = -1;
    if (IS_BLACK[k % 12]) black_of[k] = (int8_t)b++;
    else white_of[k] = (int8_t)w++;
  }
  if (settings_load(&S, sizeof S) != 0 || S.magic != MAGIC) init_all();
  clear(C_BG);
  clock_tempo((uint32_t)S.bpm * 100u);
  clock_send(1);
  clock_follow(S.sync ? MIDI_ANY : 0);
  led_backlight(1);
  print("ORBITAL ready");
}

void loop(void) {
  uint32_t now = millis();
  for (int k = 0; k < KEY_COUNT; k++) {
    if (key_pressed(k)) {
      if (white_of[k] >= 0) {
        int s = white_of[k] + (spage ? 16 : 0);
        if (s < MAXSTEP) {
          if (!held) held_used = 0;
          held |= 1u << s;
          draw_cell(lane, s);
          dirty |= D_CTX | D_PARAMS;
        }
      } else {
        black_down(black_of[k]);
      }
    }
    if (key_released(k)) {
      if (white_of[k] >= 0) {
        int s = white_of[k] + (spage ? 16 : 0);
        if (s < MAXSTEP && (held >> s & 1)) {
          held &= ~(1u << s);
          if (!held_used && s < S.len[lane]) step_tapped(s);
          draw_cell(lane, s);
          dirty |= D_CTX | D_PARAMS;
        }
      } else {
        if (black_of[k] == BK_RND) rnd_since = 0;
        if (black_of[k] == BK_INIT) init_since = 0;
        dirty |= D_CTX | D_PARAMS;
      }
    }
  }
  if (rnd_since && now - rnd_since > 1000) {
    rnd_since = 0;
    for (int l = 0; l < LANES; l++) randomize(l);
    say("ALL LANES RANDOM");
    dirty |= D_LANES;
  }
  if (init_since && now - init_since > 1000) {
    init_since = 0;
    if (playing) playing = 0, clock_stop();
    init_all();
    clock_tempo((uint32_t)S.bpm * 100u);
    held = 0;
    dirty = 0xFF;
    say("INIT");
  }
  if (button_pressed(BTN_PLAY)) {
    playing = !playing;
    playing ? clock_start() : clock_stop();
    dirty |= D_HEAD | D_LANES;
  }
  if (button_pressed(BTN_SAVE)) {
    if (playing) say("STOP TO SAVE");
    else say(settings_save(&S, sizeof S) == 0 ? "SAVED" : "SAVE FAILED");
  }
  if (button_pressed(BTN_EDIT)) {
    if (held) {
      int s0 = first_held(), on = !S.step[lane][s0].locked;
      for (int s = 0; s < MAXSTEP; s++)
        if ((held >> s & 1) && S.step[lane][s].dur) S.step[lane][s].locked = (uint8_t)on;
      held_used = 1;
      dirty |= D_LANES | D_CTX | D_PARAMS;
    } else {
      set_page(PG_VOICE);
    }
  }
  if (button_pressed(BTN_ENV)) set_page(PG_ENV);
  if (button_pressed(BTN_SEL)) set_page(PG_TONE);
  if (button_pressed(BTN_FX)) set_page(PG_SPACE);
  if (button_pressed(BTN_GLO)) set_page(PG_GLO);
  if (button_pressed(BTN_HOME)) set_page(PG_VOICE);
  if (button_pressed(BTN_OCT_DOWN) || button_pressed(BTN_OCT_UP)) {
    spage = (uint8_t)button_pressed(BTN_OCT_UP);
    held = 0;
    dirty |= D_HEAD | D_LANES;
  }
  int sel = encoder(ENC_SELECT);
  if (sel) set_page(page + (sel > 0 ? 1 : -1));
  int al = accel(ENC_ALGORITHM, encoder(ENC_ALGORITHM));
  if (al) edit_k(X_BPM, al);
  int pr = encoder(ENC_PRESETS); /* PRESETS: lane select, a quick alternative to the black keys */
  if (pr) black_down(clampi(lane + (pr > 0 ? 1 : -1), 0, LANES - 1));
  int lk = lane_key_held();
  for (int k = 0; k < 4; k++) {
    int d = accel(ENC_KNOB1 + k, encoder(ENC_KNOB1 + k));
    if (!d) continue;
    if (lk >= 0) {
      if (k == 0) S.len[lk] = (uint8_t)clampi(S.len[lk] + (d > 0 ? 1 : -1), 2, MAXSTEP), dirty |= D_LANES | D_PARAMS;
    } else if (held) {
      edit_lock(k, d);
    } else {
      edit_k(PAGE_K[page][k], d);
    }
  }
  if (S.sync && playing != running) playing = running, dirty |= D_HEAD | D_LANES;

  /* CPU guard: fewer notes at once when the chip runs short */
  static uint32_t over_since, under_since, head_at;
  uint16_t load = load_pct;
  if (load > 85) {
    if (!over_since) over_since = now | 1;
    if (now - over_since > 200 && vcap > 6) vcap = (uint8_t)(vcap - 2), over_since = 0, say("CPU: FEWER NOTES");
  } else {
    over_since = 0;
  }
  if (load < 45 && vcap < MAXV) {
    if (!under_since) under_since = now | 1;
    if (now - under_since > 3000) vcap = (uint8_t)(vcap + 2), under_since = 0;
  } else {
    under_since = 0;
  }
  if (now - head_at > 500) head_at = now, dirty |= D_HEAD;
  if (msg[0] && (int32_t)(msg_until - now) <= 0) dirty |= D_CTX;

  if (dirty & D_HEAD) draw_head();
  if (dirty & D_LANES) {
    for (int l = 0; l < LANES; l++) draw_lane(l);
  } else { /* only the playheads that moved */
    for (int l = 0; l < LANES; l++) {
      int ph = playing ? play_pos[l] : -1;
      if (ph != drawn_ph[l]) {
        int old = drawn_ph[l];
        drawn_ph[l] = (int8_t)ph;
        if (old >= 0) draw_cell(l, old);
        if (ph >= 0) draw_cell(l, ph);
      }
    }
  }
  if (dirty & D_PAGES) draw_pages();
  if (dirty & D_CTX) draw_ctx();
  if (dirty & D_PARAMS) draw_params();
  dirty = 0;

  int ph = playing ? play_pos[lane] : -1;
  for (int k = 0; k < KEY_COUNT; k++) {
    int on;
    if (white_of[k] >= 0) {
      int s = white_of[k] + (spage ? 16 : 0);
      on = s < S.len[lane] && ((S.step[lane][s].dur != 0) ^ (s == ph));
    } else {
      on = black_of[k] == lane;
    }
    led_key(k, on);
  }
  led_button(BTN_PLAY, playing);
  led_button(BTN_OCT_DOWN, !spage);
  led_button(BTN_OCT_UP, spage);
  led_button(BTN_EDIT, page == PG_VOICE);
  led_button(BTN_ENV, page == PG_ENV);
  led_button(BTN_SEL, page == PG_TONE);
  led_button(BTN_FX, page == PG_SPACE);
  led_button(BTN_GLO, page == PG_GLO);
}
