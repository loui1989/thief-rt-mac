7-Zip (7zz) for the Thief RT installer
======================================

The Thief II HD Mod (Thief 2 HD Mod 1.0) comes as a Windows installer made with NSIS. The
installer and the launcher unpack it with 7-Zip's console program, built here for Apple
silicon and put in their Resources, so players need nothing else (Homebrew's 7zz is used
only if this one is missing). Inno Setup installers are unpacked with innoextract instead
(../innoextract).

  7-Zip 26.06 (https://github.com/ip7z/7zip, commit d3296e3), unmodified: its full console
  program (CPP/7zip/Bundles/Alone2). Licence: GNU LGPL 2.1 or later, with the BSD 3-clause
  and BSD 2-clause parts and the unRAR restriction it names (licences/7-Zip-License.txt).
  The source is at the address above; Igor Pavlov, https://www.7-zip.org.

Built with clang (LLVM 21) for arm64-apple-macos13.0 against the macOS 14.5 SDK, with its
arm64 assembly, linked by ld64.lld (ad hoc signed):

  cd CPP/7zip/Bundles/Alone2
  make -f ../../cmpl_mac_arm64.mak CC=clang CXX=clang++ \
    MY_ARCH="-target arm64-apple-macos13.0 -isysroot MacOSX14.5.sdk" \
    LD_arch="-fuse-ld=lld --ld-path=ld64.lld -mlinker-version=711" \
    LFLAGS_NOEXECSTACK= LFLAGS_SRC_VER= LFLAGS_STRIP=

On a Mac, plain "make -f ../../cmpl_mac_arm64.mak" there builds the same.
