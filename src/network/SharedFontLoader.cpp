#include "network/SharedFontLoader.h"

#include <ArduinoJson.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <TtfEpdFont.h>
#include <esp_heap_caps.h>

#include "fontIds.h"

namespace {

constexpr const char* kCrossPointSettingsPath = "/.crosspoint/settings.json";
constexpr size_t kMaxSettingsBytes = 4096;

constexpr int kFallbackUi10FontId = 0x434A4B0A;  // "CJK\x0A"
constexpr int kFallbackUi12FontId = 0x434A4B0C;  // "CJK\x0C"

// Representative codepoints for CJK and other non-Latin scripts that built-in
// Ubuntu UI fonts do not cover.
constexpr uint32_t kFallbackProbes[] = {
    0x4E00,  // CJK Unified Ideographs (一)
    0x4E2D,  // CJK Unified Ideographs (中)
    0x6587,  // CJK Unified Ideographs (文)
    0x3042,  // Hiragana (あ)
    0x30A2,  // Katakana (ア)
    0xAC00,  // Hangul Syllables (가)
    0x03B1,  // Greek (α)
    0x0430,  // Cyrillic (а)
};

bool hasFallbackScriptCoverage(const EpdFontFamily& family) {
  for (const uint32_t cp : kFallbackProbes) {
    if (family.hasCodepoint(cp)) {
      return true;
    }
  }
  return false;
}

bool hasCjkCoverage(const EpdFontFamily& family) {
  return family.hasCodepoint(0x4E00) || family.hasCodepoint(0x4E2D) || family.hasCodepoint(0x6587);
}

std::string readPreferredFamilyFromCrossPointSettings() {
  if (!Storage.ready() || !Storage.exists(kCrossPointSettingsPath)) {
    return "";
  }
  std::string raw;
  if (!Storage.readFileToString("FONT", kCrossPointSettingsPath, kMaxSettingsBytes, raw)) {
    return "";
  }
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) {
    return "";
  }
  const char* name = doc["sdFontFamilyName"] | "";
  return std::string(name ? name : "");
}

}  // namespace

SharedFontLoader::SharedFontLoader() = default;
SharedFontLoader::~SharedFontLoader() = default;

bool SharedFontLoader::begin(GfxRenderer& renderer) {
  unload(renderer);
  if (!Storage.ready()) {
    return false;
  }

  if (!registry_.discover() || registry_.getFamilyCount() == 0) {
    return false;
  }

  // 1. Honor the user's selected SD font family from CrossPoint Reader (/.crosspoint/settings.json)
  const std::string preferred = readPreferredFamilyFromCrossPointSettings();
  if (!preferred.empty()) {
    if (const auto* fam = registry_.findFamily(preferred)) {
      if (tryLoadFamily(*fam, renderer)) {
        return true;
      }
    }
  }

  // 2. Scan discovered families: prefer pre-rasterized .cpfont families first
  //    (minimal ~4 KB RAM footprint on ESP32-C3), then vector .ttf/.otf/.ttc families.
  for (int pass = 0; pass < 2; ++pass) {
    const bool wantVector = (pass == 1);
    for (const auto& fam : registry_.getFamilies()) {
      if (fam.vector != wantVector) continue;
      if (!preferred.empty() && fam.name == preferred) continue;
      if (tryLoadFamily(fam, renderer)) {
        return true;
      }
    }
  }

  return false;
}

void SharedFontLoader::unload(GfxRenderer& renderer) {
  renderer.clearFallbackFonts();
  manager_.unloadAll(renderer);
#if CROSSPOINT_VECTOR_FONTS
  unloadTtf(renderer);
#endif
  activeFamily_.clear();
}

bool SharedFontLoader::tryLoadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
#if CROSSPOINT_VECTOR_FONTS
  if (family.vector) {
    return tryLoadTtfFamily(family, renderer);
  }
#else
  if (family.vector) {
    return false;
  }
#endif
  return tryLoadCpfontFamily(family, renderer);
}

bool SharedFontLoader::tryLoadCpfontFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  if (!manager_.loadFamily(family, renderer, 12)) {
    return false;
  }

  const int baseId = manager_.getFontId(family.name);
  const auto it = renderer.getFontMap().find(baseId);
  if (it == renderer.getFontMap().end() || !hasFallbackScriptCoverage(it->second)) {
    manager_.unloadAll(renderer);
    return false;
  }

  const int id12 = manager_.loadFamilyExtraSize(family, renderer, 12, kFallbackUi12FontId);
  const int id10 = manager_.loadFamilyExtraSize(family, renderer, 10, kFallbackUi10FontId);

  const int resolved12 = (id12 != 0) ? id12 : baseId;
  const int resolved10 = (id10 != 0) ? id10 : resolved12;

  renderer.setFallbackFont(UI_12_FONT_ID, resolved12);
  renderer.setFallbackFont(UI_10_FONT_ID, resolved10);
  activeFamily_ = family.name;
  LOG_DBG("FONT", "Loaded shared .cpfont fallback '%s' (cjk=%d, ui10=%d, ui12=%d)", family.name.c_str(),
          hasCjkCoverage(it->second) ? 1 : 0, resolved10, resolved12);
  return true;
}

