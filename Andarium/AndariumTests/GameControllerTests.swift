// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import CoreHaptics
import Darwin
@preconcurrency import GameController
import Testing
import UIKit
@testable import Andarium

/// What the application does with game controllers, without a real one: no
/// controller is attached to a test host, nothing is scanned for or paired,
/// and the relay is a fake on a local socket. The mapping is written element
/// by element and read here from plain readings and from a software
/// controller whose elements the test sets (`GCController.controllerWithExtendedGamepad`).
struct GameControllerMappingTests {
    typealias Element = GamepadReading.Element

    private func pressed(_ elements: Element...) -> GamepadReading {
        var reading = GamepadReading()
        for element in elements { reading.buttons[element] = .init(pressed: true, value: 1) }
        return reading
    }

    @Test func nothingPressedIsTheNeutralState() {
        #expect(GamepadInputMapper.state(GamepadReading()) == .neutral)
    }

    @Test func eachButtonMapsToItsOwnGuestButtonAndNoOther() {
        let expected: [(Element, GamepadButtons)] = [
            (.a, .south), (.b, .east), (.x, .west), (.y, .north), (.leftShoulder, .leftShoulder),
            (.rightShoulder, .rightShoulder), (.leftTrigger, .leftTrigger), (.rightTrigger, .rightTrigger),
            (.options, .select), (.menu, .start), (.home, .mode), (.share, .record),
            (.leftThumbstickButton, .leftThumbstick), (.rightThumbstickButton, .rightThumbstick),
        ]
        #expect(Set(expected.map(\.0)) == Set(Element.allCases))
        for (element, button) in expected {
            #expect(GamepadInputMapper.state(pressed(element)).buttons == button, "\(element)")
        }
    }

    @Test func aTriggerIsItsAnalogValueAndItsPress() {
        var reading = GamepadReading()
        reading.buttons[.leftTrigger] = .init(pressed: false, value: 0.3)
        reading.buttons[.rightTrigger] = .init(pressed: true, value: 0.9)
        let state = GamepadInputMapper.state(reading)
        #expect(state.leftTrigger == 0.3)
        #expect(state.rightTrigger == 0.9)
        #expect(state.buttons == .rightTrigger)
    }

    @Test func sticksAndTheDirectionPadAreCarriedAsTheyAre() {
        let reading = GamepadReading(
            leftStick: GamepadStick(x: -0.5, y: 0.75), rightStick: GamepadStick(x: 1, y: -1), directions: [.up, .right])
        let state = GamepadInputMapper.state(reading)
        #expect(state.leftStick == GamepadStick(x: -0.5, y: 0.75))
        #expect(state.rightStick == GamepadStick(x: 1, y: -1))
        #expect(state.directions == [.up, .right])
    }

    @Test func aProductCategoryChoosesTheGuestDevice() {
        #expect(GamepadInputMapper.family(productCategory: GCProductCategoryDualSense, name: "DualSense Wireless Controller") == .dualSense)
        #expect(GamepadInputMapper.family(productCategory: GCProductCategoryDualSense, name: "DualSense Edge Wireless Controller") == .dualSenseEdge)
        #expect(GamepadInputMapper.family(productCategory: GCProductCategoryDualShock4, name: "DUALSHOCK 4 Wireless Controller") == .dualShock4)
        #expect(GamepadInputMapper.family(productCategory: GCProductCategoryXboxOne, name: "Xbox Wireless Controller") == .xbox)
        for category in [GCProductCategoryMFi, GCProductCategoryHID, GCProductCategoryArcadeStick, "Anything"] {
            #expect(GamepadInputMapper.family(productCategory: category, name: "Pad") == .generic, "\(category)")
        }
    }

    @Test func aControllersMotorsAreHandlesOneEngineOrNothing() {
        #expect(GamepadInputMapper.haptics(nil) == GamepadHaptics.none)
        #expect(GamepadInputMapper.haptics([.default, .all]) == .single)
        #expect(GamepadInputMapper.haptics([.default, .all, .leftHandle]) == .single)
        #expect(GamepadInputMapper.haptics([.default, .all, .handles, .leftHandle, .rightHandle]) == .handles)
    }

