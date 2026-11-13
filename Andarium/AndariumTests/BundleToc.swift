// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Writes the table of contents of a bundletool `.apks` (`toc.pb`) for the hosted tests of Install APK: the
/// messages `BundleTable` reads, as protocol buffers, so that a test can build
/// the variants a real bundle has (one APK shared by several, a master split
/// for each) and the damage a hostile one has.
nonisolated struct BundleToc {
    struct Apk {
        var path: String
        /// Empty for a master split.
        var splitID = ""
        var master = false
        var module = "base"
        /// `AbiAlias` of bundletool: 1 armeabi, 2 armeabi-v7a, 3 arm64-v8a, 4 x86, 5 x86_64.
        var abi: Int?
        var standalone = false

        static func master(_ path: String, module: String = "base") -> Apk {
            Apk(path: path, splitID: module == "base" ? "" : module, master: true, module: module)
        }

        static func split(_ path: String, _ id: String, abi: Int? = nil, module: String = "base") -> Apk {
            Apk(path: path, splitID: id, master: false, module: module, abi: abi)
        }

        static func standalone(_ path: String) -> Apk {
            Apk(path: path, standalone: true)
        }
    }

    struct Variant {
        var minSdk: Int? = 21
        var abis: [Int] = []
        /// A field of the variant's targeting this reader does not weigh (a screen density).
        var density = false
        var number: Int?
        var apks: [Apk]
    }

    /// An asset module (an asset pack) and its slices: `AssetSliceSet` of bundletool's
    /// `BuildApksResult`.
    struct AssetModule {
        var name: String
        /// `DeliveryType`: 1 install-time, 2 on-demand, 3 fast-follow; nil for none stated.
        var delivery: Int?
        var slices: [AssetSlice]

        static func installTime(_ name: String, _ slices: [AssetSlice]) -> AssetModule {
            AssetModule(name: name, delivery: 1, slices: slices)
        }

        static func onDemand(_ name: String, _ slices: [AssetSlice]) -> AssetModule {
            AssetModule(name: name, delivery: 2, slices: slices)
        }

        static func fastFollow(_ name: String, _ slices: [AssetSlice]) -> AssetModule {
            AssetModule(name: name, delivery: 3, slices: slices)
        }
    }

    struct AssetSlice {
        var path: String
        var splitID: String
        var master = false
        var abi: Int?
        /// The lowest API level, as `ApkTargeting.sdk_version_targeting` states it.
        var minSdk: Int?
        /// False for a description that carries no asset slice metadata.
        var metadata = true

        static func slice(_ path: String, _ id: String, master: Bool = false, abi: Int? = nil, minSdk: Int? = nil) -> AssetSlice {
            AssetSlice(path: path, splitID: id, master: master, abi: abi, minSdk: minSdk)
        }
    }

    var variants: [Variant]
    var assetModules: [AssetModule] = []
    var packageName: String? = "com.example.bundle"

    // MARK: Protocol buffers

    static func varint(_ value: UInt64) -> Data {
        var data = Data()
        var rest = value
        while rest >= 0x80 {
            data.append(UInt8(rest & 0x7f) | 0x80)
            rest >>= 7
        }
        data.append(UInt8(rest))
        return data
    }

    static func message(_ field: Int, _ body: Data) -> Data {
        varint(UInt64(field << 3 | 2)) + varint(UInt64(body.count)) + body
    }

    static func string(_ field: Int, _ text: String) -> Data { message(field, Data(text.utf8)) }

    static func int(_ field: Int, _ value: Int) -> Data { varint(UInt64(field << 3)) + varint(UInt64(value)) }

    private static func abiTargeting(_ aliases: [Int]) -> Data {
        aliases.reduce(Data()) { $0 + message(1, int(1, $1)) }
    }

    private static func description(_ apk: Apk) -> Data {
        var body = Data()
        if let abi = apk.abi { body += message(1, message(1, abiTargeting([abi]))) }
        body += string(2, apk.path)
        if apk.standalone {
            body += message(4, Data())
        } else {
            var split = Data()
            if !apk.splitID.isEmpty { split += string(1, apk.splitID) }
            if apk.master { split += int(2, 1) }
            body += message(3, split)
        }
        return message(2, body)
    }

    private static func assetSet(_ module: AssetModule) -> Data {
        var metadata = string(1, module.name)
        if let delivery = module.delivery { metadata += int(4, delivery) }
        var body = message(1, metadata)
        for slice in module.slices {
            var description = Data()
            var targeting = Data()
            if let abi = slice.abi { targeting += message(1, abiTargeting([abi])) }
            if let sdk = slice.minSdk { targeting += message(5, message(1, message(1, int(1, sdk)))) }
            if !targeting.isEmpty { description += message(1, targeting) }
            description += string(2, slice.path)
            if slice.metadata {
                var split = Data()
                if !slice.splitID.isEmpty { split += string(1, slice.splitID) }
                if slice.master { split += int(2, 1) }
                description += message(7, split)
            }
            body += message(2, description)
        }
        return message(3, body)
    }

    private static func variant(_ variant: Variant, index: Int) -> Data {
        var targeting = Data()
        if let sdk = variant.minSdk { targeting += message(1, message(1, message(1, int(1, sdk)))) }
        if !variant.abis.isEmpty { targeting += message(2, abiTargeting(variant.abis)) }
        if variant.density { targeting += message(3, message(1, message(1, int(1, 6)))) }
        var body = message(1, targeting)
        // One ApkSet for each module, in the order the modules first appear.
        var order: [String] = []
        for apk in variant.apks where !order.contains(apk.module) { order.append(apk.module) }
        for module in order {
            var set = message(1, string(1, module))
            for apk in variant.apks where apk.module == module { set += description(apk) }
            body += message(2, set)
        }
        if let number = variant.number { body += int(3, number) }
        return message(1, body)
    }

    func build() -> Data {
        var data = Data()
        for (index, entry) in variants.enumerated() { data += Self.variant(entry, index: index) }
        for module in assetModules { data += Self.assetSet(module) }
        data += Self.message(2, Self.string(2, "1.17.1"))
        if let packageName { data += Self.string(4, packageName) }
        return data
    }
}
