# Eglantine for Linux x86_64

Official upstream pre-release: `nx2-build-15` (`2818f85a8f55079f687596b743996e34a12b36a7`)

Download: https://github.com/zortos293/Eglantine/releases/download/nx2-build-15/eglantine-nx2-build-15-linux-x86_64.AppImage

Upstream AppImage SHA-256: `75d17a211acff827138c774ba6969c3c6d8600af0690e8439007dbe775afca19`

Store package SHA-256: `d704cb64c834dd2bc01ebe476f4c936cfcc8ef3395a1789ebd9cbd47ea299701`

Entry: `eden-cli`

Entry SHA-256: `45109ae187ea6c2328bf9a6a31b2f77cea0bb77bc9daec439aef4001f71fde14`

The package contains the unmodified `eden-cli` executable and its required shared
libraries extracted from the official AppImage. CATui launches the SDL frontend
directly because the Seamless capture does not need Eglantine's Qt library UI.

The icon is Eglantine's upstream `dist/icon_variations/original_small.svg` asset.

Requires glibc 2.39 or newer and the Linux CATui Seamless backend. Package
integrity and installation validation do not establish game compatibility.
