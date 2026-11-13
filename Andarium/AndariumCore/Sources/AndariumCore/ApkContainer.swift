// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What Install APK does with a file that is an archive of an app's parts
/// (.apks, .xapk, .apkm): which entries are the app, and which of them this
/// virtual machine installs. The archive is only read in the central
/// directory and, for a manifest, one small entry. The APKs inside are opaque
/// byte streams: they are neither opened nor parsed, an archive inside an
/// archive is not followed, and the choice is made on the names the
/// containers use and on the container's own manifest, never on the file's
/// extension.
public enum ApkContainer {
    /// What a file turned out to be.
    public enum Inspection: Equatable, Sendable {
        /// An ordinary APK under another name; it installs like any APK.
        case singleApk
        /// An app in parts, and perhaps its data.
        case parts(ApkContainerPlan)
    }

    /// The most parts one install writes, and the largest manifest read.
    public static let maxParts = 512
    public static let maxManifestBytes = 1 << 20

    /// The processors of the virtual machine, in the order its Android prefers
    /// them (`ro.product.cpu.abilist`).
    public static let defaultGuestAbis = ["arm64-v8a", "armeabi-v7a", "armeabi"]

    /// Reads `archive` and decides what it is.
    public static func inspect(_ archive: ZipArchive) throws -> Inspection {
        do {
            return try classify(archive)
        } catch let refusal as InstallRefusal {
            throw refusal
        } catch let error as ZipError {
            throw refusal(for: error)
        } catch is BundleTable.Failure {
            throw InstallRefusal.damaged
        }
    }

    /// Opens `source` as an archive. A file that is not one is worded by what
    /// it is: a file that starts like a ZIP and stops short is a download cut
    /// off, and any other file named as an app in parts is damaged, but for an
    /// `.apkm`, which may be the encrypted kind of that file, that does not
    /// start like a ZIP at all. `checkpoint` is asked while the directory is read.
    public static func open(_ source: any ZipSource, fileName: String, checkpoint: ZipCheckpoint? = nil) throws -> ZipArchive {
        do {
            return try ZipArchive(source: source, checkpoint: checkpoint)
        } catch ZipError.notAnArchive {
            throw notAnArchive(source, fileName: fileName)
        } catch let error as ZipError {
            throw refusal(for: error)
        }
    }

    /// Opens `source` as an archive and inspects it.
    public static func inspect(_ source: any ZipSource, fileName: String, checkpoint: ZipCheckpoint? = nil) throws -> Inspection {
        try inspect(open(source, fileName: fileName, checkpoint: checkpoint))
    }

    private static func notAnArchive(_ source: any ZipSource, fileName: String) -> InstallRefusal {
        var start = [UInt8](repeating: 0, count: 2)
        let got = (try? start.withUnsafeMutableBytes { try source.read(at: 0, into: $0) }) ?? 0
        if got == 2, start == [0x50, 0x4B] { return .damaged }
        let name = fileName.lowercased()
        if name.hasSuffix(".apkm") { return .damagedOrEncrypted }
        return name.hasSuffix(".apks") || name.hasSuffix(".xapk") ? .damaged : .notApk
    }

    /// The words an archive's failure gets.
    public static func refusal(for error: ZipError) -> InstallRefusal {
        switch error {
        case .encrypted: .encrypted
        case .notAnArchive, .unsupported, .malformed, .tooLarge, .corrupt: .damaged
        }
    }

    // MARK: Classifying

    private enum Layout {
        case bundletool, sai, xapk, apkm, generic
    }

