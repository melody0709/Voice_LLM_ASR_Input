#pragma once

#include "globals.h"

#include <string>

std::wstring NormalizeAsrText(std::wstring text);
bool IsOperationalAsrError(const std::wstring& text);
bool IsUsableAsrTextForContext(const std::wstring& text);
bool ShouldRunLlmRefine(const Config& config, const std::wstring& text);
std::wstring AsrBackendDisplayName(const Config& config);
const char* AsrBackendDebugName(const std::wstring& asrBackend);
