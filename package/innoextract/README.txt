innoextract for the Thief RT installer
======================================

The HD Mods for Thief Gold and Thief II come as Windows installers (Inno Setup). The
installer and the launcher unpack them with innoextract, built here for Apple silicon and
put in their Resources, so players need nothing else (Homebrew's innoextract is used only
if this one is missing).

  innoextract 1.9 (https://github.com/dscharrer/innoextract, zlib licence)
    + boost-1.86-directory.patch (an include newer Boost needs)
  Boost 1.86.0 (iostreams, filesystem, date_time, system, program_options; static; Boost
    Software License 1.0)
  xz 5.6.3's liblzma (static; 0BSD)
  zlib, bzip2, iconv, libc++: macOS's own

Built with clang (LLVM 21) for arm64-apple-macos13.0 against the macOS 14.5 SDK, linked by
ld64.lld (ad hoc signed); cmake options: USE_STATIC_LIBS=ON USE_LZMA=ON USE_LTO=OFF.