    private static func classify(_ archive: ZipArchive) throws -> Inspection {
        if archive.entries.contains(where: \.isEncrypted) { throw InstallRefusal.encrypted }
        let live = archive.entries.filter { !$0.isDirectory && !isJunk($0.name) }
        let names = Set(live.map(\.name))
        // An app bundle, the form an app is uploaded to a store in.
        if names.contains("BundleConfig.pb") || names.contains("base/manifest/AndroidManifest.xml") {
            throw InstallRefusal.appBundle
        }
        // An APK itself: its manifest is at the root. None of the containers
        // has one there, so a file that does is the APK it looks like, whatever
        // it is named and whatever APKs it holds as assets.
        if names.contains("AndroidManifest.xml") { return .singleApk }
        let apks = live.filter { $0.name.lowercased().hasSuffix(".apk") }
        let layout: Layout =
            names.contains("toc.pb") ? .bundletool
            : names.contains("manifest.json") ? .xapk
            : names.contains("meta.sai_v2.json") || names.contains("meta.sai_v1.json") ? .sai
            : names.contains("info.json") ? .apkm : .generic
        if apks.isEmpty { throw InstallRefusal.noApp }
        switch layout {
        case .bundletool: return .parts(try bundletool(archive: archive))
        case .xapk: return .parts(try xapk(archive, apks: apks, live: live))
        case .sai: return .parts(try flat(apks, kind: .sai))
        case .apkm: return .parts(try apkm(archive, apks: apks, live: live))
        case .generic: return .parts(try flat(apks, kind: .archive))
        }
    }

    /// Entries a file manager or an archiver adds, never an app's parts.
    private static func isJunk(_ name: String) -> Bool {
        name.hasPrefix("__MACOSX/") || basename(name).hasPrefix("._") || basename(name) == ".DS_Store"
    }

    static func basename(_ name: String) -> String {
        name.split(separator: "/").last.map(String.init) ?? name
    }

    private static func stem(_ name: String) -> String {
        let base = basename(name)
        return base.lowercased().hasSuffix(".apk") ? String(base.dropLast(4)) : base
    }

    // MARK: bundletool's .apks

    /// The table of contents (`toc.pb`) names the variants of the app, each
    /// with the APKs it is made of; the variant for the virtual machine is
    /// chosen when its Android is known (`ApkContainerPlan.selecting`), and
    /// only that variant's APKs are installed, never the standalones or the
    /// splits of another variant beside them. A table that cannot be read, or
    /// names an APK the file does not hold, is a damaged file: no variant is
    /// guessed from the names.
    private static func bundletool(archive: ZipArchive) throws -> ApkContainerPlan {
        guard let tocEntry = archive.entry(named: "toc.pb"), !tocEntry.isDirectory else { throw InstallRefusal.damaged }
        guard tocEntry.size <= UInt64(maxManifestBytes) else { throw InstallRefusal.damaged }
        let table = try BundleTable(archive.data(of: tocEntry, limit: maxManifestBytes))
        guard !table.variants.isEmpty else { throw InstallRefusal.damaged }
        var parts: [ApkContainerPlan.Part] = []
        var known = Set<String>()
        var variants: [ApkContainerPlan.Variant] = []
        for variant in table.variants {
            guard variant.apks.count <= maxParts else { throw InstallRefusal.damaged }
            // The base is the one APK that is the app itself: the base module's
            // master split, or the whole app of a standalone build.
            let bases = variant.apks.filter { ($0.isMaster && $0.module == "base") || $0.isStandalone }
            guard bases.count == 1, let base = bases.first else {
                // A variant that is not made of the base and its splits: not for this install.
                if variant.apks.isEmpty { continue }
                throw InstallRefusal.damaged
            }
            if base.isStandalone, variant.apks.count != 1 { throw InstallRefusal.damaged }
            var entries: [String] = []
            var ids = Set<String>()
            for apk in variant.apks {
                guard let entry = archive.entry(named: apk.path), !entry.isDirectory, entries.count < maxParts else { throw InstallRefusal.damaged }
                // A split is installed once: the same id twice in a variant is a table that contradicts itself.
                if apk.path != base.path {
                    guard ids.insert(apk.splitID.isEmpty ? apk.path : apk.splitID).inserted else { throw InstallRefusal.damaged }
                }
                entries.append(entry.name)
                if known.insert(entry.name).inserted {
                    parts.append(ApkContainerPlan.Part(
                        entry: entry.name, bytes: entry.size, name: "", splitName: apk.path == base.path ? nil : apk.splitID,
                        abi: apk.path == base.path ? nil : apk.abi))
                }
            }
            variants.append(ApkContainerPlan.Variant(
                number: variant.number, minSdk: variant.minSdk, abis: variant.abis, otherTargeting: variant.otherTargeting,
                base: base.path, entries: entries))
        }
        guard !variants.isEmpty, parts.count <= 4096 else { throw InstallRefusal.damaged }
        return ApkContainerPlan(kind: .bundletool, packageName: nil, candidates: parts, data: [], unplacedData: 0, variants: variants)
    }

