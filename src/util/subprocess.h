/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <mutex>

#include "util/string.h"

CCL_NAMESPACE_BEGIN

/* A shell command run as a child this process can still reach.
 *
 * The kernel compilers run for minutes, and starting them with system()
 * left the caller no way to stop one: a device asked to cancel had to
 * wait for the compile to end, and so did everything joining that
 * thread -- closing a session while its kernels were building blocked
 * for the whole compile.
 *
 * The child is put in a process group of its own, so cancel() from
 * another thread takes down the compiler and every tool it drives
 * (nvcc runs cicc and ptxas as separate processes). A cancel that
 * arrives before the command starts stops it from starting at all.
 *
 * NOT IMPLEMENTED ON WINDOWS. There the command still runs through
 * system() and cancel() only raises the flag, so a compile already
 * started runs to the end and whoever joins that thread waits for it,
 * exactly as before. See the note in subprocess.cpp for what the
 * implementation needs.
 *
 * One command at a time per instance; cancel() may be called from any
 * thread while run() is in flight. */
class Subprocess {
 public:
  Subprocess() = default;
  ~Subprocess() = default;

  Subprocess(const Subprocess &) = delete;
  Subprocess &operator=(const Subprocess &) = delete;

  /* Run the command and wait for it. Returns its exit status: 0 when it
   * succeeded, non-zero when it failed, could not be started, or was
   * cancelled. */
  int run(const string &command);

  /* From any thread: stop the running command and make every later
   * run() fail at once. Returns without waiting for the child. */
  void cancel();

  bool cancelled();

 private:
  std::mutex mutex_;
  bool cancelled_ = false;
#ifndef _WIN32
  /* The running child's process group, -1 when nothing runs. */
  int group_ = -1;
#endif
};

CCL_NAMESPACE_END
