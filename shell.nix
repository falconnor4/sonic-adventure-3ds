# the host tools the builds run, for nix-shell: GNU make, Python and a C++
# compiler for the 3DS target's host steps, clang for make pc's native slice,
# patchelf for the prebuilt CIA tools, ImageMagick for the banner, ffmpeg for the CIA's movies, and
# Azahar to run the 3DS image.
# devkitARM comes from make devkitarm
{ pkgs ? import <nixpkgs> { } }:
pkgs.mkShell {
  packages = with pkgs; [ gnumake python3 gcc clang llvm binutils curl patchelf imagemagick ffmpeg azahar ];
}
