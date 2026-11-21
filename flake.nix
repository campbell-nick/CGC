{
  description = "cgc - git clone from scratch, in C";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "armv7l-linux"
        "aarch64-darwin"
      ];

      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      version = builtins.head (nixpkgs.lib.splitString "\n" (builtins.readFile ./VERSION));

      mkCgc = pkgs: pkgs.stdenv.mkDerivation {
        pname = "cgc";
        inherit version;
        src = ./.;

        # Plain Makefile project: no configure step.
        dontConfigure = true;

        # Local `make` leaves objects and a binary in the tree; path: flakes copy
        # those in, so force a fresh compile (a foreign binary has the wrong ELF
        # interpreter on NixOS).
        preBuild = ''
          make clean
        '';

        nativeBuildInputs = [ pkgs.pkg-config ];
        buildInputs = [ pkgs.zlib pkgs.openssl ];

        makeFlags = [ "PREFIX=${placeholder "out"}" ];
        installFlags = [ "PREFIX=${placeholder "out"}" ];

        meta = with nixpkgs.lib; {
          description = "A self-contained git clone implementation in C";
          homepage = "https://github.com/campbell-nick/cgc";
          license = licenses.mit;
          platforms = platforms.unix;
          mainProgram = "cgc";
        };
      };
    in
    {
      packages = forAllSystems (pkgs: {
        default = mkCgc pkgs;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            gcc
            gnumake
            pkg-config
            zlib
            openssl
          ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixpkgs-fmt);
    };
}
