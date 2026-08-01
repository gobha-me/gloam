/// SPEC §10, §19 step 5 — the offline asset pipeline.
///
/// "All assets generated offline. Nothing generated at runtime, no model in the
/// shipped binary." This is the offline half, and it is a separate executable
/// from `src/bin/main.cpp` for exactly that reason: the game must not be able to
/// bake, or "nothing generated at runtime" becomes a convention.
///
/// THIS FILE IS THE ONLY ONE IN THE PIPELINE THAT OPENS A FILE DESCRIPTOR.
/// `include/gloam/kitty.hpp`'s transmit note warns that a pixel source "would
/// drag image OWNERSHIP — a buffer of plate data — into `gloam::lib`". It does
/// not: the library's entry points all take caller-owned spans and tell the
/// caller how large to make them. The buffers live here, in `main`, on the same
/// footing as the `::write` that puts emitted bytes on a terminal.
///
/// WHAT IT BAKES TODAY: the six light fields (§4.4), and nothing else. They are
/// the one asset class §10 marks *procedural* — everything in the "Wall /
/// monster / item plates" row starts from a generated image that does not exist
/// yet, and no authoring format has been decided. See gloam#1.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "gloam/assets.hpp"
#include "gloam/budgets.hpp"
#include "gloam/gloam.hpp"  // version_string() only; the pipeline headers are off this umbrella
#include "gloam/lightfield.hpp"
#include "gloam/pack.hpp"
#include "gloam/plate.hpp"
#include "gloam/sha256.hpp"