    // MARK: Archives with the APKs side by side

    /// SAI's .apks, an .apkm and any other archive of an app's files: the
    /// base is `base.apk`, or the only APK that is not named like a split.
    private static func flat(_ apks: [ZipEntry], kind: ApkContainerPlan.Kind) throws -> ApkContainerPlan {
        // The files of one folder: the root's, or the only folder's.
        let folders = Dictionary(grouping: apks, by: { String($0.name.dropLast(basename($0.name).count)) })
        let group: [ZipEntry]
        if let root = folders[""] {
            group = root
        } else if folders.count == 1, let only = folders.values.first {
            group = only
        } else {
            throw InstallRefusal.damaged
        }
        let base: ZipEntry
        if let named = group.first(where: { basename($0.name).lowercased() == "base.apk" }) {
            base = named
        } else {
            let plain = group.filter { !looksLikeSplit(stem($0.name)) }
            guard plain.count == 1, let only = plain.first else { throw InstallRefusal.damaged }
            base = only
        }
        // The base has no processor of its own, whatever its file is called.
        let parts = group.map { entry -> ApkContainerPlan.Part in
            let id = splitID(fromFileStem: stem(entry.name))
            let isBase = entry.name == base.name
            return ApkContainerPlan.Part(
                entry: entry.name, bytes: entry.size, name: "", splitName: isBase ? nil : id, abi: isBase ? nil : abi(ofConfig: id))
        }
        return try finish(ApkContainerPlan(kind: kind, packageName: nil, candidates: parts, data: [], unplacedData: 0), base: base.name)
    }

    private static func looksLikeSplit(_ stem: String) -> Bool {
        let text = stem.lowercased()
        return text.hasPrefix("split") || text.hasPrefix("config.") || text.hasPrefix("config_") || text.contains(".config.")
    }

    /// `split_config.arm64_v8a` is the split `config.arm64_v8a`.
    private static func splitID(fromFileStem stem: String) -> String {
        stem.lowercased().hasPrefix("split_") ? String(stem.dropFirst("split_".count)) : stem
    }

    // MARK: .xapk

    private static func xapk(_ archive: ZipArchive, apks: [ZipEntry], live: [ZipEntry]) throws -> ApkContainerPlan {
        guard let manifestEntry = archive.entry(named: "manifest.json") else { throw InstallRefusal.damaged }
        let manifest = try readManifest(archive, manifestEntry)
        let package = manifest["package_name"] as? String
        if let package, !isPackageName(package) { throw InstallRefusal.damaged }
        var parts: [ApkContainerPlan.Part] = []
        var baseName: String?
        if let splits = manifest["split_apks"] as? [[String: Any]], !splits.isEmpty {
            for item in splits {
                guard let file = item["file"] as? String, let id = item["id"] as? String,
                      let entry = archive.entry(named: file), !entry.isDirectory
                else { throw InstallRefusal.damaged }
                let isBase = id == "base"
                if isBase {
                    guard baseName == nil else { throw InstallRefusal.damaged }
                    baseName = entry.name
                }
                parts.append(ApkContainerPlan.Part(
                    entry: entry.name, bytes: entry.size, name: "", splitName: isBase ? nil : id, abi: isBase ? nil : abi(ofConfig: id)))
            }
        } else {
            // Version 1: the one APK of the file, which is the app.
            let candidates = apks.filter { !$0.name.contains("/") }
            let chosen: ZipEntry
            if candidates.count == 1, let only = candidates.first {
                chosen = only
            } else if let package, let named = candidates.first(where: { $0.name == package + ".apk" }) {
                chosen = named
            } else if let named = candidates.first(where: { basename($0.name).lowercased() == "base.apk" }) {
                chosen = named
            } else {
                throw InstallRefusal.damaged
            }
            baseName = chosen.name
            parts = [ApkContainerPlan.Part(entry: chosen.name, bytes: chosen.size, name: "", splitName: nil, abi: nil)]
        }
        guard let baseName else { throw InstallRefusal.damaged }
        let data = try obb(manifest: manifest, package: package, archive: archive, live: live)
        return try finish(
            ApkContainerPlan(kind: .xapk, packageName: package, candidates: parts, data: data.files, unplacedData: data.unplaced),
            base: baseName)
    }