    @Test func aControllerWithNoBatteryReadsFull() {
        #expect(GamepadInputMapper.battery(nil) == .none)
    }

    // MARK: A software controller

    @Test func aSoftwareControllersElementsReadThroughItsProfile() throws {
        let controller = GCController.withExtendedGamepad()
        let pad = try #require(controller.extendedGamepad)
        pad.buttonA.setValue(1)
        pad.buttonY.setValue(1)
        pad.rightShoulder.setValue(1)
        pad.leftTrigger.setValue(0.6)
        pad.leftThumbstick.setValueForXAxis(0.5, yAxis: -0.25)
        pad.rightThumbstick.setValueForXAxis(-1, yAxis: 1)
        pad.dpad.setValueForXAxis(1, yAxis: 0)
        pad.buttonMenu.setValue(1)
        pad.buttonOptions?.setValue(1)
        let state = GamepadInputMapper.state(GamepadReading(profile: controller.physicalInputProfile))
        #expect(state.buttons == [.south, .north, .rightShoulder, .leftTrigger, .start, .select])
        #expect(state.leftTrigger == 0.6)
        #expect(state.leftStick == GamepadStick(x: 0.5, y: -0.25))
        #expect(state.rightStick == GamepadStick(x: -1, y: 1))
        #expect(state.directions == .right)
    }

    @Test func aSoftwareControllerListsTheElementsItHas() throws {
        let controller = GCController.withExtendedGamepad()
        let elements = GamepadInputMapper.elements(of: controller.physicalInputProfile)
        #expect(elements.leftStick)
        #expect(elements.rightStick)
        #expect(elements.directionPad)
        #expect(elements.buttons.isSuperset(of: [.south, .east, .west, .north, .leftShoulder, .rightShoulder]))
        #expect(elements.buttons.isSuperset(of: [.leftThumbstick, .rightThumbstick, .select, .start, .mode]))
    }

    // MARK: Rumble engines and indicators

    /// A source that always fails the same way, to tell which one a target used.
    private nonisolated struct Failing: HapticEngineSource {
        var code: CHHapticError.Code
        func engine(for target: HapticTarget) throws -> CHHapticEngine { throw CHHapticError(code) }
    }

    @Test func aTargetUsesTheSourceOfItsOwnActuator() {
        let sources = HapticEngineSources(device: Failing(code: .notSupported), controllers: Failing(code: .engineNotRunning))
        func code(_ target: HapticTarget) -> CHHapticError.Code? {
            do { _ = try sources.engine(for: target) } catch let error as CHHapticError { return error.code } catch {}
            return nil
        }
        #expect(code(.device) == .notSupported)
        #expect(code(.controller(slot: 0, locality: .leftHandle)) == .engineNotRunning)
        #expect(code(.controller(slot: 7, locality: .standard)) == .engineNotRunning)
    }

    @Test func aControllerTargetHasNoEngineWhenNoControllerHoldsItsSlot() {
        let engines = GameControllerEngines()
        #expect(throws: (any Error).self) { try engines.engine(for: .controller(slot: 0, locality: .leftHandle)) }
        engines.setResolver { _ in nil }
        #expect(throws: (any Error).self) { try engines.engine(for: .controller(slot: 0, locality: .leftHandle)) }
        // A software controller has no actuators.
        let software = GCController.withExtendedGamepad()
        engines.setResolver { _ in software }
        #expect(throws: (any Error).self) { try engines.engine(for: .controller(slot: 0, locality: .standard)) }
        // And the device's own target is not a controller's.
        #expect(throws: (any Error).self) { try engines.engine(for: .device) }
    }

    @Test func theLocalitiesAreIOSsOwn() {
        #expect(GameControllerEngines.locality(.standard) == .default)
        #expect(GameControllerEngines.locality(.leftHandle) == .leftHandle)
        #expect(GameControllerEngines.locality(.rightHandle) == .rightHandle)
    }

