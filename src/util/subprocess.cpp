/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "util/subprocess.h"

#include <cstdlib>

#ifndef _WIN32
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
#ifndef _WIN32
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

/* TODO: Windows has no cancel yet, so this is system() with a flag in
 * front of it and cancel() only stops a command that has not started.
 *
 * What it needs is the same rule the process group gives elsewhere --
 * take the whole tree, since the compiler driver's children are where
 * the time goes. CreateProcess the command suspended, put the process
 * in a job object created with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
 * resume it, and wait on the process handle; cancel() then calls
 * TerminateJobObject on the job, which reaches the children the job
 * holds. Keep the job handle where cancel() can find it, under the
 * same mutex the group is kept under below, and close it when run()
 * returns. It is written this way rather than guessed at because this
 * has to be built and tested on Windows. */
int Subprocess::run(const string &command)
{
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) {
      return -1;
    }
  }
  return system(command.c_str());
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
