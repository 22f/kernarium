// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;

/**
 * The host's way in: `content call --uri content://android.kernarium.sms
 * --method deliver --arg <message>` in the adb shell (Message has the
 * argument's form). Only the shell's uid and root's are served: the host
 * application reaches it over its own adb channel, and an application in the
 * guest cannot.
 * The result holds "parts", how many PDUs the message took. A message whose
 * identifier was handed over before is not handed over again; the answer is
 * the first one's (DeliveredIdentifiers).
 */
public final class InjectProvider extends ContentProvider {
    private static final String TAG = "AndariumSms";

    private final DeliveredIdentifiers delivered = new DeliveredIdentifiers();

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Bundle call(String method, String argument, Bundle extras) {
        final int uid = Binder.getCallingUid();
        if (uid != Process.SHELL_UID && uid != Process.ROOT_UID) {
            throw new SecurityException("only the adb shell may deliver a message");
        }
        if (!"deliver".equals(method)) throw new IllegalArgumentException("unknown method: " + method);
        final Message message = Message.parse(argument);
        final long identity = Binder.clearCallingIdentity();
        try {
            final DeliveredIdentifiers.Result done = delivered.deliver(
                    message.identifier, () -> Delivery.deliver(getContext(), message));
            if (done.repeat) Log.i(TAG, "a message handed over before was not handed over again");
            final Bundle result = new Bundle();
            result.putInt("parts", done.parts);
            return result;
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }
}
