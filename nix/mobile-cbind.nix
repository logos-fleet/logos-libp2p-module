# nim-libp2p's C bindings, CROSS-BUILT for one of logos-nix's mobile
# pseudo-systems.
#
# WHY THIS FILE IS HERE AND NOT IN logos-module-builder. A module's
# `nix.external_libraries` are staged into lib/ by its own `generate` step as
# BUILD-PLATFORM images, and nothing in the builder can recompile them — each
# comes from its own flake, and only the consumer knows how that flake builds.
# So the builder asks the module's flake for a per-target build
# (`externalLibInputs.<name>.mobilePackages`) and stages it over the
# build-platform image. This is libp2p_module's answer.
#
# A STATIC ARCHIVE, where the desktop module links a shared library: a Bare
# module on a phone carries every third-party library inside its own image.
# `<App>.app/Frameworks/` holds frameworks, not loose dylibs, and on Android a
# `liblibp2p.so` sitting beside the module would be an unbundled soname and fail
# the DT_NEEDED gate. libplum (the NAT-traversal C code) comes along by itself —
# nim-libp2p compiles it in through `{.compile.}` pragmas.
#
# TWO THINGS THE DESKTOP BUILD GETS ELSEWHERE ARE BUNDLED HERE:
#
#   TinyCBOR       the generated header marshals through it and the module calls
#                  it directly. On the desktop it is `nix.packages.runtime`'s
#                  `tinycbor`; for iOS that package does not build at all
#                  (nixpkgs' own iOS stdenv, which logos-nix deliberately does
#                  not use). nim-ffi vendors the four .c files the header
#                  belongs to, so they are compiled for the target and added to
#                  the archive — one library, one provenance.
#   its headers    installed under include/tinycbor/, because that is how the
#                  generated header spells the include.
#
# The rest of the headers are NOT reinstalled: they are source, identical for
# every target, and `generate` already staged the build platform's copy.
{ lib, pkgs, libp2pSrc, target }:

let
  # nim, nimble and the dependency sources RUN on the builder; only the code
  # they emit is for the phone. Same shape as the builder's Rust cross: a cross
  # toolchain is chosen on the build platform, never taken from the target set.
  bp = pkgs.pkgsBuildBuild;

  isAndroid = target == "aarch64-android";

  deps = import "${libp2pSrc}/nix/deps.nix" { pkgs = bp; };
  cbindDeps = import "${libp2pSrc}/nix/cbind-deps.nix" { pkgs = bp; };

  pathArgs = lib.concatMapStringsSep " " (p: "--path:${p}") (lib.attrValues deps);
  cbindPathArgs = lib.concatMapStringsSep " " (p: "--path:${p}") (lib.attrValues cbindDeps);

  tinycborVendor = "${cbindDeps.ffi}/ffi/codegen/templates/cpp/vendor/tinycbor";

  # Where Xcode / the NDK are, from the repo that owns that decision. Both
  # mobile overlays contribute these two names, so there is one code path here.
  nimCrossFlags = lib.concatStringsSep " " pkgs.logosNimCrossFlags;

  # ── the nim half ────────────────────────────────────────────────────────
  # The argument list is upstream's nix/cbind.nix verbatim, plus this flake's
  # own `-d:chronicles_runtime_filtering=on` (the same override libp2pInputs
  # applies to the desktop package) and the cross flags. Deliberately only the
  # `--app:staticlib` half: the shared library is not shipped on a phone, and
  # the two binding-generation passes emit HEADERS, which are target-independent
  # and already in lib/.
  nimArchive = bp.stdenv.mkDerivation {
    pname = "nim-libp2p-cbind-${target}";
    version = "dev";
    src = libp2pSrc;

    nativeBuildInputs = [ bp.nim-2_2 bp.git bp.nimble ];

    # iOS reaches Xcode through /Applications (logos-nix ADR 0002); the Android
    # NDK is in the store and needs no escape hatch.
    __noChroot = !isAndroid;

    buildPhase = ''
      runHook preBuild
      export HOME=$TMPDIR XDG_CACHE_HOME=$TMPDIR/.cache
      export NIMBLE_DIR=$TMPDIR/.nimble NIMCACHE=$TMPDIR/nimcache
      mkdir -p build $NIMCACHE

      # logosNimCrossSetup appends to this; on Android it is empty, because
      # there the whole toolchain is knowable at eval time.
      nimFlagsArray=()
      ${pkgs.logosNimCrossSetup}

      nim c ${nimCrossFlags} "''${nimFlagsArray[@]}" \
        --noNimblePath ${cbindPathArgs} ${pathArgs} \
        --threads:on --opt:size --noMain --mm:refc --d:metrics \
        -d:chronicles_runtime_filtering=on \
        -d:ffiThreadExitTimeoutMs=5000 \
        --passC:-fPIC \
        --nimMainPrefix:liblibp2p --nimcache:$NIMCACHE \
        --app:staticlib --out:build/liblibp2p.a cbind/libp2p.nim
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      mkdir -p $out/lib
      cp build/liblibp2p.a $out/lib/
      runHook postInstall
    '';

    # This derivation runs in the BUILD platform's stdenv (so nim is runnable)
    # and produces a TARGET archive, so fixup's strip is the wrong one. Measured
    # on aarch64-darwin: Darwin's strip rewrites an aarch64-android archive's
    # index into BSD's `__.SYMDEF SORTED`, and ld.lld then rejects it with "not
    # an ELF file" naming the `/` member — a message about the archive INDEX
    # that reads like a message about the objects.
    dontStrip = true;
  };

  # ── TinyCBOR, and the archive they share ────────────────────────────────
  # In the TARGET's own stdenv, because `$AR` has to write an index the target's
  # linker reads and `$CC` has to emit the target's objects. Xcode's clang for
  # iOS (nix's cc-wrapper cannot target it), the NDK cross stdenv for Android.
  mkTargetDrv = if isAndroid then pkgs.stdenv.mkDerivation else pkgs.xcodeClang.mkDerivation;

  # xcodeClang's preConfigure exports CC/AR out of xcrun, but a plain compile
  # gets no sysroot or triple from that — a CMake project would have taken them
  # from CMAKE_SYSTEM_NAME=iOS. Named here, from logos-nix's own spelling of
  # this platform, so the objects are not quietly macOS ones.
  iosFlags = lib.optionalString (!isAndroid)
    "--target=${pkgs.logosIosTriple} -isysroot $(xcrun --sdk ${pkgs.logosIosAppleSdk} --show-sdk-path)";

in mkTargetDrv {
  pname = "libp2p-cbind-${target}";
  version = "dev";

  dontUnpack = true;
  # xcodeClang.mkDerivation puts cmake on PATH for the projects that want it;
  # this is four .c files and a copy.
  dontUseCmakeConfigure = true;

  buildPhase = ''
    runHook preBuild
    mkdir -p $out/lib $out/include/tinycbor
    cp ${tinycborVendor}/*.h $out/include/tinycbor/

    for c in ${tinycborVendor}/*.c; do
      "$CC" ${iosFlags} -Os -fPIC -I${tinycborVendor} -c "$c" -o "$(basename "$c" .c).o"
    done

    # `ar r` INTO the nim archive rather than a merge of two: there is one
    # external library as far as _logos_find_external_lib is concerned, and it
    # resolves exactly one file.
    cp ${nimArchive}/lib/liblibp2p.a $out/lib/liblibp2p.a
    chmod u+w $out/lib/liblibp2p.a
    "$AR" r $out/lib/liblibp2p.a ./*.o
    runHook postBuild
  '';

  dontInstall = true;
  # The archive is the artifact and nothing may rewrite it: the build platform's
  # strip is the wrong one for every target here.
  dontFixup = true;
}
