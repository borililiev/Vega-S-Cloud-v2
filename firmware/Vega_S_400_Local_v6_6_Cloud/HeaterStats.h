#pragma once

// Estimated consumption derived from relay ON time and nominal heater power.
// Requires valid NTP time for calendar and day/night assignment.
#include <Arduino.h>
#include <LittleFS.h>
#include <time.h>
#include <stddef.h>
#include <string.h>

static constexpr uint32_t HEATER_STATS_MAGIC = 0x48535432UL;
static constexpr uint16_t HEATER_STATS_VERSION = 2;
static constexpr uint32_t HEATER_STATS_SAVE_INTERVAL = 15UL * 60UL * 1000UL;

struct HeaterBucket {
  int32_t key;
  uint32_t dayMs;
  uint32_t nightMs;
  uint32_t completedCycleMs;
  uint32_t completedCycles;
  float tempSum;
  uint32_t tempSamples;
};

struct HeaterStatsData {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t sequence;
  HeaterBucket hourly[48];
  HeaterBucket daily[35];
  HeaterBucket monthly[13];
  uint32_t checksum;
};

class HeaterStats {
 public:
  void begin() {
    emptyData();
    load();
    lastTickMs_ = millis();
    lastSaveMs_ = millis();
    lastTempSampleMs_ = millis() - 60000UL;
  }

  void clear() {
    // Delete both alternating files; merely clearing RAM could resurrect old data.
    LittleFS.remove("/heater_stats_a.bin");
    LittleFS.remove("/heater_stats_b.bin");
    emptyData();
    nextFile_ = 0;
    dirty_ = false;
    lastTickMs_ = millis();
    lastTempSampleMs_ = millis();
    lastSaveMs_ = millis();
  }

  bool clockReady() const { return time(nullptr) >= 1704067200; }

  void tick(bool relayOn, float temperature, bool tempValid,
            uint8_t dayStart, uint8_t nightStart) {
    uint32_t current = millis();
    uint32_t delta = current - lastTickMs_;
    lastTickMs_ = current;
    if (!clockReady()) return;
    time_t epoch = time(nullptr);
    tm local;
    localtime_r(&epoch, &local);
    HeaterBucket &h = mutableHour((int32_t)(epoch / 3600));
    HeaterBucket &d = mutableDay(dayKey(local));
    HeaterBucket &m = mutableMonth(monthKey(local));
    if (relayOn && delta <= 60000UL) {
      bool day = dayStart < nightStart
        ? (local.tm_hour >= dayStart && local.tm_hour < nightStart)
        : (local.tm_hour >= dayStart || local.tm_hour < nightStart);
      // With a 1-2 second loop cadence, cutover error is at most one tick.
      if (day) {
        h.dayMs += delta; d.dayMs += delta; m.dayMs += delta;
      } else {
        h.nightMs += delta; d.nightMs += delta; m.nightMs += delta;
      }
      dirty_ = true;
    }
    if (current - lastTempSampleMs_ >= 60000UL) {
      lastTempSampleMs_ = current;
      if (tempValid && isfinite(temperature)) {
        h.tempSum += temperature; ++h.tempSamples;
        d.tempSum += temperature; ++d.tempSamples;
        m.tempSum += temperature; ++m.tempSamples;
        dirty_ = true;
      }
    }
    if (dirty_ && current - lastSaveMs_ >= HEATER_STATS_SAVE_INTERVAL)
      save();
  }

  void completedCycle(uint32_t durationMs) {
    if (!clockReady()) return;
    time_t epoch = time(nullptr);
    tm local;
    localtime_r(&epoch, &local);
    HeaterBucket &d = mutableDay(dayKey(local));
    HeaterBucket &m = mutableMonth(monthKey(local));
    d.completedCycleMs += durationMs; ++d.completedCycles;
    m.completedCycleMs += durationMs; ++m.completedCycles;
    dirty_ = true;
  }

