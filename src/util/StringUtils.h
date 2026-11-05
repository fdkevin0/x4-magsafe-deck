#pragma once

#include <cctype>

namespace StringUtils {

/**
 * Case-insensitive ASCII strcmp. Returns <0, 0, or >0 like strcmp, comparing
 * each byte by its lowercased value.
 */
inline int asciiCaseCmp(const char* a, const char* b) {
  while (*a && *b) {
    int diff = std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
    if (diff != 0) return diff;
    ++a;
    ++b;
  }
  return std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
}

}  // namespace StringUtils
