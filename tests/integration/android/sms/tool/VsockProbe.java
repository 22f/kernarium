// SPDX-License-Identifier: Apache-2.0
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.UnixSocketAddress;
import java.io.FileDescriptor;
import java.net.SocketException;

/**
 * Tries to create a vsock socket as whoever runs it, for the shell's row of the text-message checks:
 * {@code CLASSPATH=/data/local/tmp/vsockprobe.dex app_process /system/bin VsockProbe}. It prints
 * {@code socket: errno=13 EACCES} when the domain may not, and {@code socket: ok} when it may. With
 * {@code unix PATH} it connects to the unix socket at PATH instead, for the handoff socket's row, and prints
 * {@code connect: errno=13 EACCES} when the domain may not and {@code connect: ok} when it may.
 */
public final class VsockProbe {
    /** Linux's AF_VSOCK; android.system.OsConstants has no public name for it. */
    private static final int AF_VSOCK = 40;

    private VsockProbe() {
    }

    public static void main(String[] arguments) {
        if (arguments.length == 2 && arguments[0].equals("unix")) {
            connect(arguments[1]);
            return;
        }
        try {
            final FileDescriptor descriptor = Os.socket(AF_VSOCK, OsConstants.SOCK_STREAM, 0);
            Os.close(descriptor);
            System.out.println("socket: ok");
        } catch (ErrnoException e) {
            System.out.println("socket: errno=" + e.errno + " " + OsConstants.errnoName(e.errno));
        }
    }

    private static void connect(String path) {
        try {
            final FileDescriptor descriptor = Os.socket(OsConstants.AF_UNIX, OsConstants.SOCK_STREAM, 0);
            try {
                Os.connect(descriptor, UnixSocketAddress.createFileSystem(path));
                System.out.println("connect: ok");
            } finally {
                Os.close(descriptor);
            }
        } catch (ErrnoException e) {
            System.out.println("connect: errno=" + e.errno + " " + OsConstants.errnoName(e.errno));
        } catch (SocketException e) {
            System.out.println("connect: " + e);
        }
    }
}
