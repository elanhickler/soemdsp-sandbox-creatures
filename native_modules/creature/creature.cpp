// soemdsp-native-module: creature
// soemdsp-native-label: Creature
// soemdsp-native-target: creature
// soemdsp-native-kind: life

// A little electrified Tamagotchi that eats the loudness of whatever signal
// you feed it. One control-rate process() call per sample derives a handful
// of cheap running stats (level in dB, a slow "expected" baseline, how much
// the level has been bouncing around, how long it's been sitting near clip,
// how harsh/rail-riding the raw waveform itself is) and folds them into two
// independent readouts -- Hunger and Health -- plus a single discrete Mood
// (Peaceful/Sad/Happy/Excited/Hungry/Angry/Fear/Meltdown), picked by a
// priority chain so exactly one mood shows at a time instead of a jittery
// blend.

namespace {

static const char kMetadataJson[] =
  "{"
    "\"module\":\"creature\","
    "\"label\":\"Creature\","
    "\"targetType\":\"creature\","
    "\"kind\":\"life\","
    "\"inputs\":[\"In\"],"
    "\"outputs\":[\"Hunger\",\"Health\",\"Mood\",\"Alive\"],"
    "\"parameters\":["
      "{"
        "\"key\":\"comfortLow\","
        "\"label\":\"Comfort Low\","
        "\"defaultValue\":-24,"
        "\"min\":-96,"
        "\"mid\":-24,"
        "\"max\":0,"
        "\"step\":\"any\","
        "\"unit\":\"dB\","
        "\"tooltip\":\"Below this level the creature is going hungry.\""
      "},"
      "{"
        "\"key\":\"comfortHigh\","
        "\"label\":\"Comfort High\","
        "\"defaultValue\":-3,"
        "\"min\":-24,"
        "\"mid\":-3,"
        "\"max\":0,"
        "\"step\":\"any\","
        "\"unit\":\"dB\","
        "\"tooltip\":\"Above this level the creature starts feeling the heat of clipping.\""
      "},"
      "{"
        "\"key\":\"sensitivity\","
        "\"label\":\"Sensitivity\","
        "\"defaultValue\":0.5,"
        "\"min\":0.05,"
        "\"mid\":0.5,"
        "\"max\":1,"
        "\"step\":\"any\","
        "\"tooltip\":\"How quickly the creature's mood reacts to changes in the signal.\""
      "}"
    "]"
  "}";

// Mood ids, in priority order (index 0 checked last as the fallback).
enum Mood {
  kMoodPeaceful = 0,
  kMoodSad = 1,
  kMoodHappy = 2,
  kMoodExcited = 3,
  kMoodHungry = 4,
  kMoodAngry = 5,
  kMoodFear = 6,
  kMoodMeltdown = 7,
};

static const int kMaxInstances = 16;

struct CreatureState {
  bool active;

  double envelope;         // fast amplitude envelope (linear, bounded 0..~1)
  double shortEnvelope;    // ~1s smoothed envelope (linear) -- "is it steady right now"
  double longEnvelope;     // ~10s smoothed envelope (linear) -- "expected loudness" baseline
  double steadiness;       // smoothed |fastLevelDb - shortLevelDb|
  double volatility;       // smoothed |fastLevelDb - longLevelDb|
  double clipHeat;          // 0..1, rises while hot, decays otherwise
  double onsetEnergy;       // decaying positive-onset detector

  double prevInput;         // last raw sample, for discontinuity detection
  double harshness;         // smoothed |sample-to-sample jump|
  double railHeat;          // smoothed "sitting at/near full scale"
  double meltdownHeat;      // 0..1, rises while harshness+railHeat both high

  double hunger;            // 0..1
  double recentlyHungry;    // decaying latch: "was hungry a moment ago"
  double health;            // 0..1
  bool   alive;             // latches false forever once health hits 0