    // MARK: .apkm

    /// An .apkm holds the APKs side by side and an `info.json`. Of that file
    /// only the package (`pname`) and the data files (`obb_files`, names of
    /// OBB files at the archive's root) are read, to place the data of an app
    /// the same way an .xapk's is placed.
    private static func apkm(_ archive: ZipArchive, apks: [ZipEntry], live: [ZipEntry]) throws -> ApkContainerPlan {
        var plan = try flat(apks, kind: .apkm)
        guard let infoEntry = archive.entry(named: "info.json") else { throw InstallRefusal.damaged }
        let info = try readManifest(archive, infoEntry)
        let package = info["pname"] as? String
        if let package, !isPackageName(package) { throw InstallRefusal.damaged }
        plan.packageName = package
        if let listed = info["obb_files"] {
            guard let names = listed as? [String], names.count <= maxParts else { throw InstallRefusal.damaged }
            let placed = try dataFiles(names, package: package, archive: archive) { name in "Android/obb/\(package ?? "")/\(name)" }
            plan.data = placed.files
            plan.unplacedData = placed.unplaced
        }
        return plan
    }

    /// The data files an archive names, each an entry of the archive; a file
    /// is placed where `destination` says if that is the package's own OBB
    /// folder (`obbFolder`), and counted as data not placed if it is not.
    private static func dataFiles(
        _ names: [String], package: String?, archive: ZipArchive, destination: (String) -> String
    ) throws -> (files: [ApkContainerPlan.DataFile], unplaced: Int) {
        var files: [ApkContainerPlan.DataFile] = []
        var unplaced = 0
        var seen = Set<String>()
        for name in names {
            guard let entry = archive.entry(named: name), !entry.isDirectory else { throw InstallRefusal.damaged }
            guard let package, let folder = obbFolder(for: destination(name), package: package) else {
                unplaced += 1
                continue
            }
            guard seen.insert(folder.name).inserted else { throw InstallRefusal.damaged }
            files.append(ApkContainerPlan.DataFile(entry: entry.name, bytes: entry.size, folder: folder.folder, name: folder.name))
        }
        return (files, unplaced)
    }

