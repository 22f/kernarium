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

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The provider's hold on the isolated renderer: binds it on the first request, keeps it for the next ones, and
 * unbinds it when it is idle, when it did not answer in time, when its process died, and after a failed drawing,
 * so that a worker that failed once is never trusted with another icon. Unbinding is what ends the isolated
 * process. One request at a time.
 */
final class IconWorker {
    /** How long a bind may take, how long one drawing may take, how long an idle worker is kept. */
    static final long BIND_MILLIS = 1_000;
    static final long RENDER_MILLIS = 6_500;
    static final long IDLE_MILLIS = 5_000;

    private final Context mContext;
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private final Runnable mIdle = new Runnable() {
        @Override
        public void run() {
            synchronized (IconWorker.this) {
                disconnect();
            }
        }
    };
    private IBinder mBinder;
    private boolean mBound;
    private CountDownLatch mConnected;
    private final ServiceConnection mConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            mBinder = service;
            mConnected.countDown();
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            mBinder = null;
        }

        @Override
        public void onBindingDied(ComponentName name) {
            mBinder = null;
        }
    };

    IconWorker(Context context) {
        mContext = context;
    }

    /** The answer for one icon: the reply's fields, or a failed status. */
    synchronized Bundle render(int resource, int[] kinds, String[] paths) {
        mMain.removeCallbacks(mIdle);
        Bundle answer = failed();
        Parcel data = null;
        Parcel reply = null;
        try {
            final IBinder worker = connect();
            if (worker == null) {
                return answer;
            }
            data = Parcel.obtain();
            reply = Parcel.obtain();
            final Parcel sent = data;
            final Parcel received = reply;
            sent.writeInt(resource);
            sent.writeIntArray(kinds);
            sent.writeStringArray(paths);
            final boolean[] done = new boolean[1];
            Thread transaction = new Thread(() -> {
                try {
                    worker.transact(IconRenderService.TRANSACTION_RENDER, sent, received, 0);
                    done[0] = true;
                } catch (Exception | LinkageError died) {
                    done[0] = false;
                }
            }, "kernarium_icons-render");
            transaction.setDaemon(true);
            transaction.start();
            transaction.join(RENDER_MILLIS);
            if (transaction.isAlive() || !done[0]) {
                return answer; // finally ends the worker
            }
            try {
                answer = read(received);
            } catch (RuntimeException bad) {
                answer = failed();
            }
            return answer;
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return answer;
        } finally {
            boolean good = answer.getInt(IconProtocol.KEY_STATUS, IconProtocol.STATUS_ERROR) == IconProtocol.STATUS_OK;
            if (!good) {
                disconnect();
            }
            if (good) {
                data.recycle();
                reply.recycle();
            } // else a transaction that did not end may still hold the parcels: they are left to the collector
            if (mBound) {
                mMain.postDelayed(mIdle, IDLE_MILLIS);
            }
        }
    }

    private static Bundle read(Parcel reply) {
        reply.setDataPosition(0);
        reply.readException();
        Bundle answer = new Bundle();
        int status = reply.readInt();
        if (status != IconProtocol.STATUS_OK) {
            return failed();
        }
        int size = reply.readInt();
        boolean adaptive = reply.readInt() != 0;
        boolean alpha = reply.readInt() != 0;
        byte[] png = reply.createByteArray();
        if (png == null || png.length > IconProtocol.MAX_PNG_BYTES) {
            return failed(); // the tool checks the rest; a reply over the bound is not passed on
        }
        answer.putInt(IconProtocol.KEY_STATUS, status);
        answer.putInt(IconProtocol.KEY_SIZE, size);
        answer.putBoolean(IconProtocol.KEY_ADAPTIVE, adaptive);
        answer.putBoolean(IconProtocol.KEY_ALPHA, alpha);
        answer.putByteArray(IconProtocol.KEY_PNG, png);
        return answer;
    }

    static Bundle failed() {
        Bundle answer = new Bundle();
        answer.putInt(IconProtocol.KEY_STATUS, IconProtocol.STATUS_ERROR);
        return answer;
    }

    private IBinder connect() throws InterruptedException {
        IBinder current = mBinder;
        if (mBound && current != null && current.isBinderAlive()) {
            return current;
        }
        disconnect();
        mConnected = new CountDownLatch(1);
        Intent intent = new Intent().setClassName(IconProtocol.PACKAGE, IconProtocol.SERVICE_CLASS);
        mBound = mContext.bindService(intent, mConnection, Context.BIND_AUTO_CREATE);
        if (!mBound || !mConnected.await(BIND_MILLIS, TimeUnit.MILLISECONDS)) {
            disconnect();
            return null;
        }
        return mBinder;
    }

    private void disconnect() {
        if (mBound) {
            try {
                mContext.unbindService(mConnection);
            } catch (IllegalArgumentException notBound) {
                // Already gone.
            }
            mBound = false;
        }
        mBinder = null;
    }
}