  int    mood;              // currently displayed mood
  int    moodCandidate;     // last priority-chain winner
  double moodHoldSeconds;   // how long moodCandidate has been winning
};

static CreatureState gPool[kMaxInstances];

static inline double safe(double x) { return x * 0.0 == 0.0 ? x : 0.0; }
static inline double clamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline double absd(double x) { return x < 0.0 ? -x : x; }

// exp(x) via the same Taylor-plus-squaring trick used elsewhere in this
// codebase -- accurate for the |x| <= 4 range we always call it with here
// (x is always -1/samples with samples >= 1, so x is in [-1, 0]).
static double dsp_exp(double x) {
  double y = x * 0.25;
  double t = 1.0 + y*(1.0 + y*(0.5 + y*(1.0/6.0 + y*(1.0/24.0 + y*(1.0/120.0 + y*(1.0/720.0 + y/5040.0))))));
  t *= t; t *= t;
  return t;
}

static double one_pole_coefficient(double seconds, double sampleRate) {
  if (!(seconds > 0.0)) {
    return 1.0;
  }
  double samples = seconds * (sampleRate < 1.0 ? 1.0 : sampleRate);
  if (samples < 1.0) samples = 1.0;
  return 1.0 - dsp_exp(-1.0 / samples);
}

// Fast approximate log2 via IEEE-754 double bit manipulation (inverse of the
// "fastpow" trick used by vactrol_envelope) -- a few percent of error, which
// is plenty for a mood readout, not used anywhere precision-critical.
static inline double dsp_fast_log2(double d) {
  union { double d; int x[2]; } u;
  u.d = d;
  return ((double)u.x[1] - 1072632447.0) / 1048576.0;
}

// 20*log10(linear), floored well below anything audible instead of -inf.
static inline double dsp_db(double linear) {
  double x = absd(linear);
  if (x < 1e-9) x = 1e-9;  // floor ~ -180dB, stands in for silence/-inf
  return dsp_fast_log2(x) * 6.0205999132796239;
}

}  // namespace

extern "C" int soemdsp_creature_create() {
  for (int i = 0; i < kMaxInstances; i++) {
    if (!gPool[i].active) {
      CreatureState& s = gPool[i];
      s.active = true;
      s.envelope = 0.0;
      s.shortEnvelope = 0.0;
      s.longEnvelope = 0.0;
      s.steadiness = 0.0;
      s.volatility = 0.0;
      s.clipHeat = 0.0;
      s.onsetEnergy = 0.0;
      s.prevInput = 0.0;
      s.harshness = 0.0;
      s.railHeat = 0.0;
      s.meltdownHeat = 0.0;
      s.hunger = 0.3;
      s.recentlyHungry = 0.0;
      s.health = 1.0;
      s.alive = true;
      s.mood = kMoodPeaceful;
      s.moodCandidate = kMoodPeaceful;
      s.moodHoldSeconds = 0.0;
      return i + 1;
    }
  }
  return 0;
}

extern "C" void soemdsp_creature_destroy(int handle) {
  if (handle < 1 || handle > kMaxInstances) return;
  gPool[handle - 1].active = false;
}

