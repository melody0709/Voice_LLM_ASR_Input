#pragma once

#include <cstdarg>

namespace asr_runtime_log {

bool Enabled();
void Write(const char* format, ...);
void WriteNamedV(const wchar_t* fileName, const char* format, va_list args);

} // namespace asr_runtime_log