    @MainActor @Test func theIndicatorsSetThePlayerAndSurviveAControllerWithNoLight() async {
        let table = ControllerTable()
        let software = GCController.withExtendedGamepad()
        let entry = table.add(software)
        let indicators = GameControllerIndicators(table: table)
        // No light on this controller, and an id nobody holds: nothing happens and nothing fails.
        indicators.setLight(controller: entry?.id ?? 0, red: 255, green: 0, blue: 0)
        indicators.setLight(controller: 99, red: 1, green: 2, blue: 3)
        indicators.setPlayerIndex(controller: 99, index: 1)
        indicators.setPlayerIndex(controller: entry?.id ?? 0, index: 1)
        let applied = await eventually { software.playerIndex == .index2 }
        #expect(applied)
        indicators.setPlayerIndex(controller: entry?.id ?? 0, index: nil)
        #expect(await eventually { software.playerIndex == .indexUnset })
    }

    @Test func theTableNumbersControllersOnceAndNeverReusesANumber() {
        let table = ControllerTable()
        let first = GCController.withExtendedGamepad()
        let second = GCController.withExtendedGamepad()
        let one = table.add(first)
        #expect(table.add(first) == nil)
        let two = table.add(second)
        #expect(one?.id == 1)
        #expect(two?.id == 2)
        #expect(table.controller(id: 2) === second)
        #expect(table.remove(first)?.id == 1)
        #expect(table.remove(first) == nil)
        #expect(table.add(first)?.id == 3)
        #expect(!table.isEmpty)
    }

    @Test func theSessionViewTakesControllerEventsThroughGameController() {
        let view = GameControllerEventInteraction.makeView()
        let interactions = view.interactions.compactMap { $0 as? GCEventInteraction }
        #expect(interactions.count == 1)
        #expect(interactions.first?.handledEventTypes == .gamepad)
    }

    @Test func theApplicationDeclaresTheExtendedGamepadProfile() throws {
        let declared = Bundle.main.object(forInfoDictionaryKey: "GCSupportedGameControllers") as? [[String: String]]
        #expect(declared == [["ProfileName": "ExtendedGamepad"]])
    }
}

/// The hot plug of a controller, as the application sees it, against a fake
/// relay on a local socket.
@MainActor
struct GameControllersTests {
    /// The guest's end: it accepts, keeps the peer, and reads and speaks frames.
    private final class Relay: GuestStreamOpener, @unchecked Sendable {
        private let lock = NSLock()
        private var peers: [Int32] = []
        private var reader = GamepadHostFrameReader()
        private var queued: [GamepadHostFrame] = []

        var connections: Int { lock.withLock { peers.count } }
        deinit { for peer in peers { close(peer) } }

        func openStream() throws -> Int32 {
            try lock.withLock {
                var sockets: [Int32] = [-1, -1]
                guard socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0 else { throw POSIXError(.EIO) }
                peers.append(sockets[1])
                return sockets[0]
            }
        }

        func send(_ frame: GamepadGuestFrame) {
            guard let peer = lock.withLock({ peers.last }) else { return }
            _ = frame.encoded.withUnsafeBytes { Darwin.send(peer, $0.baseAddress, $0.count, 0) }
        }

        /// The next frame of the host, or nil after three seconds. It waits
        /// without holding the main queue, which the application's own
        /// handlers run on.
        func next() async -> GamepadHostFrame? {
            let deadline = Date().addingTimeInterval(3)
            while Date() < deadline {
                if let frame = readOne() { return frame }
                try? await Task.sleep(for: .milliseconds(5))
            }
            return nil
        }

        private func readOne() -> GamepadHostFrame? {
            lock.withLock {
                if !queued.isEmpty { return queued.removeFirst() }
                guard let peer = peers.last else { return nil }
                var bytes = [UInt8](repeating: 0, count: 8192)
                let size = recv(peer, &bytes, bytes.count, MSG_DONTWAIT)
                guard size > 0 else { return nil }
                queued.append(contentsOf: reader.feed(bytes.prefix(size)).frames)
                return queued.isEmpty ? nil : queued.removeFirst()
            }
        }

