<!--
# Copyright 2014-2022 Jetperch LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
-->

[![Windows amd64](https://github.com/jetperch/joulescope_driver/actions/workflows/windows_amd64.yml/badge.svg)](https://github.com/jetperch/joulescope_driver/actions/workflows/windows_amd64.yml)
[![macOS amd64](https://github.com/jetperch/joulescope_driver/actions/workflows/macos_amd64.yml/badge.svg)](https://github.com/jetperch/joulescope_driver/actions/workflows/macos_amd64.yml)
[![Ubuntu Linux amd64](https://github.com/jetperch/joulescope_driver/actions/workflows/linux_amd64.yml/badge.svg)](https://github.com/jetperch/joulescope_driver/actions/workflows/linux_amd64.yml)


# Joulescope Driver

Welcome to the Joulescope™ Driver project.
[Joulescope](https://www.joulescope.com) is an affordable, precision DC energy
analyzer that enables you to build better products.

This user-space C library communicates with Joulescope products to configure 
operation and receive data.  The first-generation driver introduced in 2019 was
written in Python.  While Python proved to be a very flexible language enabling
many user scripts, it was difficult to support other languages.  
This second-generation driver launched in 2022 addresses several issues
with the first-generation python driver including:

1. Improved event-driven API based upon PubSub for easier integration with 
   user interfaces and other complicated software packages.
2. Improved portability for easier language bindings.
3. Improved performance.


## Limitations

This project is currently under active development.  As of 2022 Aug 1,
this project has the following known limitations:

1. Only Windows support
   a. Builds under macOS, but libusb_init returns error -99.
   b. Linux support planned soon.
2. JS110 support not yet at feature parity to existing [pyjoulescope](https://github.com/jetperch/pyjoulescope).
   a. No current range suppression filtering yet.
   b. No downsampling. 
4. Limited testing - Still a work in progress.  Not yet ready for production use.


## Building

Ensure that your computer has a develpment environment including CMake.  

For macOS, install homebrew, then:

    brew install libusb pkgconfig python3

Then:

    cd {your/repos/joulescope_driver}
    mkdir build && cd build
    cmake ..
    cmake --build . && ctest .

This package includes a command-line tool, jsdrv_util:

    jsdrv_util --help
    jsdrv_util scan


## Python bindings

The python bindings are made to work with Python 3.9 and later.  To install
the dependencies:

    cd {your/repos/joulescope_driver}
    pip3 install -U requirements.txt

You should then be able to build the native bindings:

    python3 setup.py build_ext --inplace

And run the Python development tools:

    python3 -m pyjoulescope_driver --help
    python3 -m pyjoulescope_driver scan
    python3 -m pyjoulescope_driver ui
