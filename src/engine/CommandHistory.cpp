// ======================================
// File: CommandHistory.cpp
// Purpose: Undo/redo stack implementation.
// ======================================

#include "CommandHistory.h"

void CommandHistory::Execute(std::unique_ptr<ICommand> cmd) {
  cmd->Execute();
  m_undoStack.push_back(std::move(cmd));
  m_redoStack.clear();

  if (m_undoStack.size() > kMaxDepth)
    m_undoStack.erase(m_undoStack.begin());
}

void CommandHistory::PushWithoutExecute(std::unique_ptr<ICommand> cmd) {
  m_undoStack.push_back(std::move(cmd));
  m_redoStack.clear();

  if (m_undoStack.size() > kMaxDepth)
    m_undoStack.erase(m_undoStack.begin());
}

void CommandHistory::Undo() {
  if (m_undoStack.empty())
    return;
  auto cmd = std::move(m_undoStack.back());
  m_undoStack.pop_back();
  cmd->Undo();
  m_redoStack.push_back(std::move(cmd));
}

void CommandHistory::Redo() {
  if (m_redoStack.empty())
    return;
  auto cmd = std::move(m_redoStack.back());
  m_redoStack.pop_back();
  cmd->Execute();
  m_undoStack.push_back(std::move(cmd));
}

const char *CommandHistory::UndoName() const {
  if (m_undoStack.empty())
    return nullptr;
  return m_undoStack.back()->Name();
}

const char *CommandHistory::RedoName() const {
  if (m_redoStack.empty())
    return nullptr;
  return m_redoStack.back()->Name();
}

void CommandHistory::Clear() {
  m_undoStack.clear();
  m_redoStack.clear();
}