        /// Whether the host closed the connection, within three seconds.
        func closedByHost() async -> Bool {
            guard let peer = lock.withLock({ peers.last }) else { return false }
            let deadline = Date().addingTimeInterval(3)
            while Date() < deadline {
                var byte: UInt8 = 0
                if recv(peer, &byte, 1, MSG_DONTWAIT) == 0 { return true }
                try? await Task.sleep(for: .milliseconds(5))
            }
            return false
        }
    }

    private nonisolated final class Player: HapticPlayer, @unchecked Sendable {
        func play(_ score: HapticScore) -> HapticPlayResult { .failed }
        func setToneIntensity(_ intensity: Float, on target: HapticTarget) {}
        func stop(_ target: HapticTarget) {}
        func stopAll() {}
        func prepare() {}
    }

    private func make(
        _ relay: Relay, center: NotificationCenter, controllers: @escaping @Sendable () -> [GCController]
    ) -> GameControllers {
        GameControllers(
            player: Player(), engines: GameControllerEngines(), opener: relay, center: center, discover: controllers,
            retryDelay: .milliseconds(10), wake: {})
    }

    @Test func aControllerThatIsAlreadyConnectedIsMadeWhenTheRelayAnswers() async throws {
        let relay = Relay()
        let software = GCController.withExtendedGamepad()
        let controllers = make(relay, center: NotificationCenter(), controllers: { [software] in [software] })
        controllers.update(running: true, booted: false, foreground: true)
        #expect(await eventually { relay.connections == 1 })
        relay.send(.hello(version: 1, slotCount: 8))
        guard case .create(let create)? = await relay.next() else { Issue.record("no device was made"); return }
        // A software controller is a plain HID gamepad with the name iOS gives it.
        #expect(create.slot == 0)
        #expect(create.vendor == 0 && create.product == 0)
        #expect(create.name == "ExtendedGamepad")
        #expect(!create.descriptor.isEmpty)
        #expect(controllers.session.slot(ofController: 1) == 0)
        relay.send(.created(slot: 0, result: 0))
        relay.send(.event(slot: 0, event: .start))
        guard case .input(0, let report)? = await relay.next() else { Issue.record("no state was sent"); return }
        #expect(report.first == 1)
        controllers.update(running: false, booted: false, foreground: false)
        #expect(await relay.closedByHost())
    }

    @Test func aControllerThatConnectsAndDisconnectsIsMadeAndDestroyed() async throws {
        let relay = Relay()
        let center = NotificationCenter()
        let software = GCController.withExtendedGamepad()
        let controllers = make(relay, center: center, controllers: { [] })
        controllers.update(running: true, booted: false, foreground: true)
        #expect(await eventually { relay.connections == 1 })
        relay.send(.hello(version: 1, slotCount: 8))
        #expect(await eventually { controllers.session.isConnected })
        center.post(name: .GCControllerDidConnect, object: software)
        guard case .create? = await relay.next() else { Issue.record("no device was made"); return }
        center.post(name: .GCControllerDidDisconnect, object: software)
        #expect(await relay.next() == .destroy(slot: 0))
        #expect(controllers.table.isEmpty)
        controllers.update(running: false, booted: false, foreground: false)
    }

    @Test func nothingIsLookedForWhileNoRunGoesOn() async {
        let relay = Relay()
        let center = NotificationCenter()
        let software = GCController.withExtendedGamepad()
        let controllers = make(relay, center: center, controllers: { [software] in [software] })
        center.post(name: .GCControllerDidConnect, object: software)
        await settle()
        #expect(controllers.table.isEmpty)
        #expect(relay.connections == 0)
        controllers.update(running: true, booted: false, foreground: true)
        #expect(await eventually { !controllers.table.isEmpty })
        // The run ends: the controller is let go of, and a connect afterwards is not taken.
        controllers.update(running: false, booted: false, foreground: false)
        #expect(controllers.table.isEmpty)
        center.post(name: .GCControllerDidConnect, object: software)
        await settle()
        #expect(controllers.table.isEmpty)
    }

