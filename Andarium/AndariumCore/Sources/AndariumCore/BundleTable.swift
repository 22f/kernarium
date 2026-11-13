// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A reader of protocol buffer messages: the wire format only (varints and
/// length-delimited fields; fixed-width fields are skipped), with every length
/// checked against the bytes that remain. The input is hostile, so a field it
/// cannot make sense of ends the read with `BundleTable.Failure` and nothing in
/// it allocates by what a field says.
struct ProtoReader {
    enum Value {
        case varint(UInt64)
        case bytes(Data)
    }

    private let data: Data
    private var position: Int

    init(_ data: Data) {
        self.data = data
        position = data.startIndex
    }

    /// The next field of the message: its number and value; nil at the end.
    /// Fixed-width fields are read past and returned as zero.
    mutating func next() throws -> (number: Int, value: Value)? {
        guard position < data.endIndex else { return nil }
        let key = try varint()
        let number = Int(truncatingIfNeeded: key >> 3)
        guard number > 0, number < (1 << 29) else { throw BundleTable.Failure() }
        switch key & 7 {
        case 0:
            return (number, .varint(try varint()))
        case 1:
            try skip(8)
            return (number, .varint(0))
        case 2:
            let length = try varint()
            guard length <= UInt64(data.endIndex - position) else { throw BundleTable.Failure() }
            let end = position + Int(length)
            defer { position = end }
            return (number, .bytes(data.subdata(in: position..<end)))
        case 5:
            try skip(4)
            return (number, .varint(0))
        default:
            // Groups are gone from the format; the rest are not wire types.
            throw BundleTable.Failure()
        }
    }

    private mutating func skip(_ count: Int) throws {
        guard count <= data.endIndex - position else { throw BundleTable.Failure() }
        position += count
    }

    private mutating func varint() throws -> UInt64 {
        var result: UInt64 = 0
        for shift in stride(from: 0, to: 70, by: 7) {
            guard position < data.endIndex else { throw BundleTable.Failure() }
            let byte = data[position]
            position += 1
            if shift == 63, byte > 1 { throw BundleTable.Failure() }
            result |= UInt64(byte & 0x7f) << UInt64(shift)
            if byte & 0x80 == 0 { return result }
        }
        throw BundleTable.Failure()
    }
}

/// The table of contents of a bundletool `.apks` (`toc.pb`, a `BuildApksResult`
/// message): the forms of the app the bundle holds (variants), for the version
/// of Android they are for, and the APKs of each. Only what the choice of one
/// variant needs is read; the rest of the message is skipped.
struct BundleTable: Equatable, Sendable {
    /// The table cannot be read, or holds more than a bundle does.
    struct Failure: Error, Equatable {}

    /// The limits of a table: the message itself is bounded by the caller.
    static let maxVariants = 64
    static let maxDepth = 8

    struct Apk: Equatable, Sendable {
        var path: String
        /// The split's id (`config.arm64_v8a`); empty for a master split.
        var splitID: String
        var isMaster: Bool
        /// The module the APK is of (`base` for the app itself).
        var module: String
        /// A standalone APK holds the whole app (`universal.apk`).
        var isStandalone: Bool
        /// The processor the APK is the code of, in Android's spelling.
        var abi: String?
    }

    struct Variant: Equatable, Sendable {
        var number: Int
        /// The lowest API level the variant is for (1 with no restriction).
        var minSdk: Int
        /// The processors the variant itself is for (a standalone build of
        /// one processor); empty for no restriction.
        var abis: [String]
        /// The variant is targeted at something this reader does not weigh
        /// (a screen density, a texture format, a runtime): not chosen.
        var otherTargeting: Bool
        var apks: [Apk]
    }

    var variants: [Variant]

    /// `AbiAlias` of bundletool's targeting messages, in Android's spelling.
    private static let abiNames: [Int: String] = [
        1: "armeabi", 2: "armeabi-v7a", 3: "arm64-v8a", 4: "x86", 5: "x86_64", 6: "mips", 7: "mips64",
    ]

