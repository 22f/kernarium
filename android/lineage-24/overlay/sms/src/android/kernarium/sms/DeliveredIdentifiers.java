// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.Map;
import java.util.function.IntSupplier;

/**
 * The identifiers of the messages this service has handed to Android since
 * it started. The host chooses an identifier for each message and sends the
 * same one again when it did not see the answer, so a second call with an
 * identifier that was handed over is answered with the first call's result
 * and hands nothing over. Deliveries run one at a time, so two calls with the
 * same identifier cannot both pass, and two messages never share a call.
 */
final class DeliveredIdentifiers {
    /** The identifiers kept; the oldest is forgotten first. */
    static final int CAPACITY = 256;

    /** What one call came to: the number of parts, and whether this call handed anything over. */
    static final class Result {
        final int parts;
        final boolean repeat;

        Result(int parts, boolean repeat) {
            this.parts = parts;
            this.repeat = repeat;
        }
    }

    private final Map<String, Integer> parts = new HashMap<>();
    private final ArrayDeque<String> order = new ArrayDeque<>();

    /**
     * Runs {@code delivery}, which returns how many parts it handed over,
     * unless {@code identifier} was handed over before. An identifier counts
     * only once its delivery returned: one that threw is tried again.
     */
    synchronized Result deliver(String identifier, IntSupplier delivery) {
        final Integer known = parts.get(identifier);
        if (known != null) return new Result(known, true);
        final int count = delivery.getAsInt();
        parts.put(identifier, count);
        order.addLast(identifier);
        if (order.size() > CAPACITY) parts.remove(order.removeFirst());
        return new Result(count, false);
    }
}
