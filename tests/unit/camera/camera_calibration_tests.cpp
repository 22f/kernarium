// SPDX-License-Identifier: GPL-2.0-or-later
// The colour data a Bayer still teaches, kept per host model and lens: it
// survives a new store over the same directory, a damaged file is ignored, a
// save replaces the file whole, and a store with no directory keeps memory only.

#include "calibration_store.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>

using namespace uml::camera;
using namespace uml::platform;
using uml::testing::require;

namespace {

CameraColorData sample(double seed)
{
    CameraColorData data;
    data.bayer = BayerOrder::gbrg;
    data.blackLevel = 64;
    data.whiteLevel = 1023;
    data.rawWidth = 4032;
    data.rawHeight = 3024;
    data.illuminant1 = 17;
    data.illuminant2 = 21;
    for (std::size_t index = 0; index < 9; ++index) {
        data.colorMatrix1[index] = seed + double(index) * 0.125;
        data.colorMatrix2[index] = seed - double(index) * 0.0625;
        data.forwardMatrix1[index] = seed * 2 + double(index);
        data.forwardMatrix2[index] = seed * 3 + double(index);
    }
    data.focalLengthMillimetres = 6.86;
    data.intrinsics = {3100.5, 3101.5, 2016.0, 1512.0};
    return data;
}

CameraInfo lens(CameraPosition position, CameraLensKind kind)
{
    CameraInfo info;
    info.position = position;
    info.lens = kind;
    return info;
}

void testKeys()
{
    require(CalibrationStore::lensKey(lens(CameraPosition::back, CameraLensKind::wide)) !=
                CalibrationStore::lensKey(lens(CameraPosition::front, CameraLensKind::wide)),
            "the same lens on another side is another key");
    require(CalibrationStore::lensKey(lens(CameraPosition::back, CameraLensKind::wide)) !=
                CalibrationStore::lensKey(lens(CameraPosition::back, CameraLensKind::ultraWide)),
            "another lens is another key");
    require(CalibrationStore::lensKey(lens(CameraPosition::back, CameraLensKind::wide)) ==
                CalibrationStore::lensKey(lens(CameraPosition::back, CameraLensKind::wide)),
            "a key is stable");
}

void testMemoryOnly()
{
    CalibrationStore store;
    require(!store.load("Model1,1", "back-wide"), "nothing is known at first");
    require(store.save("Model1,1", "back-wide", sample(1.0)), "memory always saves");
    require(store.load("Model1,1", "back-wide") == sample(1.0), "a saved value comes back");
    require(!store.load("Model1,2", "back-wide"), "another model has nothing");
    require(!store.load("Model1,1", "front-wide"), "another lens has nothing");
}

void testPersistence()
{
    uml::testing::ScratchSubdirectory directory("camera-calibration-XXXXXX");
    const auto root = directory.path() / "store";
    {
        CalibrationStore store(root);
        require(store.save("Model/1,1", "back wide", sample(2.0)), "the first save creates the directory");
        require(store.save("Model/1,1", "front-wide", sample(3.0)), "a second lens");
    }
    CalibrationStore reopened(root);
    require(reopened.load("Model/1,1", "back wide") == sample(2.0), "a new store reads the first lens");
    require(reopened.load("Model/1,1", "front-wide") == sample(3.0), "and the second");
    require(!reopened.load("Model/1,1", "back-ultrawide"), "and nothing it was not given");

    // A save replaces the file whole and leaves no temporary file behind.
    require(reopened.save("Model/1,1", "back wide", sample(4.0)), "replacing a value");
    CalibrationStore third(root);
    require(third.load("Model/1,1", "back wide") == sample(4.0), "the replacement is what a later store reads");
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        ++files;
        require(entry.path().extension() == ".cal", "only calibration files are left");
    }
    require(files == 2, "one file a lens");
}

void testDamagedFile()
{
    uml::testing::ScratchSubdirectory directory("camera-calibration-XXXXXX");
    const auto root = directory.path();
    {
        CalibrationStore store(root);
        require(store.save("M", "lens", sample(5.0)), "save");
    }
    std::filesystem::path file;
    for (const auto& entry : std::filesystem::directory_iterator(root)) file = entry.path();
    require(!file.empty(), "a file was written");
    // Flip one byte: the check word no longer matches.
    {
        std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
        stream.seekp(20);
        char byte = 0;
        stream.read(&byte, 1);
        stream.seekp(20);
        byte = char(byte ^ 0x5a);
        stream.write(&byte, 1);
    }
    CalibrationStore damaged(root);
    require(!damaged.load("M", "lens"), "a file that fails its check is ignored");
    // Cut short.
    std::filesystem::resize_file(file, 12);
    CalibrationStore shortened(root);
    require(!shortened.load("M", "lens"), "a short file is ignored");
    // An empty file, then a good save over it.
    std::filesystem::resize_file(file, 0);
    CalibrationStore empty(root);
    require(!empty.load("M", "lens"), "an empty file is ignored");
    require(empty.save("M", "lens", sample(6.0)), "a save over a damaged file");
    CalibrationStore after(root);
    require(after.load("M", "lens") == sample(6.0), "and it reads back");
}

void testUnwritableDirectory()
{
    uml::testing::ScratchSubdirectory directory("camera-calibration-XXXXXX");
    // A file where the directory should be.
    const auto blocker = directory.path() / "blocker";
    std::ofstream(blocker).put('x');
    CalibrationStore store(blocker / "inside");
    require(!store.save("M", "lens", sample(7.0)), "an unwritable directory reports the failure");
    require(store.load("M", "lens") == sample(7.0), "but the value is kept in memory");
}

} // namespace

int main()
{
    testKeys();
    testMemoryOnly();
    testPersistence();
    testDamagedFile();
    testUnwritableDirectory();
    return 0;
}
