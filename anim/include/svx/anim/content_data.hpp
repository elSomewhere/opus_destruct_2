// svx_anim — the library's content files (data/content/*.json), embedded in every build
// (cmake/embed_data.cmake): its prop catalogue (props.json).
#pragma once

#include <string_view>
#include <vector>

namespace svx::anim {

struct EmbeddedFile {
  const char* path;  // relative to data/content: "props.json"
  const char* text;
};
const std::vector<EmbeddedFile>& embedded_content();

// The text of one (nullptr: none).
inline const char* content_data(std::string_view path) {
  for (const EmbeddedFile& f : embedded_content())
    if (path == f.path) return f.text;
  return nullptr;
}

}  // namespace svx::anim
