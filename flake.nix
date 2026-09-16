{
  description = "Logos Libp2p Module";

  # Pull pre-built artifacts from the self-hosted Logos Attic cache(Nix binary cache).
  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [ "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU=" ];
  };

  inputs = {
    # A rev on the logos-fleet fork, not logos-co, and not a tag: the mobile
    # Bare outputs this flake exposes below -- and the `mobilePackages` contract
    # the libp2p externalLibInput answers -- are a property of the BUILDER, and
    # only that line has them yet. A builder without them simply publishes no
    # mobile keys in `packages`, so pointing this back at logos-co degrades the
    # flake rather than breaking it.
    logos-module-builder.url = "github:logos-fleet/logos-module-builder/738f1a6ef5a6f755f8433297ac0d2ef54bba8d2f";
    libp2p.url = "github:vacp2p/nim-libp2p/master";

    openmetrics-module = {
      url = "github:logos-co/openmetrics-module";
      inputs.logos-module-builder.follows = "logos-module-builder";
    };

    # logoscore + lgpm binaries the openmetrics e2e drives.
    logoscore-cli.url = "github:logos-co/logos-logoscore-cli";
    package-manager.url = "github:logos-co/logos-package-manager";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
      nixpkgs = logos-module-builder.inputs.nixpkgs;
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];

      forEachSystem = f: builtins.listToAttrs (map (system: {
        name = system;
        value = f system;
      }) systems);

      libp2pInputs = {
        packages = forEachSystem (system: {
          cbind = inputs.libp2p.packages.${system}.cbind.overrideAttrs (old: {
            buildPhase = builtins.replaceStrings
              [ "--threads:on --opt:size --noMain --mm:refc --d:metrics" ]
              [ "--threads:on --opt:size --noMain --mm:refc --d:metrics -d:chronicles_runtime_filtering=on" ]
              old.buildPhase;
          });
        });
      };

      externalLibInputs = {
        libp2p = {
          input = libp2pInputs;
          packages.default = "cbind";
          # THE MOBILE HALF. nim-libp2p's own flake answers for five desktop
          # systems and knows nothing about a phone, and logos-module-builder
          # cannot recompile an external library for one either -- it comes from
          # somewhere else entirely. So this flake cross-builds `cbind` itself,
          # from the SAME locked input, and the builder stages the result over
          # the build-platform image its `generate` step left in lib/. See
          # nix/mobile-cbind.nix.
          mobilePackages = { system, pkgs, buildSystem }:
            import ./nix/mobile-cbind.nix {
              inherit pkgs;
              inherit (nixpkgs) lib;
              libp2pSrc = inputs.libp2p;
              target = system;
            };
        };
      };

      # Every test derivation needs this, or a missing library is a silent skip again.
      requireLibp2pLib = "-DLIBP2P_TESTS_REQUIRE_LIB=ON";

      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        inherit externalLibInputs;
        tests = {
          dir = ./tests;
          extraCmakeFlags = [ requireLibp2pLib ];
        };
      };

      # Pre-resolved store paths for the two .lgx bundles the e2e installs.
      e2eEnv = system: {
        LIBP2P_LGX_DIR      = "${module.packages.${system}.lgx}";
        OPENMETRICS_LGX_DIR = "${inputs.openmetrics-module.packages.${system}.lgx}";
      };

      # mkLogosModuleTests splats extraCmakeFlags into cmake unquoted, so
      # multi-token values get shell-split. Inject sanitizer flags via env vars.
      baseSanitizerTests = logos-module-builder.lib.mkLogosModuleTests {
        src = ./.;
        testDir = ./tests;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        inherit externalLibInputs;
        extraCmakeFlags = [ "-DCMAKE_BUILD_TYPE=Debug" requireLibp2pLib ];
      };

      mkSanitized = system: sanitizer: runtimeOpts:
        let
          pkgs = import nixpkgs { inherit system; };
          clang = pkgs.clang;
          llvm = pkgs.llvm;  # llvm-symbolizer; symbol-based suppressions need it
          sanFlags = "-fsanitize=${sanitizer} -fno-omit-frame-pointer -g -O1";
          # deadlock: libstdc++ leaves a std::mutex undestroyed on glibc, so tsan fuses two tests' stack-recycled locks into one node of its lock-order graph.
          tsanSuppressions = pkgs.writeText "tsan.supp" ''
            race:libp2p.so
            deadlock:libp2p.so
          '';
          # Wrapper code (Libp2pModuleImpl, src/*.cpp) is NOT suppressed —
          # real wrapper leaks (e.g. src/plugin.cpp:121 privkey) still surface.
          lsanSuppressions = pkgs.writeText "lsan.supp" ''
            leak:libp2p.so
            leak:std::promise
            leak:std::__future_base
            leak:QCoreApplication
            leak:QMetaObject
            leak:QArrayData
            leak:QObjectPrivate
            leak:LogosTestRunner
            leak:LogosTestContext
          '';
          extraRuntime =
            if sanitizer == "address" then ''
              export ASAN_SYMBOLIZER_PATH=${llvm}/bin/llvm-symbolizer
              export LSAN_OPTIONS="suppressions=${lsanSuppressions}:print_suppressions=0"
            '' else ''
              export TSAN_OPTIONS="$TSAN_OPTIONS:suppressions=${tsanSuppressions}:external_symbolizer_path=${llvm}/bin/llvm-symbolizer"
            '';
        in baseSanitizerTests.${system}.unit-tests.overrideAttrs (old: {
          nativeBuildInputs = (old.nativeBuildInputs or []) ++ [ clang llvm ];
          hardeningDisable = (old.hardeningDisable or []) ++ [ "all" ];
          preBuild = (old.preBuild or "") + ''
            export CC=${clang}/bin/clang
            export CXX=${clang}/bin/clang++
            export CFLAGS="${sanFlags}"
            export CXXFLAGS="${sanFlags}"
            export LDFLAGS="-fsanitize=${sanitizer}"
            export ${runtimeOpts}
            ${extraRuntime}
          '';
        });

      sanitizerPackages = builtins.listToAttrs (map (system: {
        name = system;
        value = {
          unit-tests-asan = mkSanitized system "address"
            "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_stacktrace=1:strict_string_checks=1:check_initialization_order=1";
          unit-tests-tsan = mkSanitized system "thread"
            "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:history_size=7";
        };
      }) systems);

      perSystem = forEachSystem (system:
        let
          pkgs = import nixpkgs { inherit system; };
          unitTests = module.packages.${system}.unit-tests;

          runner = pkgs.writeShellScript "run-tests" ''
            filter="''${1:-}"
            ran=0
            for bin in ${unitTests}/bin/*; do
              name="$(basename "$bin")"
              if [ -n "$filter" ] && ! echo "$name" | grep -q "$filter"; then
                continue
              fi
              echo "=== $name ==="
              "$bin"
              ran=$((ran + 1))
            done
            if [ "$ran" -eq 0 ] && [ -n "$filter" ]; then
              echo "No test binary matched filter: $filter" >&2
              exit 1
            fi
          '';

          env = e2eEnv system;
          e2eRuntime = [ pkgs.coreutils pkgs.gnugrep pkgs.bash pkgs.iproute2 pkgs.jq ];
          e2eScript = ./tests/integration_e2e/openmetrics_e2e.sh;
          standaloneE2eScript = ./tests/integration_e2e/standalone_e2e.sh;

          # `nix run .#openmetrics-e2e`: standalone, runs a live logoscore
          # daemon so it can't be a hermetic flake check. LOGOSCORE_BIN /
          # LGPM_BIN override the vendored binaries when set.
          logoscoreBin = "${inputs.logoscore-cli.packages.${system}.default}/bin/logoscore";
          lgpmBin = "${inputs.package-manager.packages.${system}.cli}/bin/lgpm";
          openmetricsE2eApp = pkgs.writeShellScript "openmetrics-e2e" ''
            export PATH=${pkgs.lib.makeBinPath e2eRuntime}:$PATH
            export LIBP2P_LGX_DIR=${env.LIBP2P_LGX_DIR}
            export OPENMETRICS_LGX_DIR=${env.OPENMETRICS_LGX_DIR}
            export LOGOSCORE_BIN="''${LOGOSCORE_BIN:-${logoscoreBin}}"
            export LGPM_BIN="''${LGPM_BIN:-${lgpmBin}}"
            exec ${e2eScript} "$@"
          '';
          standaloneE2eApp = pkgs.writeShellScript "standalone-e2e" ''
            export PATH=${pkgs.lib.makeBinPath e2eRuntime}:$PATH
            export LIBP2P_LGX_DIR=${env.LIBP2P_LGX_DIR}
            export LOGOSCORE_BIN="''${LOGOSCORE_BIN:-${logoscoreBin}}"
            export LGPM_BIN="''${LGPM_BIN:-${lgpmBin}}"
            exec ${standaloneE2eScript} "$@"
          '';
        in {
          # *-e2e run a live logoscore daemon, so they can't be hermetic flake
          # checks; they're standalone apps run as their own CI step.
          apps = {
            tests = { type = "app"; program = toString runner; };
          } // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
            openmetrics-e2e = { type = "app"; program = toString openmetricsE2eApp; };
            standalone-e2e = { type = "app"; program = toString standaloneE2eApp; };
          };
        }
      );

      existingApps = module.apps or {};

      mergedApps = forEachSystem (system:
        (existingApps.${system} or {}) // (perSystem.${system}.apps or {})
      );

      mergedPackages = builtins.listToAttrs (map (system: {
        name = system;
        value = (module.packages.${system} or {}) // (sanitizerPackages.${system} or {});
      }) systems);

      mergedDevShells = forEachSystem (system:
        let pkgs = import nixpkgs { inherit system; };
        in (module.devShells.${system} or {}) // {
          default = pkgs.mkShell {
            inputsFrom = [ module.devShells.${system}.default ];
            buildInputs = [ pkgs.boost.dev ];
          };
        });

      # The mobile pseudo-systems the builder adds to `packages` when its
      # logos-nix carries the cross sets. They are deliberately not in `systems`
      # above, for the reason the builder keeps them out of its own: a phone
      # gets the Bare module and none of the other twenty outputs. So
      # mergedPackages, built from `systems` alone, has to have them merged back
      # in below or `packages` would lose them.
      #
      # READ OFF THE BUILDER rather than restated here: which targets exist is
      # its logos-nix's answer, and a list copied into this flake would go stale
      # silently. `or [ ]` because a builder predating them has no such
      # attribute — then this is simply a flake without mobile keys.
      mobileTargets = logos-module-builder.lib.common.mobileSystems or [ ];

    in module // {
      apps = mergedApps;
      checks = module.checks or {};
      # nix build .#packages.aarch64-ios.bare -- libp2p_module and nim-libp2p's
      # cbind, both cross-compiled, in one protocol-free image an iOS app or an
      # APK loads.
      #
      # `aarch64-android` is the exception: a cross derivation's `system` is its
      # BUILD platform, so that key is pinned to the builder's canonical one
      # (x86_64-linux) and a Mac cannot realise it. `module.legacyPackages`,
      # which `module //` above passes through untouched, is the same artifact
      # keyed by the build platform instead:
      #   nix build .#legacyPackages.aarch64-darwin.mobile.aarch64-android.bare
      packages = mergedPackages
        // nixpkgs.lib.genAttrs mobileTargets (t: module.packages.${t});
      devShells = mergedDevShells;
    };
}
