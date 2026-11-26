#pragma once

#include <string_view>

namespace a3vr {

inline bool IsOpaqueVrsMaterial(std::string_view assembly)
{
    bool colorOutput = false;
    while (!assembly.empty()) {
        const auto end = assembly.find('\n');
        auto line = assembly.substr(0, end);
        assembly = end == std::string_view::npos ? std::string_view{} : assembly.substr(end + 1);
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) continue;
        line.remove_prefix(first);
        if (line.starts_with("dcl_output")) {
            if (colorOutput || !line.starts_with("dcl_output o0.")) return false;
            colorOutput = true;
        }
        if (line.starts_with("discard") || line.starts_with("dcl_uav_") ||
            line.starts_with("eval_sample_index") ||
            (line.starts_with("dcl_input_ps") &&
             (line.find("sample_index") != std::string_view::npos ||
              line.find("coverage") != std::string_view::npos ||
              line.find(" sample ") != std::string_view::npos))) {
            return false;
        }
    }
    return colorOutput;
}

}  // namespace a3vr
