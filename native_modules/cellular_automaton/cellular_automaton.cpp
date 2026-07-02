// soemdsp-native-module: cellular_automaton
// soemdsp-native-label: Cellular Automaton
// soemdsp-native-target: cellularAutomaton
// soemdsp-native-kind: life

// A tiny "creature" whose whole body is a 1D elementary cellular automaton:
// a row of kWidth cells, each alive or dead, where every next generation is
// computed purely from each cell and its two neighbors under a fixed rule
// (0-255, the standard 8-neighborhood-pattern encoding). No randomness in
// the generation step itself -- same row, same rule, same next row, always.
// Two scalar outputs (Density, Activity) are useful modulation signals on
// their own; X/Y scan out every cell in a scrolling history window so the
// existing scope2d display can paint the classic spacetime diagram without
// any new rendering infrastructure.

namespace {

static const char kMetadataJson[] =
  "{"
    "\"module\":\"cellular_automaton\","
    "\"label\":\"Cellular Automaton\","
    "\"targetType\":\"cellularAutomaton\","
    "\"kind\":\"life\","
    "\"inputs\":[\"Reset\"],"
    "\"outputs\":[\"Density\",\"Activity\",\"X\",\"Y\"],"
    "\"parameters\":["
      "{"
        "\"key\":\"rule\","
        "\"label\":\"Rule\","
        "\"defaultValue\":30,"
        "\"min\":0,"
        "\"mid\":128,"
        "\"max\":255,"
        "\"step\":1,"
        "\"tooltip\":\"Which of the 256 possible 3-neighbor rules governs every generation.\""
      "},"
      "{"
        "\"key\":\"rate\","
        "\"label\":\"Rate\","
        "\"kind\":\"frequency\","
        "\"defaultValue\":4,"
        "\"min\":0.1,"
        "\"mid\":4,"
        "\"max\":60,"
        "\"step\":\"any\","
        "\"unit\":\"Hz\","
        "\"tooltip\":\"How many generations advance per second.\""
      "},"
      "{"
        "\"key\":\"seed\","
        "\"label\":\"Seed\","
        "\"defaultValue\":0,"
        "\"min\":0,"
        "\"mid\":1,"
        "\"max\":99999,"
        "\"step\":1,"
        "\"linearSmoothing\":false,"
        "\"tooltip\":\"Reseeds the starting row. Same seed always reproduces the same life.\""
      "}"
    "]"
  "}";

static const int kWidth = 32;          // cells per generation (fits a uint32 bitmask)
static const int kHistoryRows = 32;    // scrolling window of past generations kept for display
static const int kScanCells = kWidth * kHistoryRows;

struct AutomatonState {
  bool active;
  bool resetWasHigh;

  unsigned int row;                 // current generation, bit i = cell i alive
  unsigned int history[kHistoryRows]; // ring buffer, oldest to newest
  int historyCursor;                // index of the most-recently-written row

  double genPhase;                  // 0..1 accumulator, advances a generation at 1.0
  double density;                   // held between generations, not recomputed every sample
  double activity;

  int scanCursor;                   // 0..kScanCells-1, advances every sample
  double scanX;
  double scanY;

  unsigned int rngState;
};

static AutomatonState gPool[16];
static const int kMaxInstances = 16;

static inline double clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

static unsigned int lcgNext(unsigned int& state) {
  state = state * 1664525u + 1013904223u;
  return state;
}

static int popcount32(unsigned int v) {
  int count = 0;
  while (v) {
    count += (int)(v & 1u);
    v >>= 1;
  }
  return count;
}

static unsigned int nextGeneration(unsigned int row, int rule) {
  unsigned int next = 0;
  for (int i = 0; i < kWidth; i++) {
    const int left = (int)((row >> ((i - 1 + kWidth) % kWidth)) & 1u);
    const int center = (int)((row >> i) & 1u);
    const int right = (int)((row >> ((i + 1) % kWidth)) & 1u);
    const int neighborhood = (left << 2) | (center << 1) | right;
    const int bit = (rule >> neighborhood) & 1;
    if (bit) next |= (1u << i);
  }
  return next;
}

static void reseed(AutomatonState& s, int seed) {
  s.rngState = (unsigned int)(seed * 2654435761u + 1u);
  // A handful of scattered live cells, not just a single center dot -- more
  // interesting starting conditions for most rules than one lonely cell.
  unsigned int row = 0;
  for (int i = 0; i < kWidth; i++) {
    if ((lcgNext(s.rngState) & 0xFFu) < 70u) {  // ~27% of cells start alive
      row |= (1u << i);
    }
  }
  if (row == 0) row = 1u << (kWidth / 2);  // never start fully dead
  s.row = row;
  for (int i = 0; i < kHistoryRows; i++) {
    s.history[i] = 0;
  }
  s.history[0] = s.row;
  s.historyCursor = 0;
  s.genPhase = 0.0;
  s.density = (double)popcount32(s.row) / (double)kWidth;
  s.activity = 0.0;
}

}  // namespace

