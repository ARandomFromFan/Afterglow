{
  description = "Afterglow: a native port of The Duskbloods made by static recompilation";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      lib = pkgs.lib;
      python = pkgs.python3.withPackages (p: [ p.lz4 ]);
      toolchain = [ pkgs.clang pkgs.lld pkgs.pkg-config python pkgs.librsvg ];   # librsvg: build.py renders the window icon
      gameLibs = [ pkgs.SDL2 pkgs.sdl3 pkgs.ffmpeg pkgs.libopus pkgs.openssl ];   # sdl3: loaded first at run time (window icon)
      buildEnv = {
        VULKAN_HEADERS_DIR = "${pkgs.vulkan-headers}";
        SPIRV_HEADERS_DIR = "${pkgs.spirv-headers}";
        VMA_DIR = "${pkgs.vulkan-memory-allocator}";
        VULKAN_LOADER_DIR = "${pkgs.vulkan-loader}";
        HOS_GCC_DIR = "${pkgs.gcc.cc}";   # libatomic.a for build.py
        PKG_CONFIG_PATH = lib.makeSearchPath "lib/pkgconfig" (map lib.getDev gameLibs);
      };

      afterglow = pkgs.stdenv.mkDerivation {
        pname = "afterglow";
        version = "0-unstable-${self.shortRev or "dirty"}";
        src = self;
        nativeBuildInputs = toolchain ++ [ pkgs.makeWrapper ];
        env = buildEnv;
        dontConfigure = true;
        dontPatchELF = true;   # keeps the vulkan-loader runpath: the renderer dlopens libvulkan, so shrink-rpath drops it

        # The renderer and the lifter's translator library build from source alone; the game is lifted
        # from the user's own dump at run time, so no game code enters the store.
        buildPhase = ''
          runHook preBuild
          WORK_DIR=$PWD/work python3 scripts/build_vk.py -j $NIX_BUILD_CORES
          python3 -c 'import sys; sys.path[:0] = ["third_party/a64-kit/lift"]; import build_emit; build_emit.build()'
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          share=$out/share/afterglow
          mkdir -p $share $out/lib/afterglow $out/bin
          cp -r scripts lifter host third_party branding $share/
          cp work/afterglow_vk.so $out/lib/afterglow/
          substitute ${./scripts/afterglow.sh} $out/bin/afterglow \
            --subst-var-by share $share --subst-var-by lib $out/lib/afterglow
          chmod +x $out/bin/afterglow
          wrapProgram $out/bin/afterglow --prefix PATH : ${lib.makeBinPath toolchain} \
            ${lib.concatStringsSep " " (lib.mapAttrsToList (k: v: "--set-default ${k} ${lib.escapeShellArg v}") buildEnv)} \
            --set PYTHONDONTWRITEBYTECODE 1
          runHook postInstall
        '';

        meta = {
          description = "Native port of The Duskbloods by static recompilation (bring your own game dump)";
          license = lib.licenses.gpl3Plus;
          platforms = [ system ];
          mainProgram = "afterglow";
        };
      };
    in
    {
      packages.${system}.default = afterglow;

      devShells.${system}.default = pkgs.mkShell {
        packages = toolchain ++ gameLibs ++ [ pkgs.vulkan-tools ];
        env = buildEnv;
      };
    };
}
