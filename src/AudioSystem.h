#pragma once

#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

// Windows 標準の MCI を利用する軽量オーディオ再生層。
// MP3 を事前変換せず、短い効果音とループ環境音を同時再生できる。
class AudioSystem final {
public:
  using SoundHandle = std::size_t;
  static constexpr SoundHandle InvalidSound =
      std::numeric_limits<SoundHandle>::max();

  AudioSystem() = default;
  ~AudioSystem();

  AudioSystem(const AudioSystem &) = delete;
  AudioSystem &operator=(const AudioSystem &) = delete;
  AudioSystem(AudioSystem &&) = delete;
  AudioSystem &operator=(AudioSystem &&) = delete;

  SoundHandle LoadSound(const std::filesystem::path &path, float volume = 1.0f);
  void PlayOneShot(SoundHandle sound);
  void SetLooping(SoundHandle sound, bool shouldLoop);
  void Stop(SoundHandle sound);
  void StopAll();
  void Shutdown();

  bool IsLoaded(SoundHandle sound) const;
  bool IsLooping(SoundHandle sound) const;

private:
  struct SoundState {
    std::wstring alias;
    bool loaded = false;
    bool looping = false;
  };

  static bool SendCommand(const std::wstring &command, const wchar_t *operation,
                          bool logFailure = true);
  bool PlayFromStart(SoundHandle sound, bool loop);

  std::vector<SoundState> m_sounds;
};