extern "C" int soemdsp_cellular_automaton_create() {
  for (int i = 0; i < kMaxInstances; i++) {
    if (!gPool[i].active) {
      AutomatonState& s = gPool[i];
      s.active = true;
      s.resetWasHigh = false;
      s.density = 0.0;
      s.activity = 0.0;
      s.scanCursor = 0;
      s.scanX = -1.2;
      s.scanY = -1.2;
      reseed(s, 0);
      return i + 1;
    }
  }
  return 0;
}

extern "C" void soemdsp_cellular_automaton_destroy(int handle) {
  if (handle < 1 || handle > kMaxInstances) return;
  gPool[handle - 1].active = false;
}

extern "C" void soemdsp_cellular_automaton_process(
  int handle,
  double resetSignal,
  double rule,
  double rate,
  double seed,
  double sampleRate
) {
  if (handle < 1 || handle > kMaxInstances) return;
  AutomatonState& s = gPool[handle - 1];

  const double rateHz = clamp(rate, 0.1, 60.0);
  const double safeRate = sampleRate < 1.0 ? 1.0 : sampleRate;
  const int safeRule = (int)clamp(rule, 0.0, 255.0);
  const int safeSeed = (int)clamp(seed, 0.0, 99999.0);

  const bool resetHigh = resetSignal > 0.0;
  if (resetHigh && !s.resetWasHigh) {
    reseed(s, safeSeed);
  }
  s.resetWasHigh = resetHigh;

  s.genPhase += rateHz / safeRate;
  if (s.genPhase >= 1.0) {
    s.genPhase -= 1.0;
    const unsigned int previousRow = s.row;
    s.row = nextGeneration(s.row, safeRule);
    s.historyCursor = (s.historyCursor + 1) % kHistoryRows;
    s.history[s.historyCursor] = s.row;
    s.density = (double)popcount32(s.row) / (double)kWidth;
    s.activity = (double)popcount32(s.row ^ previousRow) / (double)kWidth;
  }

  // Scan out one (row, col) cell per sample, cycling through the whole
  // history window continuously -- fast enough (kScanCells samples per full
  // pass) that the scope2d display's burn/persistence reads as a stable,
  // continuously-refreshing image rather than a flicker.
  s.scanCursor = (s.scanCursor + 1) % kScanCells;
  const int rowSlot = s.scanCursor / kWidth;   // 0 = oldest visible row
  const int col = s.scanCursor % kWidth;
  const int ringIndex = (s.historyCursor + 1 + rowSlot) % kHistoryRows;  // oldest-first order
  const unsigned int rowBits = s.history[ringIndex];
  const bool alive = ((rowBits >> col) & 1u) != 0;
  if (alive) {
    s.scanX = ((double)col / (double)(kWidth - 1)) * 2.0 - 1.0;
    s.scanY = ((double)rowSlot / (double)(kHistoryRows - 1)) * 2.0 - 1.0;
  } else {
    s.scanX = -1.2;
    s.scanY = -1.2;
  }
}

extern "C" double soemdsp_cellular_automaton_density(int handle) {
  if (handle < 1 || handle > kMaxInstances) return 0.0;
  return gPool[handle - 1].density;
}

extern "C" double soemdsp_cellular_automaton_activity(int handle) {
  if (handle < 1 || handle > kMaxInstances) return 0.0;
  return gPool[handle - 1].activity;
}

extern "C" double soemdsp_cellular_automaton_x(int handle) {
  if (handle < 1 || handle > kMaxInstances) return -1.2;
  return gPool[handle - 1].scanX;
}

extern "C" double soemdsp_cellular_automaton_y(int handle) {
  if (handle < 1 || handle > kMaxInstances) return -1.2;
  return gPool[handle - 1].scanY;
}

extern "C" int soemdsp_cellular_automaton_version() {
  return 1;
}

extern "C" const char* soemdsp_cellular_automaton_metadata_json() {
  return kMetadataJson;
}

extern "C" int soemdsp_cellular_automaton_metadata_json_size() {
  return sizeof(kMetadataJson) - 1;
}
