// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// A bundletool `.apks` is read through its table of contents: the variants of
/// the app it holds, one of which is installed for the guest's Android, and
/// nothing is guessed from the names of the APKs.
struct BundleTableTests {
    private typealias Apk = BundleTocFixture.Apk
    private typealias Variant = BundleTocFixture.Variant

    private func entry(_ name: String, size: Int = 64) -> ZipFixture.Item {
        var data = Data("PK-apk:\(name):".utf8)
        data.append(Data(repeating: 0x41, count: max(0, size - data.count)))
        return .init(name, data, method: 0)
    }

    private func archive(_ toc: BundleTocFixture, extra: [String] = []) -> [ZipFixture.Item] {
        var paths: [String] = []
        for variant in toc.variants {
            for apk in variant.apks where !paths.contains(apk.path) { paths.append(apk.path) }
        }
        return [.init("toc.pb", toc.build(), method: 8)] + (paths + extra).map { entry($0) }
    }

    private func plan(_ items: [ZipFixture.Item]) throws -> ApkContainerPlan {
        guard case let .parts(plan) = try ApkContainer.inspect(ZipDataSource(ZipFixture(items).build()), fileName: "app.apks") else {
            Issue.record("not an archive of parts")
            throw InstallRefusal.damaged
        }
        return plan
    }

    private func refusal(_ items: [ZipFixture.Item]) -> InstallRefusal? {
        do {
            _ = try ApkContainer.inspect(ZipDataSource(ZipFixture(items).build()), fileName: "app.apks")
            return nil
        } catch let refusal as InstallRefusal {
            return refusal
        } catch {
            Issue.record("\(error)")
            return nil
        }
    }

    /// The bundle of a real build: the same splits for every variant but the
    /// master and the processor splits, which two of the variants share.
    private func release() -> BundleTocFixture {
        let shared: [Apk] = [
            .split("splits/base-en.apk", "config.en"), .split("splits/base-ja.apk", "config.ja"),
            .split("splits/base-xxhdpi.apk", "config.xxhdpi"), .split("splits/base-hdpi.apk", "config.hdpi"),
        ]
        func abis(_ suffix: String) -> [Apk] {
            [
                .split("splits/base-arm64_v8a\(suffix).apk", "config.arm64_v8a", abi: 3),
                .split("splits/base-armeabi_v7a\(suffix).apk", "config.armeabi_v7a", abi: 2),
                .split("splits/base-x86\(suffix).apk", "config.x86", abi: 4),
                .split("splits/base-x86_64\(suffix).apk", "config.x86_64", abi: 5),
            ]
        }
        return BundleTocFixture(variants: [
            Variant(minSdk: 21, apks: [.master("splits/base-master.apk")] + abis("") + shared),
            Variant(minSdk: 23, apks: [.master("splits/base-master_2.apk")] + abis("_2") + shared),
            Variant(minSdk: 31, apks: [.master("splits/base-master_3.apk")] + abis("_2") + shared),
        ])
    }

    private func entries(_ plan: ApkContainerPlan) -> Set<String> { Set(plan.candidates.map(\.entry)) }

    // MARK: One variant

