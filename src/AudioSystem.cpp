#include "AudioSystem.h"

#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <system_error>

namespace {

std::atomic_uint64_t g_audioAliasSerial{0};

void TraceAudioMessage(const std::wstring &message) {
  OutputDebugStringW((L"[AudioSystem] " + message + L"\n").c_str());
}

} // namespace

AudioSystem::~AudioSystem() { Shutdown(); }

bool AudioSystem::SendCommand(const std::wstring &command,
                              const wchar_t *operation, bool logFailure) {
  const MCIERROR error = mciSendStringW(command.c_str(), nullptr, 0, nullptr);
  if (error == 0)
    return true;

  if (logFailure) {
    wchar_t errorText[256]{};
    if (!mciGetErrorStringW(error, errorText,
                            static_cast<UINT>(std::size(errorText)))) {
      swprintf_s(errorText, L"MCI error %lu",
                 static_cast<unsigned long>(error));
    }
    TraceAudioMessage(std::wstring(operation) + L" failed: " + errorText);
  }
  return false;
}

AudioSystem::SoundHandle
AudioSystem::LoadSound(const std::filesystem::path &path, float volume) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    TraceAudioMessage(L"音源が見つかりません: " + path.wstring());
    return InvalidSound;
  }

  std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
  if (error)
    absolutePath = path;

  SoundState sound;
  sound.alias =
      L"villien_audio_" + std::to_wstring(g_audioAliasSerial.fetch_add(1) + 1);
  const std::wstring openCommand = L"open \"" + absolutePath.wstring() +
                                   L"\" type mpegvideo alias " + sound.alias;
  if (!SendCommand(openCommand, L"open"))
    return InvalidSound;

  sound.loaded = true;
  m_sounds.push_back(sound);
  const SoundHandle handle = m_sounds.size() - 1;

  const int mciVolume = std::clamp(
      static_cast<int>(std::lround(std::clamp(volume, 0.0f, 1.0f) * 1000.0f)),
      0, 1000);
  const std::wstring volumeCommand =
      L"setaudio " + sound.alias + L" volume to " + std::to_wstring(mciVolume);
  SendCommand(volumeCommand, L"setaudio");
  TraceAudioMessage(L"loaded: " + absolutePath.wstring());
  return handle;
}

bool AudioSystem::IsLoaded(SoundHandle sound) const {
  return sound < m_sounds.size() && m_sounds[sound].loaded;
}

bool AudioSystem::IsLooping(SoundHandle sound) const {
  return IsLoaded(sound) && m_sounds[sound].looping;
}

bool AudioSystem::PlayFromStart(SoundHandle sound, bool loop) {
  if (!IsLoaded(sound))
    return false;

  SoundState &state = m_sounds[sound];
  SendCommand(L"stop " + state.alias, L"stop", false);
  if (!SendCommand(L"seek " + state.alias + L" to start", L"seek")) {
    state.looping = false;
    return false;
  }

  std::wstring playCommand = L"play " + state.alias;
  if (loop)
    playCommand += L" repeat";
  const bool played = SendCommand(playCommand, L"play");
  state.looping = loop && played;
  if (!loop)
    state.looping = false;
  return played;
}

void AudioSystem::PlayOneShot(SoundHandle sound) {
  PlayFromStart(sound, false);
}

void AudioSystem::SetLooping(SoundHandle sound, bool shouldLoop) {
  if (!IsLoaded(sound))
    return;
  if (shouldLoop == m_sounds[sound].looping)
    return;
  if (shouldLoop)
    PlayFromStart(sound, true);
  else
    Stop(sound);
}

void AudioSystem::Stop(SoundHandle sound) {
  if (!IsLoaded(sound))
    return;
  SoundState &state = m_sounds[sound];
  SendCommand(L"stop " + state.alias, L"stop", false);
  SendCommand(L"seek " + state.alias + L" to start", L"seek", false);
  state.looping = false;
}

void AudioSystem::StopAll() {
  for (SoundHandle sound = 0; sound < m_sounds.size(); ++sound)
    Stop(sound);
}

void AudioSystem::Shutdown() {
  for (SoundState &sound : m_sounds) {
    if (!sound.loaded)
      continue;
    SendCommand(L"close " + sound.alias, L"close", false);
    sound.loaded = false;
    sound.looping = false;
  }
  m_sounds.clear();
}
