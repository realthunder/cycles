/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "util/subprocess.h"

#include <cstdlib>

#ifdef _WIN32
#  include "util/log.h"
#  include "util/vector.h"
#  include "util/windows.h"
#else
#  include <cerrno>
#  include <csignal>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>

extern char **environ;
#endif

CCL_NAMESPACE_BEGIN

bool Subprocess::cancelled()
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return cancelled_;
}

void Subprocess::cancel()
{
  const std::lock_guard<std::mutex> lock(mutex_);
  cancelled_ = true;
#ifdef _WIN32
  if (job_ != nullptr) {
    /* The job and not the command's own process, for the reason the
     * process group is used elsewhere: the compiler driver's children are
     * where the time is spent, and the job is what holds them. There is
     * nothing to clean up but the temporary output file, which the caller
     * removes. */
    TerminateJobObject((HANDLE)job_, 1);
    killed_ = true;
  }
#else
  if (group_ > 0) {
    /* The whole group: the compiler's own children are what the time is
     * actually spent in. SIGKILL because run() is already blocked in
     * waitpid and there is nothing to clean up but the temporary output
     * file, which the caller removes. */
    kill(-group_, SIGKILL);
  }
#endif
}

#ifdef _WIN32

namespace {

/* The standard handles, duplicated inheritable, so the child can be given
 * exactly those and nothing else.
 *
 * system() ran the command on the caller's handles, and a compiler's
 * diagnostics are the "see console for details" that a failed compile
 * refers to, so they have to keep arriving wherever the application's
 * output goes -- a redirected log included. What is worth avoiding is the
 * other half of what system() does: inheriting everything that happens to
 * be inheritable in a process this size. A handle list makes the set
 * explicit. Duplicating also keeps the list free of the repeats it
 * rejects, since stdout and stderr are commonly one handle. */
class InheritedStdHandles {
 public:
  InheritedStdHandles()
  {
    static const DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    const HANDLE self = GetCurrentProcess();
    for (int i = 0; i < 3; i++) {
      const HANDLE original = GetStdHandle(ids[i]);
      if (original == nullptr || original == INVALID_HANDLE_VALUE) {
        /* A GUI process that was never given a console has none of these,
         * and the child then gets a console of its own, as it did under
         * system(). */
        continue;
      }
      if (DuplicateHandle(self, original, self, &handles_[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        list_[count_++] = handles_[i];
      }
    }
  }

  ~InheritedStdHandles()
  {
    for (const HANDLE handle : handles_) {
      if (handle != nullptr) {
        CloseHandle(handle);
      }
    }
  }

  InheritedStdHandles(const InheritedStdHandles &) = delete;
  InheritedStdHandles &operator=(const InheritedStdHandles &) = delete;

  bool empty() const
  {
    return count_ == 0;
  }
  HANDLE *list()
  {
    return list_;
  }
  size_t size() const
  {
    return count_;
  }
  HANDLE input() const
  {
    return handles_[0];
  }
  HANDLE output() const
  {
    return handles_[1];
  }
  HANDLE error() const
  {
    return handles_[2];
  }

 private:
  HANDLE handles_[3] = {nullptr, nullptr, nullptr};
  HANDLE list_[3] = {nullptr, nullptr, nullptr};
  size_t count_ = 0;
};

}  // namespace

int Subprocess::run(const string &command)
{
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) {
      return -1;
    }
    killed_ = false;
  }

