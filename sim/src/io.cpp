#include "firesim/io.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace firesim::io {

std::map<std::string, std::string> read_flat_json(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("impossible d'ouvrir " + path.string());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::map<std::string, std::string> obj;
    size_t i = 0;
    auto skip_ws = [&] {
        while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',')) ++i;
    };
    auto read_string = [&] {
        std::string s;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            s += text[i];
        }
        ++i;
        return s;
    };
    skip_ws();
    if (i >= text.size() || text[i] != '{') throw std::runtime_error(path.string() + " : objet JSON attendu");
    ++i;
    for (skip_ws(); i < text.size() && text[i] != '}'; skip_ws()) {
        if (text[i] != '"') throw std::runtime_error(path.string() + " : clé attendue");
        std::string key = read_string();
        skip_ws();
        if (i >= text.size() || text[i] != ':') throw std::runtime_error(path.string() + " : « : » attendu");
        ++i;
        skip_ws();
        if (i < text.size() && text[i] == '"') {
            obj[key] = read_string();
        } else {
            const size_t start = i;
            while (i < text.size() && text[i] != ',' && text[i] != '}' && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            obj[key] = text.substr(start, i - start);
        }
    }
    return obj;
}

double json_number(const std::map<std::string, std::string>& obj, const std::string& key) {
    const auto it = obj.find(key);
    if (it == obj.end()) throw std::runtime_error("clé JSON absente : " + key);
    return std::stod(it->second);
}

void JsonWriter::separate() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) out_ += ',';
        first_.back() = false;
    }
}

JsonWriter& JsonWriter::begin_object() {
    separate();
    out_ += '{';
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    first_.pop_back();
    out_ += '}';
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    separate();
    out_ += '[';
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    first_.pop_back();
    out_ += ']';
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view k) {
    value(k);
    out_ += ':';
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(double v) {
    separate();
    if (!std::isfinite(v)) {
        out_ += "null";
    } else {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.10g", v);
        out_ += buf;
    }
    return *this;
}

JsonWriter& JsonWriter::value(int64_t v) {
    separate();
    out_ += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::value(bool v) {
    separate();
    out_ += v ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view v) {
    separate();
    out_ += '"';
    for (const char c : v) {
        if (c == '"' || c == '\\') out_ += '\\';
        out_ += c;
    }
    out_ += '"';
    return *this;
}

void JsonWriter::save(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("impossible d'écrire " + path.string());
    out << out_ << '\n';
}

}  // namespace firesim::io
