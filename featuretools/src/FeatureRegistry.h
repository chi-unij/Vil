#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct LabFeatureEntry {
  std::string id;
  std::wstring category;
  std::wstring name;
  std::wstring description;
  std::string route;
  std::string defaultMode;
  bool defaultEnabled = true;
  bool enabled = true;
};

struct LabRegistryLoadResult {
  bool ok = false;
  std::wstring error;
  size_t parsedCount = 0;
};

class FeatureRegistry {
public:
  LabRegistryLoadResult Load(
      const std::filesystem::path &path,
      std::string_view registryHeading =
          "## Standalone 3D Feature Lab Registry");

  [[nodiscard]] const std::vector<LabFeatureEntry> &Entries() const noexcept {
    return m_entries;
  }
  [[nodiscard]] std::vector<LabFeatureEntry> &Entries() noexcept {
    return m_entries;
  }

  [[nodiscard]] bool Enabled(std::string_view id) const;
  bool SetEnabled(std::string_view id, bool enabled);
  void SetAll(bool enabled);
  void ResetDefaults();
  [[nodiscard]] size_t EnabledCount() const;

  [[nodiscard]] const std::filesystem::path &SourcePath() const noexcept {
    return m_sourcePath;
  }

  [[nodiscard]] static std::filesystem::path
  FindFeatureFile(const std::filesystem::path &executablePath,
                  const std::filesystem::path &explicitPath = {});

private:
  void RebuildIndex();

  std::vector<LabFeatureEntry> m_entries;
  std::unordered_map<std::string, size_t> m_index;
  std::filesystem::path m_sourcePath;
};

[[nodiscard]] std::wstring LabUtf8ToWide(std::string_view text);
[[nodiscard]] std::string LabWideToUtf8(std::wstring_view text);
