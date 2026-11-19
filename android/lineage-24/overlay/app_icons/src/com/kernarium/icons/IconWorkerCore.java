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

package com.kernarium.icons;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantLock;

/**
 * The provider's hold on the isolated renderer, without the platform: binds it on the first request, keeps it for the
 * next ones, and unbinds it when it is idle, when it did not answer in time, when its process died, and after a failed
 * drawing, so that a worker that failed once is never trusted with another icon. Unbinding is what ends the isolated
 * process. One request at a time.
 *
 * <p>One deadline covers a request from its arrival: the wait for its turn, the bind and the drawing. A request whose
 * deadline passed while it queued never starts drawing, and every request is answered before the shell tool's own limit
 * (its eight seconds) by the deadline's margin. The idle cleanup never waits for the worker's turn: it runs on the
 * platform's main thread, which the bind's connection callback needs, so it only takes a turn nobody holds and does
 * nothing for a request that has come since it was scheduled.
 */
final class IconWorkerCore {
    /** The limits of one request and of an idle worker; the product's are {@link #PRODUCTION}. */
    static final class Limits {
        final long deadlineMillis;
        final long bindMillis;
        final long idleMillis;

        Limits(long deadlineMillis, long bindMillis, long idleMillis) {
            this.deadlineMillis = deadlineMillis;
            this.bindMillis = bindMillis;
            this.idleMillis = idleMillis;
        }
    }

    /** A request's whole time (its turn, the bind and the drawing), within the tool's 8 s; a bind; an idle worker. */
    static final Limits PRODUCTION = new Limits(7_000, 1_000, 5_000);

    /** What the renderer answers for one icon. */
    static final class Reply {
        static final Reply FAILED = new Reply(IconProtocol.STATUS_ERROR, 0, false, false, null);

        final int status;
        final int size;
        final boolean adaptive;
        final boolean alpha;
        final byte[] png;

        Reply(int status, int size, boolean adaptive, boolean alpha, byte[] png) {
            this.status = status;
            this.size = size;
            this.adaptive = adaptive;
            this.alpha = alpha;
            this.png = png;
        }

        boolean ok() {
            return status == IconProtocol.STATUS_OK;
        }
    }

    /** What the platform provides: a bind, a connected worker and a main thread. */
    interface Link {
        /** Begins binding the worker; null when it cannot be bound. The listener hears of the connection later. */
        Handle bind(Listener listener);
    }

    interface Handle {
        /** Ends the bind: the worker's process ends with it. */
        void unbind();
    }

    interface Listener {
        void connected(Session session);

        void lost();
    }

    interface Session {
        /** One transaction: blocks until the worker answers or dies; throws when it died or the answer is no answer. */
        Reply render(int resource, int[] kinds, String[] paths) throws Exception;

        boolean alive();
    }

    /** The platform's main thread: where the idle cleanup runs. */
    interface Poster {
        void postDelayed(Runnable task, long millis);

        void remove(Runnable task);
    }

    interface Clock {
        long now();
    }

    /** One bind: the worker it connected, and whether it did. Callbacks of an earlier bind reach an earlier object. */
    private static final class Connection implements Listener {
        private final CountDownLatch mConnected = new CountDownLatch(1);
        private volatile Session mSession;

        @Override
        public void connected(Session session) {
            mSession = session;
            mConnected.countDown();
        }

        @Override
        public void lost() {
            mSession = null;
        }
    }

    private final Link mLink;
    private final Poster mPoster;
    private final Clock mClock;
    private final Limits mLimits;
    /** The turn: one request draws at a time, in the order they came. Everything below is read and written only in it. */
    private final ReentrantLock mTurn = new ReentrantLock(true);
    private Connection mConnection;
    private Handle mHandle;
    /** Moves with every request: an idle cleanup scheduled before it is stale. */
    private long mGeneration;
    /** The idle cleanup scheduled by the last request. */
    private Runnable mIdle;

    IconWorkerCore(Link link, Poster poster, Clock clock, Limits limits) {
        mLink = link;
        mPoster = poster;
        mClock = clock;
        mLimits = limits;
    }

    /** The answer for one icon: the worker's reply, or {@link Reply#FAILED}. */
    Reply render(int resource, int[] kinds, String[] paths) {
        final long deadline = mClock.now() + mLimits.deadlineMillis;
        boolean admitted;
        try {
            admitted = mTurn.tryLock(left(deadline), TimeUnit.MILLISECONDS);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return Reply.FAILED;
        }
        if (!admitted) {
            return Reply.FAILED; // its time was spent waiting for its turn
        }
        boolean asked = false;
        Reply answer = Reply.FAILED;
        try {
            mGeneration++;
            if (mIdle != null) {
                mPoster.remove(mIdle);
                mIdle = null;
            }
            if (left(deadline) <= 0) {
                return answer; // the deadline passed while it queued: nothing is started
            }
            Session session = connect(deadline);
            if (session == null) {
                return answer;
            }
            if (left(deadline) <= 0) {
                return answer; // the bind took the rest; the worker is bound and idle
            }
            asked = true;
            answer = transact(session, resource, kinds, paths, left(deadline));
            return answer;
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return Reply.FAILED;
        } finally {
            if (asked && !answer.ok()) {
                disconnect(); // a worker that failed, or that did not answer in time, is never trusted again
            }
            scheduleIdle();
            mTurn.unlock();
        }
    }

    private long left(long deadline) {
        return deadline - mClock.now();
    }

    private Reply transact(Session session, int resource, int[] kinds, String[] paths, long millis)
            throws InterruptedException {
        final Reply[] received = new Reply[1];
        Thread transaction = new Thread(() -> {
            try {
                received[0] = session.render(resource, kinds, paths);
            } catch (Exception | LinkageError died) {
                received[0] = null;
            }
        }, "kernarium_icons-render");
        transaction.setDaemon(true);
        transaction.start();
        transaction.join(millis);
        if (transaction.isAlive()) {
            return Reply.FAILED; // the caller unbinds: the abandoned transaction ends with the worker's process
        }
        Reply reply = received[0];
        if (reply == null || !reply.ok() || reply.png == null || reply.png.length > IconProtocol.MAX_PNG_BYTES) {
            return Reply.FAILED; // the tool checks the rest; a reply over the bound is not passed on
        }
        return reply;
    }

    private Session connect(long deadline) throws InterruptedException {
        Session current = mConnection == null ? null : mConnection.mSession;
        if (current != null && current.alive()) {
            return current;
        }
        disconnect();
        final Connection connection = new Connection();
        mConnection = connection;
        mHandle = mLink.bind(connection);
        if (mHandle == null) {
            disconnect();
            return null;
        }
        long wait = Math.min(mLimits.bindMillis, left(deadline));
        if (wait <= 0 || !connection.mConnected.await(wait, TimeUnit.MILLISECONDS)) {
            disconnect();
            return null;
        }
        return connection.mSession;
    }

    private void disconnect() {
        Handle handle = mHandle;
        mHandle = null;
        mConnection = null;
        if (handle != null) {
            handle.unbind();
        }
    }

    private void scheduleIdle() {
        final long generation = mGeneration;
        if (mHandle != null) {
            mIdle = () -> {
                if (!mTurn.tryLock()) {
                    return; // a request holds the turn: it schedules the next cleanup itself
                }
                try {
                    if (generation == mGeneration) { // else a request has come since this was scheduled
                        disconnect();
                    }
                } finally {
                    mTurn.unlock();
                }
            };
            mPoster.postDelayed(mIdle, mLimits.idleMillis);
        }
    }
}
