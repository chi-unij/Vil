#include "FeatureRegistry.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_set>

namespace {

constexpr std::string_view kRegistryHeading =
    "## Standalone 3D Feature Lab Registry";

std::string Trim(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](char c) {
                      return std::isspace(static_cast<unsigned char>(c)) != 0;
                    }).base();
  if (first >= last)
    return {};
  return std::string(first, last);
}

std::string Clean(std::string value) {
  value = Trim(std::move(value));
  const std::array<std::string_view, 3> marks{"`", "**", "__"};
  for (const auto mark : marks) {
    size_t position = 0;
    while ((position = value.find(mark, position)) != std::string::npos)
      value.erase(position, mark.size());
  }
  return Trim(std::move(value));
}

std::vector<std::string> ParseRow(std::string_view line) {
  std::vector<std::string> cells;
  if (line.empty() || line.front() != '|')
    return cells;
  std::string current;
  bool escaped = false;
  for (size_t i = 1; i < line.size(); ++i) {
    const char c = line[i];
    if (escaped) {
      current.push_back(c);
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '|') {
      cells.push_back(Clean(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty())
    cells.push_back(Clean(current));
  if (!cells.empty() && cells.back().empty())
    cells.pop_back();
  return cells;
}

bool Separator(const std::vector<std::string> &cells) {
  if (cells.empty())
    return false;
  return std::all_of(cells.begin(), cells.end(), [](const std::string &cell) {
    const int dashes = static_cast<int>(
        std::count(cell.begin(), cell.end(), '-'));
    return dashes >= 3 &&
           std::all_of(cell.begin(), cell.end(), [](char c) {
             return c == '-' || c == ':' ||
                    std::isspace(static_cast<unsigned char>(c)) != 0;
           });
  });
}

int Column(const std::vector<std::string> &headers, std::string_view name) {
  const auto found = std::find(headers.begin(), headers.end(), name);
  return found == headers.end() ? -1
                                : static_cast<int>(found - headers.begin());
}

std::string Cell(const std::vector<std::string> &cells, int index) {
  if (index < 0 || static_cast<size_t>(index) >= cells.size())
    return {};
  return cells[static_cast<size_t>(index)];
}

bool ParseDefault(std::string_view text) {
  return text == "On" || text == "on" || text == "True" || text == "true" ||
         text == "1";
}

} // namespace

std::wstring LabUtf8ToWide(std::string_view text) {
  if (text.empty())
    return {};
  const int required = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0);
  if (required <= 0)
    return {};
  std::wstring result(static_cast<size_t>(required), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), required);
  return result;
}

std::string LabWideToUtf8(std::wstring_view text) {
  if (text.empty())
    return {};
  const int required = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (required <= 0)
    return {};
  std::string result(static_cast<size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), required,
                      nullptr, nullptr);
  return result;
}

LabRegistryLoadResult FeatureRegistry::Load(const std::filesystem::path &path) {
  LabRegistryLoadResult result;
  m_entries.clear();
  m_index.clear();
  m_sourcePath.clear();

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.error = L"feature.md を開けません: " + path.wstring();
    return result;
  }
  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  if (contents.size() >= 3 && static_cast<unsigned char>(contents[0]) == 0xEF &&
      static_cast<unsigned char>(contents[1]) == 0xBB &&
      static_cast<unsigned char>(contents[2]) == 0xBF)
    contents.erase(0, 3);

  std::istringstream stream(contents);
  std::string line;
  bool inRegistry = false;
  bool haveHeader = false;
  std::vector<std::string> headers;
  std::unordered_set<std::string> ids;

  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line == kRegistryHeading) {
      inRegistry = true;
      haveHeader = false;
      continue;
    }
    if (!inRegistry)
      continue;
    if (line.rfind("## ", 0) == 0)
      break;
    if (line.empty() || line.front() != '|')
      continue;

    const auto cells = ParseRow(line);
    if (cells.empty())
      continue;
    if (!haveHeader) {
      headers = cells;
      haveHeader = true;
      continue;
    }
    if (Separator(cells))
      continue;

    const int idColumn = Column(headers, "Lab ID");
    const int categoryColumn = Column(headers, "Category");
    const int featureColumn = Column(headers, "Feature");
    const int defaultColumn = Column(headers, "Default");
    const int descriptionColumn = Column(headers, "說明");
    if (idColumn < 0 || featureColumn < 0) {
      result.error = L"Feature Lab registry header が不正です。";
      return result;
    }

    LabFeatureEntry entry;
    entry.id = Cell(cells, idColumn);
    entry.category = LabUtf8ToWide(Cell(cells, categoryColumn));
    entry.name = LabUtf8ToWide(Cell(cells, featureColumn));
    entry.description = LabUtf8ToWide(Cell(cells, descriptionColumn));
    entry.defaultEnabled = ParseDefault(Cell(cells, defaultColumn));
    entry.enabled = entry.defaultEnabled;
    if (entry.id.empty() || entry.name.empty()) {
      result.error = L"Feature Lab registry に空の ID または Feature があります。";
      return result;
    }
    if (!ids.insert(entry.id).second) {
      result.error = L"Feature Lab registry の Lab ID が重複しています: " +
                     LabUtf8ToWide(entry.id);
      return result;
    }
    m_entries.push_back(std::move(entry));
  }

  if (!inRegistry) {
    result.error = L"Standalone 3D Feature Lab Registry section がありません。";
    return result;
  }
  if (m_entries.empty()) {
    result.error = L"Feature Lab registry row を取得できませんでした。";
    return result;
  }

  RebuildIndex();
  std::error_code error;
  m_sourcePath = std::filesystem::weakly_canonical(path, error);
  if (error)
    m_sourcePath = path;
  result.ok = true;
  result.parsedCount = m_entries.size();
  return result;
}

bool FeatureRegistry::Enabled(std::string_view id) const {
  const auto found = m_index.find(std::string(id));
  return found != m_index.end() && m_entries[found->second].enabled;
}

bool FeatureRegistry::SetEnabled(std::string_view id, bool enabled) {
  const auto found = m_index.find(std::string(id));
  if (found == m_index.end())
    return false;
  m_entries[found->second].enabled = enabled;
  return true;
}

void FeatureRegistry::SetAll(bool enabled) {
  for (auto &entry : m_entries)
    entry.enabled = enabled;
}

void FeatureRegistry::ResetDefaults() {
  for (auto &entry : m_entries)
    entry.enabled = entry.defaultEnabled;
}

size_t FeatureRegistry::EnabledCount() const {
  return static_cast<size_t>(std::count_if(
      m_entries.begin(), m_entries.end(),
      [](const LabFeatureEntry &entry) { return entry.enabled; }));
}

void FeatureRegistry::RebuildIndex() {
  m_index.clear();
  for (size_t i = 0; i < m_entries.size(); ++i)
    m_index[m_entries[i].id] = i;
}

std::filesystem::path FeatureRegistry::FindFeatureFile(
    const std::filesystem::path &executablePath,
    const std::filesystem::path &explicitPath) {
  std::vector<std::filesystem::path> candidates;
  if (!explicitPath.empty())
    candidates.push_back(explicitPath);
  std::error_code error;
  candidates.push_back(std::filesystem::current_path(error) / "feature.md");
  auto directory = executablePath.parent_path();
  for (int level = 0; level < 7 && !directory.empty(); ++level) {
    candidates.push_back(directory / "feature.md");
    const auto parent = directory.parent_path();
    if (parent == directory)
      break;
    directory = parent;
  }
  for (const auto &candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, error))
      return std::filesystem::weakly_canonical(candidate, error);
  }
  return {};
}
