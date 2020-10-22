let
  sources = import ./nix/sources.nix {};
  pkgs    = import sources.nixpkgs {};
  easel   = pkgs.callPackage sources.easel {};
  hmmer   = pkgs.callPackage ./default.nix { inherit easel; };
in
  hmmer
