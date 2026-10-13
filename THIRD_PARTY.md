# Third-party notices

CloudScreenTransfer is distributed under the Mozilla Public License 2.0. It grew out of GDI/cloud-screen transport experiments in the user's libcimbar checkout. New protocol and file-session code use the same MPL-2.0 license; the display codec retains the experimental grayscale geometry and calibration approach.

## libcorrect

The vendored `third_party/libcorrect` source is from [quiet/libcorrect](https://github.com/quiet/libcorrect), copyright (c) 2016 Brian Armstrong. Its BSD 3-clause license is retained in [third_party/libcorrect/LICENSE](third_party/libcorrect/LICENSE). Only the portable Reed–Solomon implementation is compiled. This project does not use libcorrect's SSE convolutional codec.

## OpenCV

The verified 0.1.0 build uses OpenCV 4.12.0 through the `x64-windows-static` vcpkg package. Only `core` and `imgproc` are required by this project. OpenCV and its bundled third-party notices are retained verbatim in [licenses/OpenCV-4.12.0.txt](licenses/OpenCV-4.12.0.txt).

If rebuilding with a different OpenCV version, the packaging script copies the installed package's `share/opencv4/copyright` into the portable distribution. Review licensing and compatibility when changing dependencies.

## Distribution

The portable ZIP includes the project license, libcorrect license, installed OpenCV notices and a source repository reference. No private desktop recordings or captured screenshots are included.
