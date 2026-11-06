#pragma once

#include <HalStorage.h>
#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>
#include <VectorFontSupport.h>

#include <memory>
#include <string>

class GfxRenderer;
class TtfEpdFont;

// Discovers and loads CJK / non-Latin fallback fonts from the MicroSD card,
// sharing the exact directory layout (/.fonts and /fonts) and user preference
// (/.crosspoint/settings.json -> "sdFontFamilyName") with CrossPoint Reader.
class SharedFontLoader {
 public:
  SharedFontLoader();
  ~SharedFontLoader();
  SharedFontLoader(const SharedFontLoader&) = delete;
  SharedFontLoader& operator=(const SharedFontLoader&) = delete;

  // Scan /.crosspoint/settings.json, /.fonts, and /fonts, and register
  // size-matched fallbacks for UI_10_FONT_ID and UI_12_FONT_ID on `renderer`.
  // Returns true if a CJK / non-Latin fallback font family was loaded.
  bool begin(GfxRenderer& renderer);

  // Release all SD font caches and close open file handles (call before deep sleep).
  void unload(GfxRenderer& renderer);

  bool isLoaded() const { return !activeFamily_.empty(); }
  const std::string& activeFamilyName() const { return activeFamily_; }

 private:
  bool tryLoadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer);
  bool tryLoadCpfontFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer);

#if CROSSPOINT_VECTOR_FONTS
  bool tryLoadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer);
  void unloadTtf(GfxRenderer& renderer);

  struct TtfSource {
    HalFile file;
    unsigned long size = 0;
    bool present = false;
  };
  TtfSource ttfSources_[4];
  std::unique_ptr<TtfEpdFont> ttf12_;
  std::unique_ptr<TtfEpdFont> ttf10_;
  int ttf12Id_ = 0;
  int ttf10Id_ = 0;
#endif

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::string activeFamily_;
};
