{
  description = "RPI Pico library that implements the EDBUS protocol";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs, ... }:
    let
      pkgsForSystem = system: import nixpkgs { inherit system; };
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forEachSystem = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forEachSystem (
        system:
        let
          pkgs = pkgsForSystem system;
        in
        rec {
          pico-edbus = pkgs.stdenv.mkDerivation {
            name = "pico-edbus";
            src = pkgs.lib.cleanSource ./.;

            nativeBuildInputs = with pkgs; [
              cmake
              gcc-arm-embedded
              python313
            ];
            cmakeFlags = [
              "-DCMAKE_C_COMPILER=${pkgs.gcc-arm-embedded}/bin/arm-none-eabi-gcc"
              "-DCMAKE_CXX_COMPILER=${pkgs.gcc-arm-embedded}/bin/arm-none-eabi-g++"
            ];
            env.PICO_SDK_PATH = "${pkgs.pico-sdk}/lib/pico-sdk";
          };
          default = pico-edbus;
        }
      );
      devShells = forEachSystem (
        system:
        let
          pkgs = pkgsForSystem system;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [
              self.packages.${system}.pico-edbus
            ];
            PICO_SDK_PATH = "${pkgs.pico-sdk}/lib/pico-sdk";
          };
        }
      );
    };
}
