// svx_city — the city's data files (data/city/*.json), embedded in every build (cmake/
// embed_data.cmake).
#pragma once

#include <string_view>
#include <vector>

namespace svx::city {

struct EmbeddedFile {
  const char* path;  // relative to data/city: "props.json"
  const char* text;
};
const std::vector<EmbeddedFile>& embedded_city_data();

// The text of one (nullptr: none).
inline const char* city_data(std::string_view path) {
  for (const EmbeddedFile& f : embedded_city_data())
    if (path == f.path) return f.text;
  return nullptr;
}

}  // namespace svx::city
