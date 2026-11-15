// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import android.Manifest;
import android.app.ActivityManager;
import android.app.AppOpsManager;
import android.app.role.RoleManager;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.UserHandle;
import android.provider.Telephony;
import android.util.Log;
import java.security.SecureRandom;
import java.util.List;
import java.util.TimeZone;

/**
 * Hands a message to the guest the way InboundSmsHandler hands one from the
 * network: SMS_DELIVER to the default SMS application, which stores it and
 * notifies, then SMS_RECEIVED to every application that may receive SMS. With
 * no SMS application the message is stored in the inbox here.
 */
final class Delivery {
    private static final String TAG = "AndariumSms";
    private static final String MESSAGING = "com.android.messaging";
    private static final SecureRandom RANDOM = new SecureRandom();

    private Delivery() {}

    /** Returns how many parts the message took. */
    static int deliver(Context context, Message message) {
        final long now = System.currentTimeMillis();
        final byte[][] pdus = DeliverPdu.encode(
                message.sender, message.body, now, TimeZone.getDefault(), RANDOM.nextInt(256));
        final UserHandle user = UserHandle.of(ActivityManager.getCurrentUser());
        final String application = smsApplication(context, user);
        if (application != null) {
            final Intent deliver = intent(Telephony.Sms.Intents.SMS_DELIVER_ACTION, pdus);
            deliver.setPackage(application);
            context.sendBroadcastAsUser(deliver, user, Manifest.permission.RECEIVE_SMS,
                    AppOpsManager.OP_RECEIVE_SMS);
        } else {
            store(context, user, message, now);
        }
        context.sendBroadcastAsUser(intent(Telephony.Sms.Intents.SMS_RECEIVED_ACTION, pdus), user,
                Manifest.permission.RECEIVE_SMS, AppOpsManager.OP_RECEIVE_SMS);
        Log.i(TAG, "delivered a message of " + pdus.length + " part(s) to "
                + (application != null ? application : "the inbox"));
        return pdus.length;
    }

    private static Intent intent(String action, byte[][] pdus) {
        final Intent intent = new Intent(action);
        intent.putExtra("pdus", pdus);
        intent.putExtra("format", "3gpp");
        intent.addFlags(Intent.FLAG_RECEIVER_FOREGROUND);
        return intent;
    }

    /** The SMS role holder, else Messaging when installed, else none. */
    private static String smsApplication(Context context, UserHandle user) {
        final RoleManager roles = context.getSystemService(RoleManager.class);
        if (roles != null) {
            final List<String> holders = roles.getRoleHoldersAsUser(RoleManager.ROLE_SMS, user);
            if (!holders.isEmpty()) return holders.get(0);
        }
        try {
            context.createContextAsUser(user, 0).getPackageManager()
                    .getApplicationInfo(MESSAGING, PackageManager.ApplicationInfoFlags.of(0));
            return MESSAGING;
        } catch (PackageManager.NameNotFoundException e) {
            return null;
        }
    }

    private static void store(Context context, UserHandle user, Message message, long now) {
        final ContentValues values = new ContentValues();
        values.put(Telephony.Sms.ADDRESS, message.sender);
        values.put(Telephony.Sms.BODY, message.body);
        values.put(Telephony.Sms.DATE, now);
        values.put(Telephony.Sms.DATE_SENT, now);
        values.put(Telephony.Sms.READ, 0);
        values.put(Telephony.Sms.SEEN, 0);
        context.createContextAsUser(user, 0).getContentResolver()
                .insert(Telephony.Sms.Inbox.CONTENT_URI, values);
    }
}