    private static func readManifest(_ archive: ZipArchive, _ entry: ZipEntry) throws -> [String: Any] {
        guard entry.size <= UInt64(maxManifestBytes) else { throw InstallRefusal.damaged }
        let data = try archive.data(of: entry, limit: maxManifestBytes)
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw InstallRefusal.damaged }
        return object
    }

    /// The data files of an .xapk: entries the manifest's `expansions` name,
    /// or, with none named, the entries under `Android/obb/<package>/`. A file
    /// is placed only as `Android/obb/<package>/<name>.obb`; any other data
    /// (`Android/data`, say) is counted, not placed.
    private static func obb(
        manifest: [String: Any], package: String?, archive: ZipArchive, live: [ZipEntry]
    ) throws -> (files: [ApkContainerPlan.DataFile], unplaced: Int) {
        var files: [ApkContainerPlan.DataFile] = []
        var unplaced = 0
        var seen = Set<String>()
        func add(entryName: String, destination: String) throws {
            guard let entry = archive.entry(named: entryName), !entry.isDirectory else { throw InstallRefusal.damaged }
            guard let package, let folder = obbFolder(for: destination, package: package) else {
                unplaced += 1
                return
            }
            guard seen.insert(folder.name).inserted else { throw InstallRefusal.damaged }
            files.append(ApkContainerPlan.DataFile(entry: entry.name, bytes: entry.size, folder: folder.folder, name: folder.name))
        }
        if let expansions = manifest["expansions"] as? [[String: Any]] {
            for item in expansions {
                guard let file = item["file"] as? String else { throw InstallRefusal.damaged }
                let destination = (item["install_path"] as? String) ?? file
                if let location = item["install_location"] as? String, location != "EXTERNAL_STORAGE" {
                    guard archive.entry(named: file) != nil else { throw InstallRefusal.damaged }
                    unplaced += 1
                    continue
                }
                try add(entryName: file, destination: destination)
            }
        } else {
            for entry in live where entry.name.hasPrefix("Android/") {
                try add(entryName: entry.name, destination: entry.name)
            }
        }
        return (files, unplaced)
    }

    /// `Android/obb/<package>/<name>.obb`, nothing else: one folder, the
    /// package's own, and one plain file name inside it.
    static func obbFolder(for path: String, package: String) -> (folder: String, name: String)? {
        let parts = path.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard parts.count == 4, parts[0] == "Android", parts[1] == "obb", parts[2] == package else { return nil }
        let name = parts[3]
        // Plain characters only: the name goes into a shell command and a path.
        guard name.hasSuffix(".obb"), name.utf8.count > 4, name.utf8.count <= 255, !name.hasPrefix("."),
              name.utf8.allSatisfy({ ($0 >= 48 && $0 <= 57) || ($0 >= 65 && $0 <= 90) || ($0 >= 97 && $0 <= 122) || $0 == 46 || $0 == 45 || $0 == 95 || $0 == 43 })
        else { return nil }
        return ("Android/obb/" + package, name)
    }

    /// Android's rule for a package name, narrowed to what a path may hold:
    /// dotted segments of ASCII letters, digits and underscores, each
    /// starting with a letter, at least two segments.
    static func isPackageName(_ name: String) -> Bool {
        let segments = name.split(separator: ".", omittingEmptySubsequences: false)
        guard segments.count >= 2, name.utf8.count <= 255 else { return false }
        return segments.allSatisfy { segment in
            guard let first = segment.utf8.first, (first >= 65 && first <= 90) || (first >= 97 && first <= 122) else { return false }
            return segment.utf8.allSatisfy { ($0 >= 48 && $0 <= 57) || ($0 >= 65 && $0 <= 90) || ($0 >= 97 && $0 <= 122) || $0 == 95 }
        }
    }

    // MARK: Processors

    private static let abiNames: [String: String] = [
        "arm64_v8a": "arm64-v8a", "armeabi_v7a": "armeabi-v7a", "armeabi": "armeabi", "x86": "x86", "x86_64": "x86_64",
        "mips": "mips", "mips64": "mips64", "riscv64": "riscv64",
    ]

    /// The processor a split's config names (`arm64_v8a`, `config.x86`,
    /// `feature.config.armeabi_v7a`); nil for a density, a language or a
    /// feature module.
    static func abi(ofConfig config: String) -> String? {
        let lowered = config.lowercased().replacingOccurrences(of: "-", with: "_")
        let tail = lowered.range(of: "config.", options: .backwards).map { String(lowered[$0.upperBound...]) } ?? lowered
        let token = tail.hasPrefix("config_") ? String(tail.dropFirst("config_".count)) : tail
        return abiNames[token]
    }

    // MARK: Finishing

    private static func finish(_ plan: ApkContainerPlan, base: String) throws -> ApkContainerPlan {
        var plan = plan
        plan.candidates = try arranged(plan.candidates, base: base)
        return plan
    }

    /// The parts of one install: the base first as `base.apk`, the others
    /// after it, each with a name of its own in the session. A part must be
    /// there once, hold bytes, and be one Android reads (under 4 GiB).
    static func arranged(_ parts: [ApkContainerPlan.Part], base: String) throws -> [ApkContainerPlan.Part] {
        guard parts.count <= maxParts, parts.contains(where: { $0.splitName == nil && $0.entry == base }) else {
            throw InstallRefusal.damaged
        }
        var ordered = parts.filter { $0.entry == base }
        ordered += parts.filter { $0.entry != base }
        var names = Set<String>()
        for index in ordered.indices {
            guard names.insert(ordered[index].entry).inserted else { throw InstallRefusal.damaged }
            let label = sanitized(stem(ordered[index].entry))
            ordered[index].name = index == 0 ? "base.apk" : "\(index)_\(label).apk"
            guard PackageInstallCommand.isValidPartName(ordered[index].name) else { throw InstallRefusal.damaged }
            // Each APK is Android's to parse, which reads none of 4 GiB or more.
            if ordered[index].bytes == 0 { throw InstallRefusal.damaged }
            if InstallRefusal(sizeBytes: ordered[index].bytes) != nil { throw InstallRefusal.partTooLarge }
        }
        return ordered
    }

    private static func sanitized(_ stem: String) -> String {
        let kept = stem.unicodeScalars.map { scalar -> Character in
            switch scalar.value {
            case 48...57, 65...90, 97...122, 45, 95: Character(scalar)
            default: "_"
            }
        }
        return String(kept.prefix(100))
    }
}

