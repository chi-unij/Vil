#include "FeatureLabApp.h"

#include <Shellapi.h>
#include <Windows.h>

#include <filesystem>
#include <string_view>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  std::filesystem::path explicitFeatureFile;
  bool selfTest = false;
  int argumentCount = 0;
  wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
  if (arguments) {
    for (int index = 1; index < argumentCount; ++index) {
      if (std::wstring_view(arguments[index]) == L"--feature-file" &&
          index + 1 < argumentCount) {
        explicitFeatureFile = arguments[index + 1];
        ++index;
      } else if (std::wstring_view(arguments[index]) == L"--self-test") {
        selfTest = true;
      }
    }
    LocalFree(arguments);
  }
  FeatureLabApp app;
  return app.Run(instance, showCommand, explicitFeatureFile, selfTest);
}