    init(_ data: Data) throws {
        var variants: [Variant] = []
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard number == 1, case let .bytes(bytes) = value else { continue }
            guard variants.count < Self.maxVariants else { throw Failure() }
            variants.append(try Self.variant(bytes, index: variants.count))
        }
        self.variants = variants
    }

    // MARK: Messages

    private static func variant(_ data: Data, index: Int) throws -> Variant {
        var result = Variant(number: index, minSdk: 1, abis: [], otherTargeting: false, apks: [])
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            switch (number, value) {
            case let (1, .bytes(bytes)):
                try variantTargeting(bytes, into: &result)
            case let (2, .bytes(bytes)):
                try apkSet(bytes, into: &result)
            case let (3, .varint(raw)):
                result.number = Int(truncatingIfNeeded: min(raw, UInt64(Int32.max)))
            default:
                continue
            }
        }
        return result
    }

    /// `VariantTargeting`: the SDK (field 1) and the processor (field 2) are
    /// weighed; any other field makes the variant one for something else.
    private static func variantTargeting(_ data: Data, into variant: inout Variant) throws {
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard case let .bytes(bytes) = value else { continue }
            switch number {
            case 1: variant.minSdk = max(1, try sdkMinimum(bytes) ?? 1)
            case 2: variant.abis = try abiValues(bytes)
            default: if !bytes.isEmpty { variant.otherTargeting = true }
            }
        }
    }

    /// `SdkVersionTargeting.value[0].min.value`.
    private static func sdkMinimum(_ data: Data) throws -> Int? {
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard number == 1, case let .bytes(version) = value else { continue }
            var inner = ProtoReader(version)
            while let (field, boxed) = try inner.next() {
                guard field == 1, case let .bytes(wrapper) = boxed else { continue }
                var wrapped = ProtoReader(wrapper)
                while let (leaf, number) = try wrapped.next() {
                    if leaf == 1, case let .varint(raw) = number { return Int(truncatingIfNeeded: min(raw, UInt64(Int32.max))) }
                }
                return 0
            }
            return 0
        }
        return nil
    }

    /// `AbiTargeting.value[].alias`.
    private static func abiValues(_ data: Data) throws -> [String] {
        var names: [String] = []
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard number == 1, case let .bytes(abi) = value else { continue }
            var inner = ProtoReader(abi)
            while let (field, alias) = try inner.next() {
                if field == 1, case let .varint(raw) = alias, let name = abiNames[Int(truncatingIfNeeded: min(raw, 1000))] { names.append(name) }
            }
        }
        return names
    }

    /// `ApkSet`: a module's metadata (field 1, the name its field 1) and its APKs (field 2).
    private static func apkSet(_ data: Data, into variant: inout Variant) throws {
        var module = ""
        var descriptions: [Data] = []
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard case let .bytes(bytes) = value else { continue }
            if number == 1 {
                var inner = ProtoReader(bytes)
                while let (field, name) = try inner.next() {
                    if field == 1, case let .bytes(text) = name { module = String(decoding: text, as: UTF8.self) }
                }
            } else if number == 2 {
                guard variant.apks.count + descriptions.count < ApkContainer.maxParts else { throw Failure() }
                descriptions.append(bytes)
            }
        }
        for description in descriptions {
            if let apk = try apkDescription(description, module: module) { variant.apks.append(apk) }
        }
    }

    /// `ApkDescription`: the targeting (field 1: the processor is its field 1),
    /// the path (2), and either split metadata (3: the split's id, and
    /// whether it is the module's master) or standalone metadata (4). An APK
    /// of any other kind (instant, system) is not installed here.
    private static func apkDescription(_ data: Data, module: String) throws -> Apk? {
        var path: String?
        var abi: String?
        var split: (id: String, master: Bool)?
        var standalone = false
        var reader = ProtoReader(data)
        while let (number, value) = try reader.next() {
            guard case let .bytes(bytes) = value else { continue }
            switch number {
            case 1:
                var targeting = ProtoReader(bytes)
                while let (field, abis) = try targeting.next() {
                    if field == 1, case let .bytes(inner) = abis { abi = try abiValues(inner).first }
                }
            case 2:
                guard let text = String(data: bytes, encoding: .utf8) else { throw Failure() }
                path = text
            case 3:
                var id = ""
                var master = false
                var inner = ProtoReader(bytes)
                while let (field, item) = try inner.next() {
                    if field == 1, case let .bytes(text) = item { id = String(decoding: text, as: UTF8.self) }
                    if field == 2, case let .varint(flag) = item { master = flag != 0 }
                }
                split = (id, master)
            case 4:
                standalone = true
            default:
                continue
            }
        }
        guard let path else { throw Failure() }
        guard split != nil || standalone else { return nil }
        return Apk(
            path: path, splitID: split?.id ?? "", isMaster: split?.master ?? false, module: module, isStandalone: standalone,
            abi: standalone ? nil : abi)
    }
}
