#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace fh6::subprocess {

std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view ws);

std::wstring quote(const std::wstring& s);

// GetStdHandle returns NULL under windowed DLL injection, which breaks
// STARTF_USESTDHANDLES; NUL is a safe substitute.
HANDLE open_nul(DWORD access);

// Shared %TEMP%\fh6-stderr.log; FILE_APPEND_DATA makes writes atomic across
// concurrent children.
HANDLE open_stderr_log();
std::filesystem::path stderr_log_path();

// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE reaps the whole tree (incl. grandchildren
// like yt-dlp's deno) when the last handle drops.
HANDLE create_kill_on_close_job();

// CREATE_SUSPENDED + AssignProcessToJobObject + ResumeThread so fast children
// can't spawn descendants outside the job.
HANDLE spawn_in_job(HANDLE job, const std::wstring& cmd, HANDLE stdin_h, HANDLE stdout_h,
                    HANDLE stderr_h);

std::string describe_launch_failure(const std::wstring& bin, DWORD ec, bool from_config);

} // namespace fh6::subprocess
