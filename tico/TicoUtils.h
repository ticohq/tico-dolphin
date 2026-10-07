#pragma once
#include <string>
#include <vector>

namespace TicoUtils {

    // Helper to trim whitespace
    static inline std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (std::string::npos == first) {
            return str;
        }
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    // Exception-free title cleaner
    // Strips content within () and []
    static inline std::string GetCleanTitle(const std::string& filename) {
        // First remove extension
        std::string title = filename;
        size_t lastDot = title.find_last_of(".");
        if (lastDot != std::string::npos) {
            title = title.substr(0, lastDot);
        }

        std::string result = "";
        result.reserve(title.length());

        int parenDepth = 0;
        int bracketDepth = 0;
        bool lastWasSpace = false;

        for (char c : title) {
            if (c == '(') {
                parenDepth++;
                continue;
            }
            if (c == ')') {
                if (parenDepth > 0) parenDepth--;
                continue;
            }
            if (c == '[') {
                bracketDepth++;
                continue;
            }
            if (c == ']') {
                if (bracketDepth > 0) bracketDepth--;
                continue;
            }

            if (parenDepth == 0 && bracketDepth == 0) {
                // Determine if we should append this character
                // Logic: coalesce spaces, don't start with space
                if (c == ' ' || c == '_') {
                     // Treat underscore as space or just keep it? 
                     // Usually filenames have underscores. Let's convert to space for title?
                     // Standard practice is often to replace _ with space. 
                     // Let's stick to user request: strip regions. 
                     // But usually "Game_Name" -> "Game Name" is desired too.
                     // I'll stick to just passing space if it is one.
                     if (!result.empty() && !lastWasSpace) {
                         result += ' ';
                         lastWasSpace = true;
                     }
                } else {
                    result += c;
                    lastWasSpace = false;
                }
            }
        }

        return Trim(result);
    }

}
