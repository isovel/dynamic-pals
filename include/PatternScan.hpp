#pragma once
#include <string>
#include <vector>

namespace DynPals {
    // Scans the game's executable image; "??" matches any byte. The first match, or null.
    void* FindPattern(const std::string& patternStr);
    std::vector<void*> FindMultiplePatterns(const std::string& patternStr);
}