extern "C" void soemdsp_creature_process(
  int handle,
  double input,
  double comfortLow,
  double comfortHigh,
  double sensitivity,
  double sampleRate
) {
  if (handle < 1 || handle > kMaxInstances) return;
  CreatureState& s = gPool[handle - 1];

  const double rate = sampleRate < 1.0 ? 1.0 : sampleRate;
  const double dt = 1.0 / rate;
  const double sens = clamp(sensitivity, 0.05, 1.0);
  const double low = comfortHigh > comfortLow ? comfortLow : comfortHigh - 21.0;
  const double high = comfortHigh > comfortLow ? comfortHigh : comfortLow + 21.0;

  // --- signal tracking -------------------------------------------------
  // Everything is smoothed in LINEAR envelope space (bounded ~0..1), never
  // directly in dB -- dB is unbounded downward (silence reads as -180dB),
  // so a one-pole filter chasing a dB value from a long silence has to
  // traverse a ~168dB gap, and even a "1 second" time constant takes many
  // seconds to close a gap that large. Smoothing the bounded linear
  // envelope instead means each time constant behaves like it says on the
  // tin regardless of how long the signal was silent before. dB is only
  // computed at the end, for comparisons/thresholds.
  const double safeInput = safe(input);
  const double rectified = absd(safeInput);
  const double envCoeff = one_pole_coefficient(rectified > s.envelope ? 0.003 : 0.15, rate);
  s.envelope = safe(s.envelope + (rectified - s.envelope) * envCoeff);
  const double fastLevelDb = dsp_db(s.envelope);

  // shortEnvelope settles in ~1s and only answers "is the level steady right
  // now" (gates Happy/Peaceful/hunger recovery). longEnvelope settles in
  // ~10s and only answers "has this been feast-or-famine over time" (feeds
  // the hunger-from-inconsistency term). Using the same slow reference for
  // both used to mean a creature could never look "steady" for ~20-30s
  // after any level change -- it never recovered from Hungry even sitting
  // in a perfectly good, steady signal.
  const double shortCoeff = one_pole_coefficient(1.0 / sens, rate);
  s.shortEnvelope = safe(s.shortEnvelope + (s.envelope - s.shortEnvelope) * shortCoeff);
  const double shortLevelDb = dsp_db(s.shortEnvelope);

  const double longCoeff = one_pole_coefficient(10.0 / sens, rate);
  s.longEnvelope = safe(s.longEnvelope + (s.envelope - s.longEnvelope) * longCoeff);
  const double longLevelDb = dsp_db(s.longEnvelope);

  const double steadyTarget = clamp(absd(fastLevelDb - shortLevelDb), 0.0, 60.0);
  const double steadyCoeff = one_pole_coefficient(0.5 / sens, rate);
  s.steadiness = safe(s.steadiness + (steadyTarget - s.steadiness) * steadyCoeff);

  const double volTarget = clamp(absd(fastLevelDb - longLevelDb), 0.0, 60.0);
  const double volCoeff = one_pole_coefficient(5.0 / sens, rate);
  s.volatility = safe(s.volatility + (volTarget - s.volatility) * volCoeff);

  const double clipTarget = fastLevelDb > high ? 1.0 : 0.0;
  const double clipCoeff = one_pole_coefficient(clipTarget > s.clipHeat ? 3.0 : 8.0, rate);
  s.clipHeat = clamp(s.clipHeat + (clipTarget - s.clipHeat) * clipCoeff, 0.0, 1.0);

  const double onsetRaw = fastLevelDb - longLevelDb;
  const double onsetDecay = one_pole_coefficient(0.6, rate);
  const double onsetDecayed = s.onsetEnergy * (1.0 - onsetDecay);
  s.onsetEnergy = onsetRaw > onsetDecayed ? clamp(onsetRaw, 0.0, 40.0) : onsetDecayed;

  // --- meltdown: harsh, rail-riding digital signal (near-square-wave hard
  // clipping), not just "loud". A smooth sine at full volume rails just as
  // often but never jumps abruptly between samples -- it's the combination
  // of both that reads as broken/harsh rather than merely hot.
  const double jump = absd(safeInput - s.prevInput);
  s.prevInput = safeInput;
  const double harshCoeff = one_pole_coefficient(0.05, rate);
  s.harshness = safe(s.harshness + (clamp(jump, 0.0, 2.0) - s.harshness) * harshCoeff);
  const double railTarget = rectified > 0.94 ? 1.0 : 0.0;
  const double railCoeff = one_pole_coefficient(railTarget > s.railHeat ? 0.05 : 0.3, rate);
  s.railHeat = clamp(s.railHeat + (railTarget - s.railHeat) * railCoeff, 0.0, 1.0);
  const double meltdownTarget = (s.harshness > 0.6 && s.railHeat > 0.7) ? 1.0 : 0.0;
  const double meltdownCoeff = one_pole_coefficient(meltdownTarget > s.meltdownHeat ? 0.15 : 1.0, rate);
  s.meltdownHeat = clamp(s.meltdownHeat + (meltdownTarget - s.meltdownHeat) * meltdownCoeff, 0.0, 1.0);

  // --- hunger ------------------------------------------------------------
  // Recovers only in the comfort band while the signal is steady; rises
  // from being too quiet (faster the closer to true silence), and rises
  // from inconsistency (feast-or-famine) even if the average level is fine.
  const bool inComfort = fastLevelDb >= low && fastLevelDb <= high;
  const bool steady = s.steadiness < 3.0;
  double hungerRate;
  if (inComfort && steady) {
    hungerRate = -0.05;
  } else if (fastLevelDb < low) {
    const double depth = clamp((low - fastLevelDb) / 40.0, 0.0, 1.0);
    hungerRate = 0.02 + depth * 0.18;
  } else {
    hungerRate = 0.01;
  }
  hungerRate += s.volatility * 0.0025;
  s.hunger = clamp(s.hunger + hungerRate * sens * dt, 0.0, 1.0);

  const double recentlyHungryTarget = s.hunger > 0.55 ? 1.0 : 0.0;
  const double recentlyHungryCoeff = one_pole_coefficient(recentlyHungryTarget > s.recentlyHungry ? 0.05 : 6.0, rate);
  s.recentlyHungry = clamp(s.recentlyHungry + (recentlyHungryTarget - s.recentlyHungry) * recentlyHungryCoeff, 0.0, 1.0);

  // --- health (the real stakes) -------------------------------------------
  // Only drains once hunger has been pinned near max for a while, and drains
  // slowly enough that stepping away for an afternoon won't kill it. Once it
  // latches dead it stays dead -- feeding it again no longer helps.
  if (s.alive) {
    // These are per-second rates -- multiply by dt only, never by rate too
    // (dt * rate is always exactly 1.0, which turns a per-second rate into
    // a per-sample one and drains a whole health bar in a couple seconds).
    if (s.hunger > 0.92) {
      s.health = clamp(s.health - (0.0006 * dt), 0.0, 1.0);
    } else if (s.hunger < 0.5) {
      s.health = clamp(s.health + (0.002 * dt), 0.0, 1.0);
    }
    if (s.health <= 0.0) {
      s.alive = false;
    }
  }

  // --- mood: priority chain, with a little hysteresis so it doesn't flicker
  int candidate;
  if (s.meltdownHeat > 0.5) {
    candidate = kMoodMeltdown;
  } else if (fastLevelDb > high + 3.0) {
    candidate = kMoodFear;
  } else if (s.clipHeat > 0.6) {
    candidate = kMoodAngry;
  } else if (s.onsetEnergy > 6.0 && inComfort && s.recentlyHungry > 0.4) {
    candidate = kMoodExcited;
  } else if (s.hunger > 0.5) {
    candidate = kMoodHungry;
  } else if (inComfort && steady) {
    candidate = kMoodHappy;
  } else if (fastLevelDb < low && fastLevelDb > low - 24.0 && steady) {
    candidate = kMoodPeaceful;
  } else {
    candidate = kMoodSad;
  }

  if (candidate == s.moodCandidate) {
    s.moodHoldSeconds += dt;
  } else {
    s.moodCandidate = candidate;
    s.moodHoldSeconds = 0.0;
  }
  const double holdRequired = (candidate == kMoodFear || candidate == kMoodMeltdown) ? 0.0 : 0.2;
  if (s.moodHoldSeconds >= holdRequired) {
    s.mood = candidate;
  }
}

extern "C" double soemdsp_creature_hunger(int handle) {
  if (handle < 1 || handle > kMaxInstances) return 0.0;
  return gPool[handle - 1].hunger * 100.0;
}

extern "C" double soemdsp_creature_health(int handle) {
  if (handle < 1 || handle > kMaxInstances) return 0.0;
  return gPool[handle - 1].health * 100.0;
}

extern "C" double soemdsp_creature_mood(int handle) {
  if (handle < 1 || handle > kMaxInstances) return 0.0;
  return (double)gPool[handle - 1].mood;
}

extern "C" double soemdsp_creature_alive(int handle) {
  if (handle < 1 || handle > kMaxInstances) return 0.0;
  return gPool[handle - 1].alive ? 1.0 : 0.0;
}

extern "C" int soemdsp_creature_version() {
  return 1;
}

extern "C" const char* soemdsp_creature_metadata_json() {
  return kMetadataJson;
}

extern "C" int soemdsp_creature_metadata_json_size() {
  return sizeof(kMetadataJson) - 1;
}
