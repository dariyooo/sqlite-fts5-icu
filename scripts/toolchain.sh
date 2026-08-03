#!/usr/bin/env bash
# Sourced by build.sh. Sets the compiler, flags and library names for one
# target, and reports whether the result can be run on the current machine.
#
# Target ids are the release asset stems: fts5_icu_<target>.<ext>. The Dart
# build hook in DaLang looks for exactly those names.

# Deployment targets follow the DaKanji app project settings.
: "${MACOS_MIN:=11.0}"
: "${IOS_MIN:=14.0}"
: "${ANDROID_API:=21}"

toolchain_targets() {
  echo "mac_arm64 mac_x64 ios_arm64 ios-sim_arm64 ios-sim_x64 \
linux_x64 linux_arm64 android_arm64 android_arm android_x64 \
windows_x64 windows_arm64"
}

# "yes" when a library built for $1/$2 can be loaded on this machine.
_runs_here() {
  local want_os="$1" want_arch="$2" host_arch
  case "$(uname -m)" in
    arm64|aarch64) host_arch=arm64 ;;
    x86_64|amd64) host_arch=x64 ;;
    *) host_arch=other ;;
  esac
  local host_os="$(uname -s)"
  # Under MSYS2 uname reports the environment, not the platform.
  case "$host_os" in MINGW*|MSYS*|CYGWIN*) host_os=Windows ;; esac

  if [[ "$host_os" != "$want_os" ]]; then
    echo no
  elif [[ "$host_arch" == "$want_arch" ]]; then
    echo yes
  elif [[ "$want_os" == Darwin && "$host_arch" == arm64 && "$want_arch" == x64 ]] \
    && pgrep -q oahd 2> /dev/null; then
    # Rosetta is running, so the x86_64 build can be tested here too.
    echo yes
  else
    echo no
  fi
}

# Fails with a message when the NDK is not where the Android targets expect it.
_android_toolchain_bin() {
  local ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_ROOT:-}}}"
  if [[ -z "$ndk" || ! -d "$ndk" ]]; then
    echo "set ANDROID_NDK_HOME to build an android target" >&2
    exit 1
  fi
  local host
  case "$(uname -s)" in
    Darwin) host=darwin-x86_64 ;;
    *) host=linux-x86_64 ;;
  esac
  echo "$ndk/toolchains/llvm/prebuilt/$host/bin"
}

