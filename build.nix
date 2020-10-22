let
  sources = import ./nix/sources.nix {};
  pkgs    = import sources.nixpkgs {};
  hmmer   = pkgs.callPackage ./default.nix {};
in
  hmmer