  /* A job object is what a process group is elsewhere: everything the
   * command starts is in it, so one call takes the whole tree.
   * KILL_ON_JOB_CLOSE covers the paths that never reach cancel() as well
   * -- whatever is still in the job dies when run() drops the last handle
   * to it. */
  const HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    return -1;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    CloseHandle(job);
    return -1;
  }

  /* `/s` makes cmd's quoting rule the simple one: strip the first and last
   * quote and take the rest of the line verbatim. Every command that gets
   * here carries quoted paths of its own, which the rule without `/s`
   * counts and then takes apart. The shell is named in the command line
   * rather than in lpApplicationName, which searches no PATH. */
  const wchar_t *comspec = _wgetenv(L"ComSpec");
  const wstring shell = (comspec != nullptr) ? wstring(comspec) : wstring(L"cmd.exe");
  const wstring line = L"\"" + shell + L"\" /s /c \"" + string_to_wstring(command) + L"\"";
  vector<wchar_t> line_buffer(line.begin(), line.end());
  line_buffer.push_back(L'\0');

  InheritedStdHandles std_handles;

  vector<char> attribute_buffer;
  LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
  if (!std_handles.empty()) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    attribute_buffer.resize(size);
    LPPROC_THREAD_ATTRIBUTE_LIST list = (LPPROC_THREAD_ATTRIBUTE_LIST)attribute_buffer.data();
    if (InitializeProcThreadAttributeList(list, 1, 0, &size)) {
      if (UpdateProcThreadAttribute(list,
                                    0,
                                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    std_handles.list(),
                                    std_handles.size() * sizeof(HANDLE),
                                    nullptr,
                                    nullptr))
      {
        attributes = list;
      }
      else {
        DeleteProcThreadAttributeList(list);
      }
    }
  }

  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = (attributes != nullptr) ? sizeof(startup) : sizeof(startup.StartupInfo);
  if (attributes != nullptr) {
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = std_handles.input();
    startup.StartupInfo.hStdOutput = std_handles.output();
    startup.StartupInfo.hStdError = std_handles.error();
    startup.lpAttributeList = attributes;
  }

  /* Suspended, so that it is in the job before it runs at all: a shell
   * that got as far as starting the compiler would leave that outside the
   * job, and out of the cancel's reach. */
  DWORD flags = CREATE_SUSPENDED;
  if (attributes != nullptr) {
    flags |= EXTENDED_STARTUPINFO_PRESENT;
  }

  PROCESS_INFORMATION process = {};
  const BOOL started = CreateProcessW(nullptr,
                                      line_buffer.data(),
                                      nullptr,
                                      nullptr,
                                      (attributes != nullptr) ? TRUE : FALSE,
                                      flags,
                                      nullptr,
                                      nullptr,
                                      &startup.StartupInfo,
                                      &process);
  if (attributes != nullptr) {
    DeleteProcThreadAttributeList(attributes);
  }
  if (!started) {
    CloseHandle(job);
    return -1;
  }

  HANDLE holder = job;
  if (!AssignProcessToJobObject(job, process.hProcess)) {
    /* Windows nests jobs since 8, so being in one already is not the
     * refusal it used to be; whatever this is, the command still has to
     * run. It runs uncancellable, which is what it did before. */
    LOG_WARNING << "Could not put a command in a job object (error " << GetLastError()
                << "); it cannot be cancelled.";
    CloseHandle(job);
    holder = nullptr;
  }

  ResumeThread(process.hThread);

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) {
      /* Cancelled while it was starting. */
      if (holder != nullptr) {
        TerminateJobObject(holder, 1);
        killed_ = true;
      }
    }
    else {
      job_ = holder;
    }
  }

  WaitForSingleObject(process.hProcess, INFINITE);

  DWORD status = 1;
  if (!GetExitCodeProcess(process.hProcess, &status)) {
    status = 1;
  }

  bool killed = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    job_ = nullptr;
    killed = killed_;
  }

  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (holder != nullptr) {
    /* The last handle to the job, which takes anything the command left
     * running behind it. */
    CloseHandle(holder);
  }

  /* A killed command's exit status is only what the kill put there, and a
   * cancel is not a failure to report. */
  return killed ? -1 : (int)status;
}

#else

int Subprocess::run(const string &command)
{
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) {
      return -1;
    }
  }

  /* posix_spawn rather than fork(): this process has render threads, and
   * only the spawn's own attributes are needed anyway. */
  posix_spawnattr_t attr;
  if (posix_spawnattr_init(&attr) != 0) {
    return -1;
  }
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);

  char *const argv[] = {const_cast<char *>("sh"),
                        const_cast<char *>("-c"),
                        const_cast<char *>(command.c_str()),
                        nullptr};
  pid_t pid = -1;
  const int err = posix_spawn(&pid, "/bin/sh", nullptr, &attr, argv, environ);
  posix_spawnattr_destroy(&attr);
  if (err != 0 || pid <= 0) {
    return -1;
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) {
      /* Cancelled while it was starting. */
      kill(-pid, SIGKILL);
    }
    else {
      group_ = pid;
    }
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      status = -1;
      break;
    }
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    group_ = -1;
  }

  if (status != -1 && WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  /* Signalled (a cancel) or not waited for. */
  return -1;
}

#endif

CCL_NAMESPACE_END
