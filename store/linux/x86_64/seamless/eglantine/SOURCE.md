# Eglantine for Linux x86_64

Official upstream pre-release: `build-92` (`37908985af9d080bc4795c16c8e49c7ef1d2a98b`)

Download: https://github.com/Djihads80/Eglantine/releases/download/build-92/eglantine-37908985af9d080bc4795c16c8e49c7ef1d2a98b-linux-x86_64.AppImage

Upstream AppImage SHA-256: `c2977f0330ed5279737e90cf3bc07dc376d53bfd7356ab965bbbf8e367555266`

Store package SHA-256: `610a4bc207dd9d71abf2e665237021bdc2eb2be2d2fe7cd5ede0e1e6da05e700`

Entry: `eden-cli`

Entry SHA-256: `810ccecdb3643f11276709233e1d7ee329b134396f1fafe69d5e97088cf915f8`

The package contains the unmodified `eden-cli` executable and its required shared
libraries extracted from the official AppImage. CATui launches the SDL frontend
directly because the Seamless capture does not need Eglantine's Qt library UI.

The icon is Eglantine's upstream `dist/icon_variations/original_small.svg` asset.

Requires glibc 2.39 or newer and the Linux CATui Seamless backend. Package
integrity and installation validation do not establish game compatibility.
