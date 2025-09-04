{
  description = "LKP WS24 Project";

  nixConfig = {
    extra-substituters = [
      "https://attic.jungkamp.dev/pjungkamp-lkp"
    ];

    extra-trusted-public-keys = [
      "pjungkamp-lkp:x4tr7c1OR6bZokeDVotd7An5IZJccrAVLiHDd2rfFhE="
    ];
  };

  inputs = {
    flake-parts.url = "github:hercules-ci/flake-parts";
    flake-recipes.url = "github:pjungkamp/flake-recipes";
    git-hooks.url = "github:cachix/git-hooks.nix";
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } (flake: {
      imports = [
        inputs.flake-recipes.flakeModules.default
        inputs.git-hooks.flakeModule
      ];

      systems = [ "x86_64-linux" ];

      perSystem =
        perSystem@{
          config,
          pkgs,
          system,
          self',
          ...
        }:
        {
          _module.args.pkgs = import inputs.nixpkgs {
            inherit system;
            overlays = builtins.attrValues flake.self.overlays;
          };

          formatter = pkgs.nixfmt-rfc-style;

          devShells.default = pkgs.stdenv.mkDerivation {
            name = "linux-lkp";

            hardeningDisable = [
              "pic"
              "format"
            ];

            MAKEFLAGS = [
              "KERNELDIR=${pkgs.linux_lkp.dev}/lib/modules/${pkgs.linux_lkp.modDirVersion}/build"
            ];

            nativeBuildInputs = pkgs.linux_lkp.nativeBuildInputs ++ [
              pkgs.bear
              pkgs.clang-tools
              pkgs.gdb
              pkgs.ncurses
              pkgs.qemu
              (pkgs.python3.withPackages (pythonPkgs: [
                pythonPkgs.ply
                pythonPkgs.gitpython
              ]))
            ];
          };

          pre-commit = {
            check.enable = true;

            settings.hooks = {
              clang-format.enable = true;
              editorconfig-checker.enable = true;
              editorconfig-checker.entry = "${pkgs.editorconfig-checker}/bin/editorconfig-checker -disable-indentation";
              nixfmt-rfc-style.enable = true;
            };
          };

          packages = rec {
            default = pkgs.ouichefs;
            setup-git-hooks = pkgs.writeShellScriptBin "setup-git-hooks" config.pre-commit.installationScript;
          };

          checks = {
            nixos = pkgs.testers.runNixOSTest {
              name = "ouichefs";

              nodes.machine = {
                boot.kernelPackages = pkgs.linuxPackages_lkp;
                boot.extraModulePackages = [ self'.packages.default ];
                boot.kernelParams = [ "ouichefs.dyndbg" ];
                boot.consoleLogLevel = inputs.nixpkgs.lib.mkForce 8;
                environment.systemPackages = [ self'.packages.default ];
                system.stateVersion = "24.11";
              };

              testScript =
                { nodes, ... }:
                ''
                  machine.wait_for_unit("default.target")

                  machine.succeed("modprobe ouichefs")
                  machine.succeed("dd if=/dev/zero of=test.img bs=1M count=75 && mkfs.ouichefs test.img >&2")
                  machine.succeed("mkdir mnt && mount test.img ./mnt")

                  machine.succeed("touch mnt/file && [ -f mnt/file ]")
                  machine.succeed("mkdir -p mnt/test/deep/dir && [ -d mnt/test/deep/dir ]")
                  snap_create_find = machine.succeed("find mnt")
                  snap_create_stat = machine.succeed("stat -f mnt")
                  machine.succeed("echo 0 > /sys/fs/ouichefs/loop0/create")
                  machine.succeed("cat /sys/fs/ouichefs/loop0/list >&2")

                  nr_snapshots = 5
                  for i in range(1, nr_snapshots):
                    machine.succeed(f"echo test{i} >> mnt/file")
                    machine.succeed(f"echo {i} > /sys/fs/ouichefs/loop0/create")

                  for i in range(1, nr_snapshots):
                    machine.succeed(f"echo {i} > /sys/fs/ouichefs/loop0/restore")
                    machine.succeed(f'cat mnt/file >&2 && [ "$(wc -l < mnt/file)" -eq {i} ]')

                  for i in range(1, nr_snapshots):
                    machine.succeed(f"echo {i} > /sys/fs/ouichefs/loop0/destroy")

                  machine.succeed("echo 0 > /sys/fs/ouichefs/loop0/restore")
                  machine.succeed("echo 0 > /sys/fs/ouichefs/loop0/destroy")
                  snap_restore_find = machine.succeed("find mnt")
                  snap_restore_stat = machine.succeed("stat -f mnt")
                  machine.succeed("cat /sys/fs/ouichefs/loop0/list >&2")

                  assert snap_create_stat == snap_restore_stat, f"leaked inodes or blocks\n\nbefore:\n{snap_create_stat}\n\nafter:\n{snap_restore_stat}"
                  assert snap_create_find == snap_restore_find, "failed to restore filesystem layout"

                  machine.succeed("umount mnt")
                  machine.succeed("modprobe -r ouichefs")
                '';
            };
          };
        };

      recipes.overlay.enable = true;

      flake.recipes = {
        linux_lkp =
          args@{ fetchurl, buildLinux, ... }:
          buildLinux (
            args
            // rec {
              version = "6.5.7";
              modDirVersion = version;

              src = fetchurl {
                url = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.5.7.tar.xz";
                hash = "sha256-DQnqRIAFyc/lOD5Mcqhys5GIuSj4xE4UawOxt4Ufu4w=";
              };

              extraMeta.branch = "6.5";
            }
          );

        linuxPackages_lkp =
          {
            recurseIntoAttrs,
            linuxPackagesFor,
            linux_lkp,
          }:
          recurseIntoAttrs (linuxPackagesFor linux_lkp);

        ouichefs =
          {
            stdenv,
            lib,
            linuxPackages_lkp,
            kernel ? linuxPackages_lkp.kernel,
            ...
          }:
          stdenv.mkDerivation (finalAttrs: {
            name = "ouichefs";
            src = ./.;

            hardeningDisable = [
              "pic"
              "format"
            ];

            makeFlags = [
              "prefix=${placeholder "out"}"
              "KERNELDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
              "INSTALL_MOD_PATH=${placeholder "out"}"
            ];

            meta = {
              mainProgram = "mkfs.ouichefs";
            };
          });
      };
    });
}
