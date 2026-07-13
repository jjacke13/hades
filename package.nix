# package.nix — hades as a Nix derivation, for the aarch64 static cross build.
#
# Built via pkgsCross.aarch64-multiplatform.pkgsStatic (musl, fully static) so the result runs on a
# bare Debian aarch64 (Raspberry Pi OS Lite) with zero deps: scp the whole $out and run ./bin/hades.
# Only the SHIPPED binaries are built (not hades_tests → no static gtest link); the native test suite
# gates in the dev shell instead (doCheck = false).
{ stdenv, cmake, ninja, pkg-config, libcpr, nlohmann_json, httplib, libedit, gtest }:
let
  bins = [
    "hades" "hades-scope"
    "hades-fs-read" "hades-write-file" "hades-list-dir" "hades-shell" "hades-http-fetch"
    "hades-save-memory" "hades-core-memory" "hades-use-skill" "hades-save-skill" "hades-ask-agent"
    "hades-grep" "hades-glob" "hades-edit-file" "hades-git-read" "hades-run-command"
    "hades-schedule-task" "hades-list-tasks" "hades-cancel-task"
  ];
in
stdenv.mkDerivation {
  pname = "hades";
  version = "0.1.0";
  src = ./.;

  nativeBuildInputs = [ cmake ninja pkg-config ];
  # gtest is only needed for find_package at CONFIGURE time (tests are not built).
  buildInputs = [ libcpr nlohmann_json httplib libedit gtest ];

  cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];
  doCheck = false;

  # Build ONLY the shipped targets (ninja resolves hades_core + tool deps; skips hades_tests).
  ninjaFlags = bins;

  # $out IS the deploy dir: binaries in bin/, runtime files at the root, a Pi manifest at pi.hades.
  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/tools
    for b in ${toString bins}; do install -Dm755 "$b" "$out/bin/$b"; done
    cp -r $src/web $src/prompts $out/
    cp -r $src/skills $out/ 2>/dev/null || true   # curated skills (e.g. email/); wire with Module=skills + Skills{dir=skills}
    cp $src/tools/*.sh $out/tools/ 2>/dev/null || true
    cp $src/tools/*.py $out/tools/ 2>/dev/null || true
    cp $src/manifests/pi.hades $out/pi.hades
    runHook postInstall
  '';

  meta.description = "hades AI-agent harness — aarch64 static build for Raspberry Pi OS Lite";
}
