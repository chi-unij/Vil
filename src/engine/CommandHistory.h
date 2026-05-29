// ======================================
// File: CommandHistory.h
// Purpose: Undo/redo framework using the command pattern.
//          Milestone 4 Phase 1: Geometry Tools.
// ======================================

#pragma once

#include <memory>
#include <string>
#include <vector>

class ICommand {
public:
  virtual ~ICommand() = default;
  virtual void Execute() = 0;
  virtual void Undo() = 0;
  virtual const char *Name() const = 0;
};

class CommandHistory {
public:
  void Execute(std::unique_ptr<ICommand> cmd);
  void PushWithoutExecute(std::unique_ptr<ICommand> cmd);
  void Undo();
  void Redo();

  bool CanUndo() const { return !m_undoStack.empty(); }
  bool CanRedo() const { return !m_redoStack.empty(); }

  const char *UndoName() const;
  const char *RedoName() const;

  void Clear();

private:
  static constexpr size_t kMaxDepth = 100;

  std::vector<std::unique_ptr<ICommand>> m_undoStack;
  std::vector<std::unique_ptr<ICommand>> m_redoStack;
};