namespace {

constexpr auto kDefaultOutput = "pack.gloam";

auto usage() -> void {
  std::cout << "usage: gloam_bake [-o PATH] [--quiet]\n"
            << "       gloam_bake --verify PATH\n"
            << "\n"
            << "Bakes GLOAM's asset pack (SPEC §10). Two runs over the same\n"
            << "input produce byte-identical output; the pack hash is a build gate.\n"
            << "\n"
            << "  -o PATH        write the pack here (default: " << kDefaultOutput << ")\n"
            << "  --verify PATH  check an existing pack and exit non-zero if it is bad\n"
            << "  --quiet        print only the pack hash\n"
            << "  --version      print the version and exit\n";
}

/// §10: "The build verifies it." This is that check, available against a pack
/// this process did not produce — which is the only interesting case, since a
/// baker trivially agrees with itself.
auto verify_file(const std::string& path, bool quiet) -> int {
  // THE REGULAR-FILE CHECK IS THE LOAD-BEARING ONE, and the size check below is
  // not a substitute for it. This comment used to claim that "tellg() returns
  // -1 on anything unseekable — a directory, a FIFO". It does not: opening a
  // directory with libstdc++ SUCCEEDS and `tellg()` returns LLONG_MAX, which
  // sailed past the `size < 0` guard and aborted the process on bad_alloc while
  // resizing to nine exabytes. `gloam_bake --verify .` did that until this line
  // existed. A pack also cannot be smaller than its own header.
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // Kept distinct from the regular-file check: a typo'd or not-yet-produced
    // path is the likeliest failure by far, and reporting it as "not a regular
    // file" sends a reader looking for a FIFO instead of a missing file.
    std::cerr << "gloam_bake: '" << path << "' does not exist\n";
    return EXIT_FAILURE;
  }
  if (!std::filesystem::is_regular_file(path, ec)) {
    std::cerr << "gloam_bake: '" << path << "' is not a regular file\n";
    return EXIT_FAILURE;
  }

  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    std::cerr << "gloam_bake: cannot open '" << path << "' for reading\n";
    return EXIT_FAILURE;
  }

  const auto size = static_cast<std::streamoff>(in.tellg());
  if (size < 0) {
    std::cerr << "gloam_bake: cannot determine the size of '" << path << "'\n";
    return EXIT_FAILURE;
  }
  if (static_cast<std::uintmax_t>(size) < gloam::pack::kHeaderBytes) {
    std::cerr << "gloam_bake: '" << path << "' is " << size
              << " B — too short to be a pack at all\n";
    return EXIT_FAILURE;
  }

  in.seekg(0);
  std::vector<std::byte> image(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char*>(image.data()), size);
  if (!in) {
    std::cerr << "gloam_bake: reading '" << path << "' failed\n";
    return EXIT_FAILURE;
  }

  const auto res = gloam::pack::verify(image);
  if (!res) {
    // §10 makes this fatal rather than advisory: "a mismatched hash refuses to
    // launch rather than half-uploading a corrupt plate set."
    std::cerr << "gloam_bake: '" << path << "' is not a valid pack — error "
              << static_cast<int>(res.error) << " at plate " << res.plate_index << '\n';
    return EXIT_FAILURE;
  }

  gloam::pack::Header header{};
  (void)gloam::pack::read_header(image, header);
  const auto hex = gloam::hash::to_hex(gloam::hash::sha256(image));
  if (!quiet) {
    std::cout << "ok  " << path << "  " << header.plate_count << " plates, " << image.size()
              << " B\n";
  }
  std::cout.write(hex.data(), static_cast<std::streamsize>(hex.size()));
  std::cout << '\n';
  return EXIT_SUCCESS;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  std::string output = kDefaultOutput;
  std::string verify_path;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      usage();
      return EXIT_SUCCESS;
    }
    if (arg == "--version") {
      std::cout << gloam::version_string() << '\n';
      return EXIT_SUCCESS;
    }
    if (arg == "--quiet") {
      quiet = true;
      continue;
    }
    if (arg == "-o" || arg == "--verify") {
      if (i + 1 >= argc) {
        std::cerr << "gloam_bake: " << arg << " needs a path\n";
        return EXIT_FAILURE;
      }
      (arg == "-o" ? output : verify_path) = argv[++i];
      continue;
    }
    std::cerr << "gloam_bake: unrecognised argument '" << arg << "'\n";
    usage();
    return EXIT_FAILURE;
  }

  if (!verify_path.empty()) return verify_file(verify_path, quiet);

  // ── Bake and assemble ───────────────────────────────────────────────────
  //
  // Every buffer is owned here. `gloam::assets` says what goes in the pack and
  // how big each buffer has to be; the library allocates nothing, and this is
  // the only translation unit in the pipeline that does.
  std::vector<std::byte> pixels(gloam::assets::pixel_bytes());
  std::vector<gloam::pack::Record> records(gloam::assets::kPlateCount);
  std::vector<std::span<const std::byte>> blobs(gloam::assets::kPlateCount);
  std::vector<std::byte> image(gloam::assets::image_bytes());

  const auto built = gloam::assets::build_pack(pixels, records, blobs, image);
  if (!built) {
    std::cerr << "gloam_bake: building the pack failed with error "
              << static_cast<int>(built.error) << " at plate " << built.plate_index << '\n';
    return EXIT_FAILURE;
  }

  // §11's residency cap. `pack.hpp` deliberately does not know about budgets
  // (the parser reports, the budget judges — `emit.hpp`'s rule), so the
  // comparison is made here and in `test/10budgets/`.
  if (records.size() > static_cast<std::size_t>(gloam::budget::kMaxResidentImages)) {
    std::cerr << "gloam_bake: " << records.size() << " plates exceeds §11's cap of "
              << gloam::budget::kMaxResidentImages << '\n';
    return EXIT_FAILURE;
  }

  if (!quiet) {
    for (const auto& record : records) {
      std::cout << "plate " << record.plate_id << "  " << record.w << "x" << record.h << "  "
                << record.length << " B  at " << record.offset << '\n';
    }
  }

  // ── Write ───────────────────────────────────────────────────────────────

  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "gloam_bake: cannot open '" << output << "' for writing\n";
    return EXIT_FAILURE;
  }
  out.write(reinterpret_cast<const char*>(image.data()),
            static_cast<std::streamsize>(image.size()));
  out.close();
  if (!out) {
    std::cerr << "gloam_bake: writing '" << output << "' failed\n";
    return EXIT_FAILURE;
  }

  const auto digest = gloam::hash::sha256(image);
  const auto hex = gloam::hash::to_hex(digest);

  if (!quiet) {
    std::cout << "\n"
              << records.size() << " plates, " << image.size() << " B -> " << output << '\n'
              << "pack sha256  ";
  }
  std::cout.write(hex.data(), static_cast<std::streamsize>(hex.size()));
  std::cout << '\n';

  return EXIT_SUCCESS;
}