    @Test func aControllerWithNoExtendedGamepadIsLeftOut() async {
        let relay = Relay()
        let micro = GCController.withMicroGamepad()
        let controllers = make(relay, center: NotificationCenter(), controllers: { [micro] in [micro] })
        controllers.update(running: true, booted: false, foreground: true)
        await settle()
        #expect(controllers.table.isEmpty)
        controllers.update(running: false, booted: false, foreground: false)
    }

    /// The system's own virtual controller, which UIKit puts on the screen of
    /// the test host: the one controller a Simulator can connect, with iOS's
    /// real notifications and its real buffered input. The host may list
    /// other controllers too, so the test follows its own by identity.
    @Test func aVirtualControllerIsTakenAndLeftThroughIOSsOwnNotifications() async throws {
        let configuration = GCVirtualController.Configuration()
        configuration.elements = [GCInputLeftThumbstick, GCInputButtonA, GCInputButtonB]
        let virtual = GCVirtualController(configuration: configuration)
        try await virtual.connect()
        defer { virtual.disconnect() }
        let controller = try #require(virtual.controller)
        let relay = Relay()
        let controllers = GameControllers(
            player: Player(), engines: GameControllerEngines(), opener: relay, retryDelay: .milliseconds(10), wake: {})
        controllers.update(running: true, booted: false, foreground: true)
        #expect(await eventually { controllers.table.all.contains { $0.controller === controller } })
        let entry = try #require(controllers.table.all.first { $0.controller === controller })
        #expect(await eventually { relay.connections == 1 })
        relay.send(.hello(version: 1, slotCount: 8))
        // Every controller the host lists is made, in slot order; this one is found by its name.
        var slot: Int?
        while slot == nil, let frame = await relay.next() {
            if case .create(let create) = frame, create.name == controller.vendorName { slot = create.slot }
        }
        let own = try #require(slot)
        #expect(controllers.session.slot(ofController: entry.id) == own)
        relay.send(.created(slot: own, result: 0))
        relay.send(.event(slot: own, event: .start))
        var gotState = false
        while !gotState, let frame = await relay.next() {
            if case .input(own, _) = frame { gotState = true }
        }
        #expect(gotState)
        // The buffered input of a controller nobody touches is the neutral state.
        #expect(GamepadInputMapper.state(GamepadReading(state: controller.input.capture())) == .neutral)
        // The virtual controller goes away, and its device with it.
        virtual.disconnect()
        var destroyed = false
        while !destroyed, let frame = await relay.next() {
            if frame == .destroy(slot: own) { destroyed = true }
        }
        #expect(destroyed)
        controllers.update(running: false, booted: false, foreground: false)
    }

    @Test func theGuestsFeatureReportsAreAnsweredForTheControllersSlot() async throws {
        let relay = Relay()
        let software = GCController.withExtendedGamepad()
        let controllers = make(relay, center: NotificationCenter(), controllers: { [software] in [software] })
        controllers.update(running: true, booted: false, foreground: true)
        #expect(await eventually { relay.connections == 1 })
        relay.send(.hello(version: 1, slotCount: 8))
        _ = await relay.next()
        relay.send(.getReport(slot: 0, reportType: GamepadReportType.feature, number: 0x09, id: 4))
        // A plain HID gamepad has no feature report to give.
        #expect(await relay.next() == .getReportReply(slot: 0, id: 4, error: GamepadErrno.invalid, data: []))
        controllers.update(running: false, booted: false, foreground: false)
    }
}

/// Polls for `condition` on the main queue for at most `timeout` seconds,
/// letting the main run loop turn between looks.
@MainActor
private func eventually(_ timeout: TimeInterval = 3, _ condition: @MainActor () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if condition() { return true }
        try? await Task.sleep(for: .milliseconds(5))
    }
    return condition()
}

/// Lets the main queue run what is waiting on it.
@MainActor
private func settle() async {
    try? await Task.sleep(for: .milliseconds(100))
}
