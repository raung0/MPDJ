{
  description = "My flake";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };

        mpd-local = pkgs.mpd.overrideAttrs (old: {
          src = ./.;

          buildInputs = old.buildInputs ++ [
            pkgs.rubberband
            pkgs.libkeyfinder
            pkgs.aubio
          ];
        });
      in
      {
        packages.default = mpd-local;
      }
    );
}
