{
  description = "hades — C++ AI agent harness on the MOOS-IvP architecture";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      # Workspace convention: x86_64-linux + aarch64-linux.
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
    in
    {
      # aarch64 static cross build for Raspberry Pi OS Lite (Debian aarch64): musl, fully static
      # -> runs on the Pi with zero deps. `nix build .#hades-aarch64-static` -> result/ (scp + run).
      packages.x86_64-linux.hades-aarch64-static =
        (import nixpkgs { system = "x86_64-linux"; })
          .pkgsCross.aarch64-multiplatform.pkgsStatic.callPackage ./package.nix { };

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
          ];

          # Libraries (headers + .pc / cmake config for pkg-config / find_package).
          buildInputs = with pkgs; [
            libcpr         # C++ Requests (cpr) — HTTPS to the Anthropic API (libcurl-backed, SSE streaming)
            curl           # libcurl + curl CLI (cpr links it; explicit for debugging)
            nlohmann_json  # JSON — LLM messages + MCP JSON-RPC (NOT the manifest; that's plain-text)
            httplib        # cpp-httplib: header-only HTTP server — the `--serve` front-end module
            readline       # GNU readline — line editing in the interactive chat REPL (GPL-3)
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
