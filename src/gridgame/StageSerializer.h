// ======================================
// File: StageSerializer.h
// Purpose: JSON save/load for StageData (stage level files).
//          Milestone 4 Phase 5: Grid & Level Editor.
// ======================================

#pragma once

#include "StageData.h"

#include <string>

namespace StageSerializer {

// Save stage to a JSON file. Returns true on success.
bool SaveToFile(const StageData &stage, const std::string &path);

// Load stage from a JSON file. Returns true on success.
bool LoadFromFile(const std::string &path, StageData &outStage);

} // namespace StageSerializer