/// The parts of an app and the files that go with it, as an archive holds them.
public struct ApkContainerPlan: Equatable, Sendable {
    public enum Kind: Equatable, Sendable {
        /// bundletool's .apks (`toc.pb`, `splits/`).
        case bundletool
        /// An export of an installer app (`meta.sai_v2.json`), or any
        /// archive that holds a base APK and its splits.
        case sai
        case xapk
        case apkm
        case archive
    }

    /// One APK of the app.
    public struct Part: Equatable, Sendable {
        /// The entry in the archive.
        public var entry: String
        /// Its length, as the directory declares it.
        public var bytes: UInt64
        /// Its name in the install session.
        public var name: String
        /// The split's name; nil for the base.
        public var splitName: String?
        /// The processor a split carries code for, in Android's spelling.
        public var abi: String?
    }

    /// One data file of an .xapk, placed as `folder/name` below the guest's
    /// internal storage.
    public struct DataFile: Equatable, Sendable {
        public var entry: String
        public var bytes: UInt64
        public var folder: String
        public var name: String
    }

    /// One form of the app a bundletool file holds: the APKs it is made of and
    /// the Android it is for.
    public struct Variant: Equatable, Sendable {
        public var number: Int
        /// The lowest API level the variant is for.
        public var minSdk: Int
        /// The processors the variant is for, when it is one build per processor.
        public var abis: [String]
        /// The variant is for something the table names and this install does not weigh.
        public var otherTargeting: Bool
        /// The entry of the variant's base.
        public var base: String
        /// The entries of every APK of the variant, the base among them.
        public var entries: [String]
    }

    public var kind: Kind
    /// The app's package where the container says it. It is the container's
    /// own word until the package manager has checked it against the base APK
    /// (`PackageInstallCommand.createSession(packageName:)`).
    public var packageName: String?
    /// Every APK of the app. For a plan without variants the base is first,
    /// before the processor is chosen; with variants, the APKs of all of them.
    public var candidates: [Part]
    public var data: [DataFile]
    /// Data the archive carries that is not placed (anything but OBB files).
    public var unplacedData: Int
    /// The forms of the app a bundletool file holds; empty for any other.
    public var variants: [Variant]

    public init(
        kind: Kind, packageName: String?, candidates: [Part], data: [DataFile], unplacedData: Int, variants: [Variant] = []
    ) {
        self.kind = kind
        self.packageName = packageName
        self.candidates = candidates
        self.data = data
        self.unplacedData = unplacedData
        self.variants = variants
    }