#if CROSSPOINT_VECTOR_FONTS
void SharedFontLoader::unloadTtf(GfxRenderer& renderer) {
  if (ttf10Id_ != 0) {
    renderer.unregisterTtfFont(ttf10Id_);
    renderer.removeFont(ttf10Id_);
    ttf10Id_ = 0;
  }
  if (ttf12Id_ != 0) {
    renderer.unregisterTtfFont(ttf12Id_);
    renderer.removeFont(ttf12Id_);
    ttf12Id_ = 0;
  }
  ttf10_.reset();
  ttf12_.reset();
  for (auto& s : ttfSources_) {
    if (s.file) s.file.close();
    s.size = 0;
    s.present = false;
  }
}

bool SharedFontLoader::tryLoadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  unloadTtf(renderer);
  if (family.files.empty()) {
    return false;
  }

  char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
  for (const auto& file : family.files) {
    const uint8_t role = file.style < 4 ? file.style : 0;
    if (ttfSources_[role].present) continue;
    if (!family.filePath(file, path, sizeof(path))) continue;
    HalFile f = Storage.open(path);
    if (!f) continue;
    const size_t len = f.size();
    if (len == 0) {
      f.close();
      continue;
    }
    ttfSources_[role].file = std::move(f);
    ttfSources_[role].size = static_cast<unsigned long>(len);
    ttfSources_[role].present = true;
  }

  if (!ttfSources_[0].present) {
    unloadTtf(renderer);
    return false;
  }

  auto configureSources = [this](TtfEpdFont& font) {
    for (uint8_t st = 0; st < 4; ++st) {
      if (ttfSources_[st].present) {
        font.addStreamSource(st, &SdCardFontRegistry::halFileRead, &ttfSources_[st].file, ttfSources_[st].size);
      }
    }
  };

  ttf12_ = makeUniqueNoThrow<TtfEpdFont>();
  if (!ttf12_) {
    unloadTtf(renderer);
    return false;
  }
  configureSources(*ttf12_);
  if (!ttf12_->load(12, /*twoBit=*/true, /*glyphCacheBytes=*/16 * 1024, /*maxGlyphs=*/256)) {
    unloadTtf(renderer);
    return false;
  }

  const EpdFontFamily fam12 = ttf12_->family();
  if (!hasFallbackScriptCoverage(fam12)) {
    unloadTtf(renderer);
    return false;
  }

  ttf12Id_ = kFallbackUi12FontId;
  renderer.insertFont(ttf12Id_, fam12);
  renderer.registerTtfFont(ttf12Id_, ttf12_.get());
  renderer.setFallbackFont(UI_12_FONT_ID, ttf12Id_);

  // Load a dedicated 10pt face when sufficient internal heap remains; otherwise
  // share the 12pt face for UI_10_FONT_ID (GfxRenderer centers its baseline).
  static constexpr size_t kSecondFaceMinHeap = 96 * 1024;
  const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (internalFree >= kSecondFaceMinHeap) {
    ttf10_ = makeUniqueNoThrow<TtfEpdFont>();
    if (ttf10_) {
      configureSources(*ttf10_);
      if (ttf10_->load(10, /*twoBit=*/true, /*glyphCacheBytes=*/12 * 1024, /*maxGlyphs=*/192)) {
        ttf10Id_ = kFallbackUi10FontId;
        renderer.insertFont(ttf10Id_, ttf10_->family());
        renderer.registerTtfFont(ttf10Id_, ttf10_.get());
        renderer.setFallbackFont(UI_10_FONT_ID, ttf10Id_);
      } else {
        ttf10_.reset();
      }
    }
  }
  if (ttf10Id_ == 0) {
    renderer.setFallbackFont(UI_10_FONT_ID, ttf12Id_);
  }

  activeFamily_ = family.name;
  LOG_DBG("FONT", "Loaded shared TTF fallback '%s' (ui10=%d, ui12=%d)", family.name.c_str(),
          ttf10Id_ ? ttf10Id_ : ttf12Id_, ttf12Id_);
  return true;
}
#endif  // CROSSPOINT_VECTOR_FONTS
