#include "utils/Json.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace nuigraph {

std::string jsonResponse(const nlohmann::json& value) {
    return value.dump();
}

nlohmann::json errorJson(const std::string& message) {
    return nlohmann::json{{"error", message}};
}

std::string readTextFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read file: " + path);
    }
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

std::string htmlEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int hi = hexValue(value[i + 1]);
            int lo = hexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            }
        } else if (value[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::string formValue(const std::string& body, const std::string& key) {
    std::size_t pos = 0;
    while (pos <= body.size()) {
        auto end = body.find('&', pos);
        auto token = body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        auto eq = token.find('=');
        if (eq != std::string::npos && urlDecode(token.substr(0, eq)) == key) {
            return urlDecode(token.substr(eq + 1));
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}

} // namespace nuigraph
