// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.kernarium.apps;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.TimeUnit;

/**
 * Runs one child process (the argument vector is a process's, never a shell's command line) and returns its
 * status and the first bytes of its output. The output is read while the process runs, so that a chatty
 * child cannot fill the pipe and stall; the whole run has one deadline; and the child is terminated and
 * reaped, in a {@code finally}, however the run ends (a deadline, an interrupt, a failure after the spawn).
 */
final class ChildProcess {
    /** How long the output is read on after the process has ended (a descendant may hold the pipe open). */
    static final long DRAIN_MILLIS = 2_000;
    /** How long a terminated child is given to end, once with SIGTERM and once with SIGKILL. */
    static final long GRACE_MILLIS = 2_000;

    /** How the child ended and its first output (standard output and error together). */
    static final class Result {
        final int exit;
        final String output;

        Result(int exit, String output) {
            this.exit = exit;
            this.output = output;
        }
    }

    private ChildProcess() {}

    /** Runs {@code argv}; keeps at most {@code keep} bytes of output; fails when it is not over after {@code limitMillis}. */
    static Result run(List<String> argv, long limitMillis, int keep) throws Exception {
        Process process = null;
        try {
            process = new ProcessBuilder(argv).redirectErrorStream(true).start();
            process.getOutputStream().close();
            final InputStream output = process.getInputStream();
            final ByteArrayOutputStream kept = new ByteArrayOutputStream();
            Thread reader = new Thread(() -> drain(output, kept, keep), "kernarium_apps-output");
            reader.setDaemon(true);
            reader.start();
            if (!process.waitFor(limitMillis, TimeUnit.MILLISECONDS)) {
                throw new PlatformException("cmd activity start-activity did not finish");
            }
            reader.join(DRAIN_MILLIS);
            String text;
            synchronized (kept) {
                text = new String(kept.toByteArray(), StandardCharsets.UTF_8);
            }
            return new Result(process.exitValue(), text);
        } finally {
            if (process != null) {
                terminate(process);
            }
        }
    }

    private static void drain(InputStream in, ByteArrayOutputStream kept, int keep) {
        byte[] buffer = new byte[4096];
        try {
            int n;
            while ((n = in.read(buffer)) != -1) {
                synchronized (kept) {
                    int room = keep - kept.size();
                    if (room > 0) {
                        kept.write(buffer, 0, Math.min(n, room));
                    }
                }
            }
        } catch (IOException ended) {
            // The pipe was closed under the reader: what was read is what there is.
        }
    }

    /** The child ended, or SIGKILL has been sent and waited for (a bounded time each). The streams are closed. */
    private static void terminate(Process process) {
        try {
            if (process.isAlive()) {
                process.destroy();
                if (!process.waitFor(GRACE_MILLIS, TimeUnit.MILLISECONDS)) {
                    process.destroyForcibly();
                    process.waitFor(GRACE_MILLIS, TimeUnit.MILLISECONDS);
                }
            }
        } catch (InterruptedException interrupted) {
            process.destroyForcibly();
            Thread.currentThread().interrupt();
        } finally {
            try {
                process.getInputStream().close();
            } catch (IOException ignored) {
                // Nothing more to do with it.
            }
        }
    }
}
