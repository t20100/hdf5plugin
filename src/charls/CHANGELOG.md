# Change Log

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/) and this project adheres to [Semantic Versioning](http://semver.org/).

## [2.1.0] - 2019-12-29

### Added

- Two new C++ classes (jpegls_encoder \ jpegls_decoder) have been added to make it much easier to use CharLS from C++
- A new C API (charls_xxx functions) was added to provide a more stable ABI for future updates. The old API calls are internally forwarded to the new API.
- CharLS can now read and write JPEG-LS standard SPIFF headers
- Support has been added to detect the unsupported JPEG-LS extension (ISO/IEC 14495-2) SOF_57 marker and IDs in LSE marker
- The unit test project has been extended and now includes 188 tests.
- Support has been added to encode\decode 4 component images in all interleave modes

### Deprecated

- The legacy 1.x\2.0 C API has been marked as deprecated. This legacy API will be maintained until the next major upgrade.
  Future 2.x updates will start to mark the legacy types and functions with the C++ ```[[deprecated]]``` attribute.

### Changed

- charls_error has been replaced by a C++11 compatible jpegls_errc error code enum design
- The included C and C++ sample have been updated to use the new C\C++ API
- Improved the validation of the JPEG-LS stream during decoding
- #pragma once is now used to prevent that header files are included multiple times (supported by all modern C++ compilers)
- The referenced NuGet packages of the .NET wrapper assembly are updated to their latest versions
- The CMake build script has been updated to modern CMake and requires at least CMake 3.9
- All types are now in the charls C++ namespace
- All source code files now use the SPDX Unique License Identifiers (BSD-3-Clause) to identify the license

### Removed

- Support to write JFIF headers during encoding has been removed. JFIF headers were already skipped when present during decoding.
  SPIFF headers should be used when creating standalone .jls files
- Support for .NET Code Contracts has been removed as this technology is being phased out by Microsoft

### Fixed

- Fixed [#7](https://github.com/team-charls/charls/issues/7), How to compile CharLS with Xcode has been documented in the Wiki
- Fixed [#44](https://github.com/team-charls/charls/issues/44), Only the API functions should be exported from a Linux shared library
- Fixes [#35](https://github.com/team-charls/charls/issues/35), Encoding will fail if the bit per sample is greater than 8, and a custom RESET value is used
- Fixes [#51](https://github.com/team-charls/charls/issues/51), The default threshold values are not corrected computed for 6 bit images or less
- Fixed the ASSERT in the ModuloRange function, which would trigger false assertions in debug builds

## [2.0.0] - 2016-5-18

### Changed

- Updated the source code to C++14
- Refactored several APIs to make usage of the library easier

### Fixed

- Fixes [#10](https://github.com/team-charls/charls/issues/10), Fixed the problem that "output buffer to small" was not detected when writing encoded bytes to a fixed output buffer. This could cause memory corruption problems
- Fixes [11](https://github.com/team-charls/charls/issues/11), Update charlstest to return EXIT_SUCCESS/FAILURE
- Fixed the issue that DecodeToPnm would set params.colorTransform = ColorTransformation::BigEndian but the library didn’t support this option during decoding

## [1.1.0] - 2016-5-15

### Fixed

- Fixes [#9](https://github.com/team-charls/charls/issues/9) EncoderStrategy::AppendToBitStream method fails if buffer is full, 31 bits are added and xFF bytes are written

## [1.0.0] - 2010-11-18

First release of the CharLS JPEG-LS codec.
