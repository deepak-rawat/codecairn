#pragma once

#include <limits>

namespace RuntimeLimits {
inline constexpr int SymbolPageSize = 300;
inline constexpr int DefinitionCandidateLimit = 100;
inline constexpr int WorkerThreads = 2;
inline constexpr int MaximumWorkspaceRefreshMinutes = std::numeric_limits<int>::max() / 60000;
inline constexpr int ContextHistoryEntries = 32;
inline constexpr int ContextHistoryBytes = 16 * 1024 * 1024;
inline constexpr int NavigationHistoryEntries = 200;
inline constexpr int ReferenceResults = 10000;
}