    /// The parts this virtual machine installs: the variant for its Android
    /// (the one with the highest minimum API level that is not above
    /// `guestSdk`; the newest when the level is not known), then the base, the
    /// feature, density and language splits, and the splits of the one
    /// processor of `guestAbis` (its own list, most preferred first) that the
    /// app has code for, so that no unused code takes room and an app with a
    /// choice of 32 and 64 bits runs in 64. An app whose only code is for
    /// other processors has no match. The base is always the first part.
    public func selecting(guestAbis: [String], guestSdk: Int? = nil) throws -> ApkContainerPlan {
        var plan = self
        if !variants.isEmpty {
            let variant = try chosenVariant(guestAbis: guestAbis, guestSdk: guestSdk)
            let wanted = Set(variant.entries)
            plan.candidates = candidates.filter { wanted.contains($0.entry) }.map { part in
                var part = part
                if part.entry == variant.base { part.splitName = nil; part.abi = nil }
                return part
            }
            plan.variants = []
            plan.candidates = try ApkContainer.arranged(plan.candidates, base: variant.base)
        }
        let present = Set(plan.candidates.compactMap(\.abi))
        if !present.isEmpty {
            guard let chosen = guestAbis.first(where: { present.contains($0) }) else { throw InstallRefusal.noMatchingProcessor }
            plan.candidates = plan.candidates.filter { $0.abi == nil || $0.abi == chosen }
        }
        // The base is kept, first, whatever was filtered.
        guard let first = plan.candidates.first, first.splitName == nil, first.name == "base.apk", first.abi == nil,
              plan.candidates.filter({ $0.splitName == nil }).count == 1
        else { throw InstallRefusal.damaged }
        return plan
    }

    /// The variant to install: not one for another kind of device, for an
    /// Android no newer than the guest's, the newest of those, and among
    /// equals the one whose processor the guest prefers. None is a file whose
    /// parts do not fit this virtual machine.
    private func chosenVariant(guestAbis: [String], guestSdk: Int?) throws -> Variant {
        let level = guestSdk ?? Int.max
        func rank(_ variant: Variant) -> Int {
            if variant.abis.isEmpty { return guestAbis.count }
            return variant.abis.compactMap { guestAbis.firstIndex(of: $0) }.min() ?? Int.max
        }
        let fitting = variants.filter { !$0.otherTargeting && $0.minSdk <= level && rank($0) != Int.max }
        guard let best = fitting.min(by: { lhs, rhs in
            if lhs.minSdk != rhs.minSdk { return lhs.minSdk > rhs.minSdk }
            if rank(lhs) != rank(rhs) { return rank(lhs) < rank(rhs) }
            return lhs.number < rhs.number
        }) else { throw InstallRefusal.damaged }
        return best
    }

    /// Where `file` goes in the guest: its folder and its full path, both
    /// below internal storage. Nil unless the file is one the container can
    /// place, `Android/obb/<package>/<name>.obb` with this app's package, so
    /// that a plan built by hand cannot name any other place.
    public func guestLocation(of file: DataFile) -> (folder: String, path: String)? {
        guard let packageName, let place = ApkContainer.obbFolder(for: file.folder + "/" + file.name, package: packageName),
              place.folder == file.folder, place.name == file.name
        else { return nil }
        let folder = GuestFilePath.child(place.folder, of: GuestFilePath.root)
        let path = GuestFilePath.child(place.name, of: folder)
        guard GuestFilePath.isInsideRoot(path) else { return nil }
        return (folder, path)
    }

    /// The bytes of the APKs and of the data files, and both together.
    public var apkBytes: UInt64 { candidates.reduce(0) { Self.add($0, $1.bytes) } }
    public var dataBytes: UInt64 { data.reduce(0) { Self.add($0, $1.bytes) } }
    public var totalBytes: UInt64 { Self.add(apkBytes, dataBytes) }

    private static func add(_ a: UInt64, _ b: UInt64) -> UInt64 {
        let (sum, overflow) = a.addingReportingOverflow(b)
        return overflow ? .max : sum
    }
}
