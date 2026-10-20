# Install BG

These instructions build BG from a local copy of this repository. Run the
commands from the repository directory. You need a C++23 compiler, CMake 3.31
or later, and Git available. Configuration downloads build dependencies.

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

You also need Python 3.13 or later. Installation builds the Python extension.

Create a virtual environment:

```sh
python -m venv .venv
```

Activate it on Linux or macOS:

```sh
source .venv/bin/activate
```

On Windows, activate it in PowerShell:

```powershell
.venv\Scripts\Activate.ps1
```

Install BG into that environment:

```sh
python -m pip install .
```
:::

:::{tab-item} C++
:sync: cpp

Configure a release build with tests disabled, then build and install it:

```sh
cmake -S . -B build/cpp -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build/cpp --config Release
cmake --install build/cpp --config Release
```

These commands work in a Linux/macOS shell or Windows PowerShell. They install
BG and its C++ dependencies into the repository's `install` directory.
Python is not required for this build.

The next lesson uses a small CMake project to find this installation and link
to `bg::bg`.
:::
::::

Continue to [Your first graph](getting-started.md).