# Sets every TARGET_* variable for "$1".
toolchain_configure() {
  local target="$1"

  TARGET_AR="" TARGET_RANLIB="" TARGET_STRIP=""

  case "$target" in
    mac_*|ios_*|ios-sim_*)
      SHARED_EXT=dylib
      CXX_RUNTIME_LIBS=""
      # Only the entry point leaves the library; ICU's symbols stay private so
      # they can never bind to a system ICU of a different version.
      # headerpad_max_install_names leaves room for a consumer to rewrite the
      # install name to an absolute path, which Dart's asset bundling does.
      SHARED_LDFLAGS="-dynamiclib -Wl,-dead_strip -Wl,-headerpad_max_install_names"
      EXPORT_SYMBOL_PREFIX=_
      SMOKE_LIBS="-lm"
      STRIP_FLAGS="-x"
      ;;
    linux_*|android_*)
      SHARED_EXT=so
      CXX_RUNTIME_LIBS="-lm"
      SHARED_LDFLAGS="-shared -Wl,--gc-sections"
      EXPORT_SYMBOL_PREFIX=
      SMOKE_LIBS="-lm -ldl -lpthread"
      STRIP_FLAGS="--strip-unneeded"
      ;;
    windows_*)
      SHARED_EXT=dll
      # Pulls the toolchain's C++ library and unwinder in statically, so the
      # DLL depends on nothing but the system CRT.
      CXX_RUNTIME_LIBS="-static"
      SHARED_LDFLAGS="-shared -Wl,--gc-sections"
      EXPORT_SYMBOL_PREFIX=
      SMOKE_LIBS=""
      STRIP_FLAGS="--strip-all"
      ;;
    *)
      echo "unknown target: $target" >&2
      exit 1
      ;;
  esac

  local sdk arch bin
  case "$target" in
    mac_arm64|mac_x64)
      arch=$([[ "$target" == mac_arm64 ]] && echo arm64 || echo x86_64)
      TARGET_TRIPLE=$([[ "$target" == mac_arm64 ]] && echo aarch64-apple-darwin || echo x86_64-apple-darwin)
      TARGET_CC=clang
      TARGET_CXX=clang++
      TARGET_CFLAGS="-arch $arch -mmacosx-version-min=$MACOS_MIN"
      TARGET_LDFLAGS=""
      TARGET_CAN_RUN=$(_runs_here Darwin "${target#mac_}")
      ;;
    ios_arm64)
      sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
      TARGET_TRIPLE=aarch64-apple-darwin
      TARGET_CC=clang
      TARGET_CXX=clang++
      TARGET_CFLAGS="-arch arm64 -isysroot $sdk -miphoneos-version-min=$IOS_MIN"
      TARGET_LDFLAGS="-isysroot $sdk"
      TARGET_CAN_RUN=no
      ;;
    ios-sim_arm64|ios-sim_x64)
      arch=$([[ "$target" == ios-sim_arm64 ]] && echo arm64 || echo x86_64)
      sdk="$(xcrun --sdk iphonesimulator --show-sdk-path)"
      TARGET_TRIPLE=$([[ "$arch" == arm64 ]] && echo aarch64-apple-darwin || echo x86_64-apple-darwin)
      TARGET_CC=clang
      TARGET_CXX=clang++
      TARGET_CFLAGS="-arch $arch -isysroot $sdk -mios-simulator-version-min=$IOS_MIN"
      TARGET_LDFLAGS="-isysroot $sdk"
      TARGET_CAN_RUN=no
      ;;
    linux_x64)
      TARGET_TRIPLE=x86_64-linux-gnu
      TARGET_CC=gcc
      TARGET_CXX=g++
      TARGET_CFLAGS=""
      TARGET_LDFLAGS=""
      TARGET_CAN_RUN=$(_runs_here Linux x64)
      ;;
    linux_arm64)
      TARGET_TRIPLE=aarch64-linux-gnu
      TARGET_CC=aarch64-linux-gnu-gcc
      TARGET_CXX=aarch64-linux-gnu-g++
      TARGET_AR=aarch64-linux-gnu-ar
      TARGET_RANLIB=aarch64-linux-gnu-ranlib
      TARGET_STRIP=aarch64-linux-gnu-strip
      TARGET_CFLAGS=""
      TARGET_LDFLAGS=""
      TARGET_CAN_RUN=no
      ;;
    android_arm64|android_arm|android_x64)
      bin="$(_android_toolchain_bin)"
      case "$target" in
        android_arm64) TARGET_TRIPLE=aarch64-linux-android ;;
        android_arm)   TARGET_TRIPLE=armv7a-linux-androideabi ;;
        android_x64)   TARGET_TRIPLE=x86_64-linux-android ;;
      esac
      TARGET_CC="$bin/${TARGET_TRIPLE}${ANDROID_API}-clang"
      TARGET_CXX="$bin/${TARGET_TRIPLE}${ANDROID_API}-clang++"
      TARGET_AR="$bin/llvm-ar"
      TARGET_RANLIB="$bin/llvm-ranlib"
      TARGET_STRIP="$bin/llvm-strip"
      TARGET_CFLAGS=""
      # 32-bit arm needs libatomic for the 64-bit atomics ICU uses.
      TARGET_LDFLAGS=$([[ "$target" == android_arm ]] && echo "-latomic" || echo "")
      CXX_RUNTIME_LIBS="-static-libstdc++ -lm"
      # The NDK names the 32-bit arm compiler after the ABI variant, which
      # config.sub does not recognise as a triple.
      case "$target" in
        android_arm) TARGET_TRIPLE=arm-linux-androideabi ;;
      esac
      TARGET_CAN_RUN=no
      ;;
    windows_x64|windows_arm64)
      TARGET_TRIPLE=$([[ "$target" == windows_x64 ]] && echo x86_64-w64-mingw32 || echo aarch64-w64-mingw32)
      TARGET_CC="${TARGET_TRIPLE}-clang"
      TARGET_CXX="${TARGET_TRIPLE}-clang++"
      TARGET_AR="llvm-ar"
      TARGET_RANLIB="llvm-ranlib"
      TARGET_STRIP="llvm-strip"
      TARGET_CFLAGS=""
      TARGET_LDFLAGS=""
      TARGET_CAN_RUN=$(_runs_here Windows "${target#windows_}")
      ;;
  esac

  : "${TARGET_AR:=ar}"
  : "${TARGET_RANLIB:=ranlib}"
  : "${TARGET_STRIP:=strip}"
  export TARGET_TRIPLE TARGET_CC TARGET_CXX TARGET_AR TARGET_RANLIB TARGET_STRIP
  export TARGET_CFLAGS TARGET_LDFLAGS TARGET_CAN_RUN
  export SHARED_EXT SHARED_LDFLAGS CXX_RUNTIME_LIBS EXPORT_SYMBOL_PREFIX SMOKE_LIBS
  export STRIP_FLAGS ICU_LIB_UC ICU_LIB_I18N ICU_LIB_DATA
}
