#pragma once

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Lecture/écriture minimales : tableaux binaires bruts et JSON simple, sans dépendance externe.
namespace firesim::io {

static_assert(std::endian::native == std::endian::little, "les fichiers de couches sont petit-boutistes");

template <class T>
std::vector<T> read_raw(const std::filesystem::path& path, size_t count) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("impossible d'ouvrir " + path.string());
    std::vector<T> data(count);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(count * sizeof(T)));
    if (in.gcount() != static_cast<std::streamsize>(count * sizeof(T))) {
        throw std::runtime_error(path.string() + " : taille inattendue");
    }
    return data;
}

template <class T>
void write_raw(const std::filesystem::path& path, std::span<const T> data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("impossible d'écrire " + path.string());
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size_bytes()));
}

// Objet JSON plat {"clé": nombre ou "texte"} ; les valeurs sont rendues sous forme de texte.
std::map<std::string, std::string> read_flat_json(const std::filesystem::path& path);
double json_number(const std::map<std::string, std::string>& obj, const std::string& key);

// Écriture JSON au fil de l'eau, virgules et échappements gérés.
class JsonWriter {
public:
    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view k);
    JsonWriter& value(double v);
    JsonWriter& value(int64_t v);
    JsonWriter& value(int v) { return value(static_cast<int64_t>(v)); }
    JsonWriter& value(bool v);
    JsonWriter& value(std::string_view v);
    JsonWriter& value(const char* v) { return value(std::string_view(v)); }
    template <class T>
    JsonWriter& field(std::string_view k, T v) { return key(k).value(v); }

    const std::string& str() const { return out_; }
    void save(const std::filesystem::path& path) const;

private:
    void separate();
    std::string out_;
    std::vector<bool> first_;  // pile : premier élément du conteneur courant ?
    bool after_key_ = false;
};

}  // namespace firesim::io
