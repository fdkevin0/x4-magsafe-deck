#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "AncsProtocol.h"

namespace magsafe_badge {

enum class BadgeKind : uint8_t {
  PassOrCard,
  BmpImage,
};

struct BadgeCard {
  static constexpr size_t kTagMax = 24;
  static constexpr size_t kTitleMax = 48;
  static constexpr size_t kSubtitleMax = 64;
  static constexpr size_t kMetaMax = 64;
  static constexpr size_t kQrMax = 256;
  static constexpr size_t kPathMax = 96;

  BadgeKind kind = BadgeKind::PassOrCard;
  char tag[kTagMax] = {0};
  char title[kTitleMax] = {0};
  char subtitle[kSubtitleMax] = {0};
  char meta[kMetaMax] = {0};
  char qrPayload[kQrMax] = {0};
  char bmpPath[kPathMax] = {0};

  void clear() {
    kind = BadgeKind::PassOrCard;
    tag[0] = '\0';
    title[0] = '\0';
    subtitle[0] = '\0';
    meta[0] = '\0';
    qrPayload[0] = '\0';
    bmpPath[0] = '\0';
  }
};

inline void copyTrimmedField(char* dst, size_t cap, std::string_view sv) {
  if (!dst || cap == 0) return;
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
    sv.remove_suffix(1);
  }
  const size_t n = std::min<size_t>(sv.size(), cap - 1);
  std::memcpy(dst, sv.data(), n);
  dst[n] = '\0';
  ancs::utf8SafeTruncate(dst, n);
}

// Parse a pipe-delimited card line: "TAG|TITLE|SUBTITLE|META|QR_PAYLOAD"
// or compact 2-field line: "TITLE|QR_PAYLOAD".
// Blank lines and lines starting with '#' are ignored.
inline bool parseCardLine(std::string_view line, BadgeCard& out) {
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
    line.remove_prefix(1);
  }
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
    line.remove_suffix(1);
  }
  if (line.empty() || line.front() == '#') {
    return false;
  }

  std::array<std::string_view, 5> fields{};
  size_t count = 0;
  size_t start = 0;
  while (count < 4) {
    const size_t bar = line.find('|', start);
    if (bar == std::string_view::npos) break;
    fields[count++] = line.substr(start, bar - start);
    start = bar + 1;
  }
  fields[count++] = line.substr(start);
  if (count < 2) {
    return false;
  }

  out.clear();
  out.kind = BadgeKind::PassOrCard;
  if (count == 2) {
    copyTrimmedField(out.tag, sizeof(out.tag), "MAGSAFE VAULT");
    copyTrimmedField(out.title, sizeof(out.title), fields[0]);
    copyTrimmedField(out.qrPayload, sizeof(out.qrPayload), fields[1]);
  } else if (count == 3) {
    copyTrimmedField(out.tag, sizeof(out.tag), fields[0]);
    copyTrimmedField(out.title, sizeof(out.title), fields[1]);
    copyTrimmedField(out.qrPayload, sizeof(out.qrPayload), fields[2]);
  } else if (count == 4) {
    copyTrimmedField(out.tag, sizeof(out.tag), fields[0]);
    copyTrimmedField(out.title, sizeof(out.title), fields[1]);
    copyTrimmedField(out.subtitle, sizeof(out.subtitle), fields[2]);
    copyTrimmedField(out.qrPayload, sizeof(out.qrPayload), fields[3]);
  } else {
    copyTrimmedField(out.tag, sizeof(out.tag), fields[0]);
    copyTrimmedField(out.title, sizeof(out.title), fields[1]);
    copyTrimmedField(out.subtitle, sizeof(out.subtitle), fields[2]);
    copyTrimmedField(out.meta, sizeof(out.meta), fields[3]);
    copyTrimmedField(out.qrPayload, sizeof(out.qrPayload), fields[4]);
  }
  return out.title[0] != '\0' || out.qrPayload[0] != '\0';
}