  HeaterBucket hourAt(time_t epoch) const {
    return lookup(data_.hourly, 48, (int32_t)(epoch / 3600));
  }
  HeaterBucket dayAt(time_t epoch) const {
    tm t; localtime_r(&epoch, &t);
    return lookup(data_.daily, 35, dayKey(t));
  }
  HeaterBucket monthAt(time_t epoch) const {
    tm t; localtime_r(&epoch, &t);
    return lookup(data_.monthly, 13, monthKey(t));
  }

  // The current nominal rating is used at reporting time. Existing stored data
  // retains relay ON time, so older periods are revalued when the rating changes.
  static float kwh(uint32_t onMs, uint16_t powerW) {
    return (float)onMs * (float)powerW / 3600000000.0f;
  }
  static float hours(uint32_t onMs) { return (float)onMs / 3600000.0f; }

 private:
  HeaterStatsData data_{};
  uint32_t lastTickMs_ = 0;
  uint32_t lastTempSampleMs_ = 0;
  uint32_t lastSaveMs_ = 0;
  bool dirty_ = false;
  uint8_t nextFile_ = 0;

  static int32_t dayKey(const tm &t) { return (t.tm_year + 1900) * 400 + t.tm_yday; }
  static int32_t monthKey(const tm &t) { return (t.tm_year + 1900) * 12 + t.tm_mon; }
  static HeaterBucket emptyBucket(int32_t key = -1) {
    HeaterBucket b{}; b.key = key; return b;
  }
  static HeaterBucket lookup(const HeaterBucket *list, int count, int32_t key) {
    const HeaterBucket &b = list[(uint32_t)key % count];
    return b.key == key ? b : emptyBucket(key);
  }
  static HeaterBucket &getOrReset(HeaterBucket *list, int count, int32_t key) {
    HeaterBucket &b = list[(uint32_t)key % count];
    if (b.key != key) b = emptyBucket(key);
    return b;
  }
  HeaterBucket &mutableHour(int32_t key) {
    return getOrReset(data_.hourly, 48, key);
  }
  HeaterBucket &mutableDay(int32_t key) {
    return getOrReset(data_.daily, 35, key);
  }
  HeaterBucket &mutableMonth(int32_t key) {
    return getOrReset(data_.monthly, 13, key);
  }

  void emptyData() {
    memset(&data_, 0, sizeof(data_));
    data_.magic = HEATER_STATS_MAGIC;
    data_.version = HEATER_STATS_VERSION;
    for (auto &b : data_.hourly) b.key = -1;
    for (auto &b : data_.daily) b.key = -1;
    for (auto &b : data_.monthly) b.key = -1;
  }
  static uint32_t checksum(const HeaterStatsData &value) {
    uint32_t h = 2166136261UL;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
    for (size_t i = 0; i < offsetof(HeaterStatsData, checksum); ++i)
      h = (h ^ bytes[i]) * 16777619UL;
    return h;
  }
  static bool readSnapshot(const char *path, HeaterStatsData &result) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    bool ok = f.size() == sizeof(result) &&
              f.readBytes(reinterpret_cast<char *>(&result), sizeof(result)) == sizeof(result);
    f.close();
    return ok && result.magic == HEATER_STATS_MAGIC &&
           result.version == HEATER_STATS_VERSION &&
           result.checksum == checksum(result);
  }
  void load() {
    bool aValid = readSnapshot("/heater_stats_a.bin", data_);
    HeaterStatsData *b = new HeaterStatsData;
    bool bValid = b && readSnapshot("/heater_stats_b.bin", *b);
    if (bValid && (!aValid || b->sequence > data_.sequence)) {
      data_ = *b;
      nextFile_ = 0;
    } else if (aValid) {
      nextFile_ = 1;
    } else {
      emptyData();
    }
    delete b;
  }
  void save() {
    data_.sequence++;
    data_.checksum = checksum(data_);
    const char *path = nextFile_ ? "/heater_stats_b.bin" : "/heater_stats_a.bin";
    File f = LittleFS.open(path, "w");
    if (!f) return;
    size_t written = f.write(reinterpret_cast<const uint8_t *>(&data_), sizeof(data_));
    f.flush();
    f.close();
    if (written == sizeof(data_)) {
      dirty_ = false;
      lastSaveMs_ = millis();
      nextFile_ ^= 1;
    }
  }
};
