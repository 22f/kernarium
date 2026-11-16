// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

nonisolated extension AdbGuestOperations {
    /// Android's screen reader in the guest; nil when the guest did not
    /// answer, or a command in the read failed.
    func screenReader() async -> ScreenReaderState? {
        guard let result = try? await shell(ScreenReader.stateCommand, limit: .seconds(15)) else { return nil }
        return ScreenReader.parse(result)
    }

    /// Turns the screen reader on or off, and returns the state the same
    /// command read after it.
    func changeScreenReader(_ on: Bool, service: String) async throws -> ScreenReaderState {
        let result: ShellResult
        do {
            result = try await shell(ScreenReader.changeCommand(turningOn: on, service: service), limit: .seconds(30))
        } catch {
            throw AdbOperationRunner.presented(error, activity: .screenReader)
        }
        guard result.exitCode == 0 else {
            let said = String(decoding: result.stderr, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            throw AdbFailure(message: said.isEmpty ? AdbFailure(.io(EIO), activity: .screenReader).message : said)
        }
        // The command ends with the report, so an answer that is not one says
        // the change cannot be told to have happened.
        guard let state = ScreenReader.parse(result) else {
            throw AdbFailure(.io(EIO), activity: .screenReader)
        }
        return state
    }

    /// Opens Android's Accessibility page on the guest's screen.
    func openAccessibilitySettings() async throws {
        do {
            _ = try await shell(ScreenReader.settingsCommand, limit: .seconds(15))
        } catch {
            throw AdbOperationRunner.presented(error, activity: .screenReader)
        }
    }
}