    @Test func theVariantForTheGuestsAndroidIsTheOneInstalled() throws {
        let found = try plan(archive(release()))
        #expect(found.kind == .bundletool && found.variants.count == 3)
        let arm = ["arm64-v8a", "armeabi-v7a", "armeabi"]
        // Android 17 (API 37): the newest variant, with its own master and the processor splits it shares with the middle one.
        let newest = try found.selecting(guestAbis: arm, guestSdk: 37)
        #expect(entries(newest) == [
            "splits/base-master_3.apk", "splits/base-arm64_v8a_2.apk", "splits/base-en.apk", "splits/base-ja.apk",
            "splits/base-xxhdpi.apk", "splits/base-hdpi.apk",
        ])
        #expect(newest.candidates.first?.entry == "splits/base-master_3.apk" && newest.candidates.first?.name == "base.apk")
        #expect(newest.candidates.first?.splitName == nil)
        #expect(newest.variants.isEmpty)
        let middle = try found.selecting(guestAbis: arm, guestSdk: 30)
        #expect(middle.candidates.first?.entry == "splits/base-master_2.apk" && entries(middle).contains("splits/base-arm64_v8a_2.apk"))
        let oldest = try found.selecting(guestAbis: arm, guestSdk: 22)
        #expect(oldest.candidates.first?.entry == "splits/base-master.apk" && entries(oldest).contains("splits/base-arm64_v8a.apk"))
        #expect(!entries(oldest).contains("splits/base-arm64_v8a_2.apk"))
        // The exact levels a variant starts at.
        #expect(try found.selecting(guestAbis: arm, guestSdk: 31).candidates.first?.entry == "splits/base-master_3.apk")
        #expect(try found.selecting(guestAbis: arm, guestSdk: 23).candidates.first?.entry == "splits/base-master_2.apk")
        #expect(try found.selecting(guestAbis: arm, guestSdk: 21).candidates.first?.entry == "splits/base-master.apk")
        // A guest that cannot say its level gets the newest variant.
        #expect(try found.selecting(guestAbis: arm).candidates.first?.entry == "splits/base-master_3.apk")
        // No variant is for an Android older than the oldest one.
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: arm, guestSdk: 20) }
    }

    @Test func noPartOfAnotherVariantIsEverSentBesideTheChosenOne() throws {
        let found = try plan(archive(release()))
        for level in [21, 22, 23, 29, 31, 37, 60] {
            let chosen = try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: level)
            // One base, one split for each processor at most, no split twice.
            #expect(chosen.candidates.filter { $0.splitName == nil }.count == 1, "api \(level)")
            let ids = chosen.candidates.compactMap(\.splitName)
            #expect(Set(ids).count == ids.count, "api \(level)")
            #expect(chosen.candidates.compactMap(\.abi) == ["arm64-v8a"], "api \(level)")
            #expect(Set(chosen.candidates.map(\.name)).count == chosen.candidates.count, "api \(level)")
        }
    }

    @Test func aBundleIsReadFromItsTableAndNotFromTheNamesOfItsApks() throws {
        // `-master_2` is not a master here: the table says which APK is.
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 21, apks: [.master("splits/something.apk"), .split("splits/base-master_2.apk", "config.en")]),
        ])
        let chosen = try plan(archive(toc)).selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)
        #expect(chosen.candidates.first?.entry == "splits/something.apk")
        #expect(chosen.candidates.map(\.splitName) == [nil, "config.en"])
        // An APK the table does not name is not installed, whatever it is called.
        let more = try plan(archive(toc, extra: ["splits/base-xxhdpi.apk", "standalones/standalone.apk", "universal.apk"]))
        #expect(entries(try more.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)) == ["splits/something.apk", "splits/base-master_2.apk"])
    }

    @Test func aFeatureModuleIsInstalledWithTheBase() throws {
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 21, apks: [
                .master("splits/base-master.apk"), .split("splits/base-en.apk", "config.en"),
                .master("splits/feature-master.apk", module: "feature"),
                .split("splits/feature-arm64_v8a.apk", "feature.config.arm64_v8a", abi: 3, module: "feature"),
                .split("splits/feature-x86.apk", "feature.config.x86", abi: 4, module: "feature"),
            ]),
        ])
        let chosen = try plan(archive(toc)).selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)
        #expect(entries(chosen) == ["splits/base-master.apk", "splits/base-en.apk", "splits/feature-master.apk", "splits/feature-arm64_v8a.apk"])
        #expect(chosen.candidates.first?.entry == "splits/base-master.apk")
        // Its master has the module's name for an id, and is not a second base.
        #expect(chosen.candidates.filter { $0.splitName == nil }.count == 1)
    }

    // MARK: Universal and standalone

    @Test func aUniversalBundleInstallsAsOneApk() throws {
        let toc = BundleTocFixture(variants: [Variant(minSdk: nil, apks: [.standalone("universal.apk")])])
        let found = try plan(archive(toc))
        let chosen = try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37)
        #expect(chosen.candidates.map(\.entry) == ["universal.apk"] && chosen.candidates.first?.name == "base.apk")
        #expect(chosen.candidates.first?.splitName == nil)
        #expect(chosen.data.isEmpty)
    }

    @Test func standaloneBuildsForOneProcessorEachAreChosenByTheGuestsProcessors() throws {
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 1, abis: [4], apks: [.standalone("standalones/standalone-x86.apk")]),
            Variant(minSdk: 1, abis: [2], apks: [.standalone("standalones/standalone-armeabi_v7a.apk")]),
            Variant(minSdk: 1, abis: [3], apks: [.standalone("standalones/standalone-arm64_v8a.apk")]),
        ])
        let found = try plan(archive(toc))
        #expect(try found.selecting(guestAbis: ["arm64-v8a", "armeabi-v7a"], guestSdk: 37).candidates.map(\.entry) == ["standalones/standalone-arm64_v8a.apk"])
        #expect(try found.selecting(guestAbis: ["armeabi-v7a"], guestSdk: 37).candidates.map(\.entry) == ["standalones/standalone-armeabi_v7a.apk"])
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: ["riscv64"], guestSdk: 37) }
    }

    @Test func theSplitsOfARecentAndroidBeatTheStandalonesOfAnOldOne() throws {
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 1, apks: [.standalone("standalones/standalone.apk")]),
            Variant(minSdk: 21, apks: [.master("splits/base-master.apk"), .split("splits/base-en.apk", "config.en")]),
        ])
        let found = try plan(archive(toc))
        #expect(try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37).candidates.first?.entry == "splits/base-master.apk")
        #expect(try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 19).candidates.map(\.entry) == ["standalones/standalone.apk"])
    }

    @Test func aVariantForSomethingElseIsNotChosen() throws {
        let toc = BundleTocFixture(variants: [
            Variant(minSdk: 21, density: true, apks: [.standalone("standalones/standalone-xxhdpi.apk")]),
        ])
        let found = try plan(archive(toc))
        #expect(throws: InstallRefusal.damaged) { _ = try found.selecting(guestAbis: ApkContainer.defaultGuestAbis, guestSdk: 37) }
    }

    // MARK: A table that cannot be trusted

    @Test func aTableThatIsNotOneIsDamagedNotGuessedFrom() {
        let names = ["splits/base-master.apk", "splits/base-en.apk"]
        for bytes in [
            Data(), Data("toc".utf8), Data(repeating: 0xff, count: 40), Data([0x0a, 0x7f]), Data([0x0a]), Data([0x08]),
            Data(repeating: 0x80, count: 12), Data([0x0b, 0x01]), Data([0x0c]), Data([0x0e, 0x00]), Data([0x00, 0x01]),
        ] {
            let items: [ZipFixture.Item] = [.init("toc.pb", bytes, method: 8)] + names.map { entry($0) }
            #expect(refusal(items) == .damaged, "\(Array(bytes))")
        }
    }

    @Test func everyCutOfATableIsDamagedOrAWholeTable() {
        let toc = release().build()
        var cut = 0
        while cut < toc.count {
            let items: [ZipFixture.Item] = [.init("toc.pb", toc.prefix(cut), method: 8)] + archive(release()).dropFirst()
            let result = refusal(items)
            #expect(result == nil || result == .damaged, "cut at \(cut)")
            cut += 7
        }
    }

    @Test func aTableThatNamesWhatTheFileDoesNotHoldOrContradictsItselfIsDamaged() {
        // An APK the file does not hold.
        let missing = BundleTocFixture(variants: [Variant(apks: [.master("splits/base-master.apk"), .split("splits/gone.apk", "config.en")])])
        #expect(refusal(archive(missing).filter { $0.name != "splits/gone.apk" }) == .damaged)
        // A directory is not an APK.
        let directory = BundleTocFixture(variants: [Variant(apks: [.master("splits/")])])
        #expect(refusal([.init("toc.pb", directory.build()), .init("splits/", Data(), method: 0), entry("other.apk")]) == .damaged)
        // No master, two masters, no variant, a split twice.
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: [.split("splits/base-en.apk", "config.en")])]))) == .damaged)
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: [.master("splits/a.apk"), .master("splits/b.apk")])]))) == .damaged)
        #expect(refusal(archive(BundleTocFixture(variants: []), extra: ["splits/base-master.apk"])) == .damaged)
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: [
            .master("splits/base-master.apk"), .split("splits/a.apk", "config.en"), .split("splits/b.apk", "config.en"),
        ])]))) == .damaged)
        // A master of a feature module is not the app's base.
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: [.master("splits/f.apk", module: "feature")])]))) == .damaged)
        // A standalone APK with splits beside it.
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: [.standalone("universal.apk"), .split("splits/a.apk", "config.en")])]))) == .damaged)
    }

    @Test func aTableOverTheCapOrOfTooManyPartsIsDamaged() {
        let big = Data(repeating: 0, count: ApkContainer.maxManifestBytes + 1)
        #expect(refusal([.init("toc.pb", big, method: 8), entry("splits/base-master.apk")]) == .damaged)
        var apks: [Apk] = [.master("splits/base-master.apk")]
        for index in 0..<ApkContainer.maxParts { apks.append(.split("splits/l\(index).apk", "config.l\(index)")) }
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: apks)]))) == .damaged)
        apks.removeLast()
        #expect(refusal(archive(BundleTocFixture(variants: [Variant(apks: apks)]))) == nil)
        // More variants than a bundle has.
        let variants = (0..<(BundleTable.maxVariants + 1)).map { Variant(minSdk: $0 + 1, apks: [.master("splits/base-master.apk")]) }
        #expect(refusal(archive(BundleTocFixture(variants: variants))) == .damaged)
    }

    @Test func theWireFormatIsReadWithItsLengthsChecked() throws {
        // A varint of eleven bytes, a length past the end, a group, a field number of zero.
        for bytes in [
            Data(repeating: 0x80, count: 10) + Data([0x01]), Data([0x0a, 0x05, 0x01]), Data([0x0b]), Data([0x04]), Data([0x00, 0x00]),
            Data([0x0f]), Data([0x0e]),
        ] {
            #expect(throws: BundleTable.Failure.self, "\(Array(bytes))") { _ = try BundleTable(bytes) }
        }
        // Fields it does not read are skipped, fixed-width ones included.
        let fixed32 = Data([0x1d, 1, 2, 3, 4])
        let fixed64 = Data([0x21, 1, 2, 3, 4, 5, 6, 7, 8])
        let skipped = try BundleTable(fixed32 + fixed64 + BundleTocFixture.string(7, "x") + BundleTocFixture.int(9, 5))
        #expect(skipped.variants.isEmpty)
        // A whole table reads.
        let table = try BundleTable(release().build())
        #expect(table.variants.map(\.minSdk) == [21, 23, 31])
        #expect(table.variants[2].apks.first?.isMaster == true && table.variants[2].apks.first?.module == "base")
        #expect(table.variants[2].apks.contains { $0.path == "splits/base-arm64_v8a_2.apk" && $0.abi == "arm64-v8a" })
    }
}
