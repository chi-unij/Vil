// ======================================
// File: StageSerializer.cpp
// Purpose: JSON serialization for StageData.
//          Follows the same manual JSON pattern as Entity.cpp:
//          manual construction, .contains() guards for backward compat.
//          Milestone 4 Phase 5: Grid & Level Editor.
// ======================================

#include "StageSerializer.h"

#include <nlohmann/json.hpp>

#include <fstream>

using json = nlohmann::json;

// ---- String conversion helpers ----

static const char *TileTypeToString(TileType t) {
  switch (t) {
  case TileType::Floor:     return "Floor";
  case TileType::Wall:      return "Wall";
  case TileType::Fire:      return "Fire";
  case TileType::Lightning: return "Lightning";
  case TileType::Spike:     return "Spike";
  case TileType::Ice:       return "Ice";
  case TileType::Crumble:   return "Crumble";
  case TileType::Start:     return "Start";
  case TileType::Goal:      return "Goal";
  }
  return "Floor";
}

static TileType StringToTileType(const std::string &s) {
  if (s == "Wall")      return TileType::Wall;
  if (s == "Fire")      return TileType::Fire;
  if (s == "Lightning") return TileType::Lightning;
  if (s == "Spike")     return TileType::Spike;
  if (s == "Ice")       return TileType::Ice;
  if (s == "Crumble")   return TileType::Crumble;
  if (s == "Start")     return TileType::Start;
  if (s == "Goal")      return TileType::Goal;
  return TileType::Floor;
}

static const char *TowerSideToString(TowerSide s) {
  switch (s) {
  case TowerSide::Left:   return "Left";
  case TowerSide::Right:  return "Right";
  case TowerSide::Top:    return "Top";
  case TowerSide::Bottom: return "Bottom";
  }
  return "Left";
}

static TowerSide StringToTowerSide(const std::string &s) {
  if (s == "Right")  return TowerSide::Right;
  if (s == "Top")    return TowerSide::Top;
  if (s == "Bottom") return TowerSide::Bottom;
  return TowerSide::Left;
}

static const char *TowerPatternToString(TowerPattern p) {
  switch (p) {
  case TowerPattern::Row:      return "Row";
  case TowerPattern::Column:   return "Column";
  case TowerPattern::Cross:    return "Cross";
  case TowerPattern::Diagonal: return "Diagonal";
  }
  return "Row";
}

static TowerPattern StringToTowerPattern(const std::string &s) {
  if (s == "Column")   return TowerPattern::Column;
  if (s == "Cross")    return TowerPattern::Cross;
  if (s == "Diagonal") return TowerPattern::Diagonal;
  return TowerPattern::Row;
}

// ---- Tile JSON ----

static json TileDataToJson(const TileData &t) {
  json j;
  // Only write non-default fields (sparse representation).
  if (t.type != TileType::Floor)
    j["type"] = TileTypeToString(t.type);
  if (t.hasWall)
    j["wall"] = true;
  if (t.wallDestructible)
    j["wallDestructible"] = true;
  return j;
}

static TileData JsonToTileData(const json &j) {
  TileData t;
  if (j.contains("type"))
    t.type = StringToTileType(j["type"].get<std::string>());
  if (j.contains("wall"))
    t.hasWall = j["wall"].get<bool>();
  if (j.contains("wallDestructible"))
    t.wallDestructible = j["wallDestructible"].get<bool>();
  return t;
}

// ---- Tower JSON ----

static json TowerDataToJson(const TowerData &t) {
  json j;
  j["x"] = t.x;
  j["y"] = t.y;
  j["side"] = TowerSideToString(t.side);
  j["pattern"] = TowerPatternToString(t.pattern);
  j["delay"] = t.delay;
  j["interval"] = t.interval;
  return j;
}

static TowerData JsonToTowerData(const json &j) {
  TowerData t;
  if (j.contains("x"))
    t.x = j["x"].get<int>();
  if (j.contains("y"))
    t.y = j["y"].get<int>();
  if (j.contains("side"))
    t.side = StringToTowerSide(j["side"].get<std::string>());
  if (j.contains("pattern"))
    t.pattern = StringToTowerPattern(j["pattern"].get<std::string>());
  if (j.contains("delay"))
    t.delay = j["delay"].get<float>();
  if (j.contains("interval"))
    t.interval = j["interval"].get<float>();
  return t;
}

// ---- Stage JSON ----

static json StageDataToJson(const StageData &s) {
  json j;
  j["version"] = 1;
  j["name"] = s.name;
  j["width"] = s.width;
  j["height"] = s.height;
  if (s.timeLimit > 0.0f)
    j["timeLimit"] = s.timeLimit;
  if (s.parMoves > 0)
    j["parMoves"] = s.parMoves;

  // Tiles: row-major array.
  json tilesArr = json::array();
  for (const auto &t : s.tiles)
    tilesArr.push_back(TileDataToJson(t));
  j["tiles"] = tilesArr;

  // Towers.
  if (!s.towers.empty()) {
    json towersArr = json::array();
    for (const auto &t : s.towers)
      towersArr.push_back(TowerDataToJson(t));
    j["towers"] = towersArr;
  }

  j["playerSpawn"] = json::array({s.playerSpawnX, s.playerSpawnY});
  j["cargoSpawn"] = json::array({s.cargoSpawnX, s.cargoSpawnY});
  return j;
}

static StageData JsonToStageData(const json &j) {
  StageData s;
  if (j.contains("name"))
    s.name = j["name"].get<std::string>();
  if (j.contains("width"))
    s.width = j["width"].get<int>();
  if (j.contains("height"))
    s.height = j["height"].get<int>();
  if (j.contains("timeLimit"))
    s.timeLimit = j["timeLimit"].get<float>();
  if (j.contains("parMoves"))
    s.parMoves = j["parMoves"].get<int>();

  // Tiles.
  if (j.contains("tiles") && j["tiles"].is_array()) {
    s.tiles.clear();
    for (const auto &tj : j["tiles"]) {
      if (tj.is_null()) { s.tiles.push_back(TileData{}); continue; }
      s.tiles.push_back(JsonToTileData(tj));
    }
  }
  s.EnsureSize();

  // Towers.
  if (j.contains("towers") && j["towers"].is_array()) {
    s.towers.clear();
    for (const auto &tj : j["towers"])
      s.towers.push_back(JsonToTowerData(tj));
  }

  if (j.contains("playerSpawn") && j["playerSpawn"].is_array() &&
      j["playerSpawn"].size() >= 2) {
    s.playerSpawnX = j["playerSpawn"][0].get<int>();
    s.playerSpawnY = j["playerSpawn"][1].get<int>();
  }
  if (j.contains("cargoSpawn") && j["cargoSpawn"].is_array() &&
      j["cargoSpawn"].size() >= 2) {
    s.cargoSpawnX = j["cargoSpawn"][0].get<int>();
    s.cargoSpawnY = j["cargoSpawn"][1].get<int>();
  }

  return s;
}

// ---- Public API ----

namespace StageSerializer {

bool SaveToFile(const StageData &stage, const std::string &path) {
  json j = StageDataToJson(stage);
  std::ofstream file(path);
  if (!file.is_open())
    return false;
  file << j.dump(2);
  return file.good();
}

bool LoadFromFile(const std::string &path, StageData &outStage) {
  std::ifstream file(path);
  if (!file.is_open())
    return false;
  try {
    json j = json::parse(file);
    outStage = JsonToStageData(j);
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace StageSerializer
