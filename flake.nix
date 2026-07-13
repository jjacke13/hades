{
  description = "hades — C++ AI agent harness on the MOOS-IvP architecture";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      # Workspace convention: x86_64-linux + aarch64-linux.
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));

      # Official simplex-chat terminal CLI, autoPatchelf'd for NixOS — nixpkgs packages only the
      # desktop app, and the CLI is a GHC-scale source build we don't want. Runtime dep of the
      # `simplex` front-end (the local `simplex-chat -p 5225` daemon); himalaya precedent, but as
      # a release-binary wrapper. x86_64 only: the Pi runs Debian, where the official aarch64
      # release binary works as-is (no wrapper needed).
      mkSimplexCli = pkgs: pkgs.stdenv.mkDerivation {
        pname = "simplex-chat-cli";
        version = "6.5.6";
        src = pkgs.fetchurl {
          url = "https://github.com/simplex-chat/simplex-chat/releases/download/v6.5.6/simplex-chat-ubuntu-24_04-x86_64";
          sha256 = "1gidv9fldfc0cw4vxx9nlbnchsa2ivazasgrp7xkvv1mplg0mv0a";
        };
        dontUnpack = true;
        nativeBuildInputs = [ pkgs.autoPatchelfHook ];
        buildInputs = [ pkgs.zlib pkgs.openssl pkgs.gmp ];   # + glibc via stdenv (ldd: z, crypto, gmp)
        installPhase = ''
          runHook preInstall
          install -Dm755 $src $out/bin/simplex-chat
          runHook postInstall
        '';
        meta.description = "SimpleX Chat terminal CLI (official release binary, patched for NixOS)";
      };
    in
    {
      # aarch64 static cross build for Raspberry Pi OS Lite (Debian aarch64): musl, fully static
      # -> runs on the Pi with zero deps. `nix build .#hades-aarch64-static` -> result/ (scp + run).
      packages.x86_64-linux.hades-aarch64-static =
        (import nixpkgs { system = "x86_64-linux"; })
          .pkgsCross.aarch64-multiplatform.pkgsStatic.callPackage ./package.nix { };

      # `nix build .#simplex-chat-cli` -> result/bin/simplex-chat (run with -p 5225 for the bot API).
      packages.x86_64-linux.simplex-chat-cli =
        mkSimplexCli (import nixpkgs { system = "x86_64-linux"; });

      devShells = forAllSystems (pkgs: {
        # Plain mkShell (NOT an FHS env): FHS sandboxes the network namespace,
        # which would interfere with tool subprocesses / sockets — use the host
        # environment (same rationale as the triton MOOS-IvP shell).
        default = pkgs.mkShell {
          name = "hades-dev";

          # Tools (on PATH).
          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            clang-tools   # clangd for editor LSP (compiler itself = stdenv gcc14, C++20)
            gdb
            git
            jq
            nodejs_22     # `npx @modelcontextprotocol/server-*` MCP servers for adapter testing
            himalaya      # CLI email client — runtime dep of the `email` skill (not linked; on PATH for `nix develop`)
          ] ++ nixpkgs.lib.optionals pkgs.stdenv.isx86_64 [
            (mkSimplexCli pkgs)   # simplex-chat CLI daemon — runtime dep of the `simplex` front-end
          ];

          # Libraries (headers + .pc / cmake config for pkg-config / find_package).
          buildInputs = with pkgs; [
            libcpr         # C++ Requests (cpr) — HTTPS to the Anthropic API (libcurl-backed, SSE streaming)
            curl           # libcurl + curl CLI (cpr links it; explicit for debugging)
            nlohmann_json  # JSON — LLM messages + MCP JSON-RPC (NOT the manifest; that's plain-text)
            httplib        # cpp-httplib: header-only HTTP server — the `--serve` front-end module
            libedit        # BSD-3 line editing (readline-compat API) — REPL; replaced GPL-3 GNU readline for the MIT release
            gtest          # GoogleTest — TDD
          ];
          # Manifest is a plain-text MOOS-style block format parsed by hades itself
          # (the .moos GetConfiguration idiom) — no TOML/JSON config dependency.

          shellHook = ''
            echo "hades dev shell  |  C++20  |  $(cmake --version | head -1)  |  $(g++ --version | head -1)"
          '';
        };
      });
    };
}
