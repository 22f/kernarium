// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore

extension HostDevice {
    /// An iPhone without haptics, for a test that does not care what kind of
    /// device it runs on. A test of either passes both, as the application
    /// does (`HostDevice.init`).
    public init(
        name: String, processorCount: Int, memory: MemoryPolicy, freeStorageBytes: UInt64,
        runsGuestGraphics: Bool = true, supportsPointer: Bool = true
    ) {
        self.init(
            name: name, processorCount: processorCount, memory: memory, freeStorageBytes: freeStorageBytes,
            runsGuestGraphics: runsGuestGraphics, supportsPointer: supportsPointer, idiom: .phone,
            hasHaptics: false)
    }
}