// Parse multi-line cards.txt content into a bounded array of BadgeCards.
template <size_t N>
size_t parseCardsText(std::string_view content, std::array<BadgeCard, N>& outCards, size_t existingCount = 0) {
  size_t count = existingCount;
  size_t pos = 0;
  while (pos < content.size() && count < N) {
    const size_t nl = content.find('\n', pos);
    const std::string_view line =
        (nl == std::string_view::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
    if (parseCardLine(line, outCards[count])) {
      ++count;
    } else {
      outCards[count].clear();
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return count;
}

// Incremental cards.txt parser: feed the file in arbitrary chunks (no need to hold the whole file in
// RAM) and stop reading once `full()`. Lines longer than kMaxLineBytes cannot be a valid card (the
// fields are much smaller) and are skipped instead of truncated. Within an accepted line, each field
// is still clipped to its BadgeCard buffer by parseCardLine (e.g. QR payloads to kQrMax - 1 bytes).
template <size_t N>
class CardsTextStreamParser {
 public:
  static constexpr size_t kMaxLineBytes = 512;

  CardsTextStreamParser(std::array<BadgeCard, N>& cards, size_t existingCount)
      : cards_(cards), count_(std::min(existingCount, N)) {}

  void feed(const char* data, size_t len) {
    for (size_t i = 0; i < len && !full(); ++i) {
      const char c = data[i];
      if (c == '\n') {
        flushLine();
        continue;
      }
      if (overlong_) continue;
      if (lineLen_ == kMaxLineBytes) {
        overlong_ = true;
        ++skippedOverlong_;
        continue;
      }
      line_[lineLen_++] = c;
    }
  }

  // Parse a trailing line without a newline. Call once after the last feed().
  void finish() {
    if (!full()) flushLine();
  }

  bool full() const { return count_ >= N; }
  size_t count() const { return count_; }
  size_t skippedOverlongLines() const { return skippedOverlong_; }

 private:
  void flushLine() {
    if (!overlong_ && lineLen_ > 0 && count_ < N) {
      if (parseCardLine(std::string_view(line_.data(), lineLen_), cards_[count_])) {
        ++count_;
      } else {
        cards_[count_].clear();
      }
    }
    lineLen_ = 0;
    overlong_ = false;
  }

  std::array<BadgeCard, N>& cards_;
  size_t count_;
  std::array<char, kMaxLineBytes> line_{};
  size_t lineLen_ = 0;
  bool overlong_ = false;
  size_t skippedOverlong_ = 0;
};

inline void populateDefaultCards(BadgeCard& badgeOut, BadgeCard& passOut) {
  badgeOut.clear();
  badgeOut.kind = BadgeKind::PassOrCard;
  copyTrimmedField(badgeOut.tag, sizeof(badgeOut.tag), "DIGITAL BADGE");
  copyTrimmedField(badgeOut.title, sizeof(badgeOut.title), "XTEINK X4 MAGSAFE");
  copyTrimmedField(badgeOut.subtitle, sizeof(badgeOut.subtitle), "Zero-Power E-Ink Companion");
  copyTrimmedField(badgeOut.meta, sizeof(badgeOut.meta), "SD: /badges/cards.txt or *.json");
  copyTrimmedField(badgeOut.qrPayload, sizeof(badgeOut.qrPayload), "https://crosspointreader.com");

  passOut.clear();
  passOut.kind = BadgeKind::PassOrCard;
  copyTrimmedField(passOut.tag, sizeof(passOut.tag), "BOARDING PASS");
  copyTrimmedField(passOut.title, sizeof(passOut.title), "CX 840  HKG -> JFK");
  copyTrimmedField(passOut.subtitle, sizeof(passOut.subtitle), "GATE 24  |  SEAT 12A");
  copyTrimmedField(passOut.meta, sizeof(passOut.meta), "BOARDING 18:15  |  ZONE 1");
  copyTrimmedField(passOut.qrPayload, sizeof(passOut.qrPayload),
                   "M1PASSENGER/XTEINK   E840HKGJFKCX 0840 309Y012A0001 100");
}

}  // namespace magsafe_badge
