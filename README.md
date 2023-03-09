# comPyler

A CPython extension that compiles bytecode with LLVM.

*To protect our source code during manuscript review, we temporarily use this repository to showcase the source code. After the manuscript is processed, we will migrate it to our common account for maintenance.*



# How to build it

We show how to build comPyler on the x86_64 Ubuntu Linux platform.

It theoretically works on other platforms, since it's implemented in pure C/C++, and both LLVM and CPython are cross-platform.

## Build LLVM

It is not recommended to use the LLVM libraries from the Ubuntu repository, because they are built without the `-ffunction-sections` and `-fdata-sections` options, which will cause comPyler to link in a lot of unused content, and then the volume can reach hundreds of MiB.

- download and extract

  ```sh
  wget https://github.com/llvm/llvm-project/releases/download/llvmorg-14.0.6/llvm-project-14.0.6.src.tar.xz
  tar -axf llvm-project-14.0.6.src.tar.xz
  ```

- run cmake

  ```sh
  cmake -S ./llvm-project-14.0.6.src/llvm/ -B ./llvm-build/ -DCMAKE_BUILD_TYPE=MinSizeRel \
  	-DLLVM_ENABLE_UNWIND_TABLES=OFF -DLLVM_ENABLE_PEDANTIC=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_ZLIB=OFF \
      -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_ENABLE_RUNTIMES="all"
  ```

  - `-DCMAKE_BUILD_TYPE=MinSizeRel`, **necessary**.  Enable `-ffunction-sections -fdata-sections`
  - `-DLLVM_ENABLE_UNWIND_TABLES=OFF -DLLVM_ENABLE_PEDANTIC=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_ZLIB=OFF`, not very important. Turn off some unnecessary features to make the final product smaller, although the effect is minimal
  - `-DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_ENABLE_RUNTIMES="all"`, optional. We will use Clang to build comPyler. If you don't have Clang on your system, you can build a Clang compiler from the llvm project by the way. Otherwise, these options can be omitted

- run makefile through cmake (it usually takes a long time)

  ```sh
  cmake --build ./llvm-build/ -j $(nproc)
  ```

- install llvm to the `./llvm-install/` directory

  ```sh
  cmake -DCMAKE_INSTALL_PREFIX=./llvm-install/ -P ./llvm-build/cmake_install.cmake
  ```

## Install CPython or build it from source

Although more versions can be adapted through some efforts, currently comPyler only supports Python 3.10. Therefore, we need to get the correct CPython version.

If that works, you can install directly from Ubuntu repository.

```sh
sudo apt install -s python3.10-dev
```

Otherwise, build CPython from source

- download and extract

  ```sh
  wget https://www.python.org/ftp/python/3.10.10/Python-3.10.10.tar.xz
  tar -axf Python-3.10.10.tar.xz
  ```

- build it

  ```sh
  mkdir CPython-build
  cd CPython-build
  ../Python-3.10.10/configure --prefix=$(realpath ../CPython-install/) --enable-optimizations --with-lto
  make -s -j $(nproc)
  make install
  cd -  # go back to the previous directory
  ```

## build comPyler

- clone this repository

  ```sh
  git clone https://github.com/comPylerProject/comPyler
  ```

- run cmake

  ```sh
  cmake -B ./comPyler-build -S ./comPyler -DCMAKE_BUILD_TYPE=Release \
  	-DLLVM_ROOT=./llvm-install \
  	-DCPYTHON_EXE=./CPython-install/bin/python3.10 \
  	-DCMAKE_C_COMPILER=$(pwd)/llvm-install/bin/clang -DCMAKE_CXX_COMPILER=$(pwd)/llvm-install/bin/clang++
  ```

  - `-DLLVM_ROOT=./llvm-install`. Specify the directory where LLVM was installed
  - `-DCPYTHON_EXE=./CPython-install/bin/python3.10`. Specify the path to Python 3.10
  - `-DCMAKE_C_COMPILER=$(pwd)/llvm-install/bin/clang -DCMAKE_CXX_COMPILER=$(pwd)/llvm-install/bin/clang++`.

- run makefile through cmake (it takes some time because PGO is enabled)

  ```sh
  cmake --build ./comPyler-build/ -j $(nproc)
  ```

- done, let's take a look at it

  ```sh
  ls -lh comPyler-build/compyler.*.so
  ```

  It is a monolithic binary, about 15MiB in size. The LLVM library is statically linked, and you can delete the relevant directories for building and installing LLVM if you feel necessary.



# How to use

There are three ways to use LLVM.

- the manual way

  Put the binary in an appropriate path, then add `import compyler` at the beginning of the main Python file.

- the automatic ways  (reference: https://docs.python.org/3/library/site.html)

  - through `usercustomize` 

    Rename `compyler.*.so` to `usercustomize.*.so` (or use a symbolic link), and place it in the path included in `PYTHONPATH`.

  - through `.pth` file

    Put it in the sitepackages directory of Python, and then create a text file named `compyler.pth` in the same directory, with only one line of content: `import compyler`.



Let's take the `usercustomize`-based method as an example.

```sh
mkdir comPyler-install
cp comPyler-build/compyler.*.so comPyler-install/
rename s/compyler/usercustomize/ comPyler-install/compyler.*.so    # rename it
```

Try it out with the nbody benchmark from https://benchmarksgame-team.pages.debian.net/.

```sh
wget -O nbody.py https://raw.githubusercontent.com/southerngs/benchmarksgame/master/bench/nbody/nbody.python3
time 
time PYTHONPATH=comPyler-install ./CPython-install/bin/python3.10 nbody.py 200000
```

Run the benchmark directly with CPython 3.10.

```sh
\time -f %es ./CPython-install/bin/python3.10 nbody.py 500000
```

Run the it with comPyler enabled. (Here we add the `./comPyler-install/` directory to the Python's path by setting the environment variable, and then comPyler will be automatically imported as the usercustomize module.)

```sh
PYTHONPATH=./comPyler-install/ \time -f %es ./CPython-install/bin/python3.10 nbody.py 500000
```



# More settings via environment variables

- `COMPYLER_CACHE_ROOT`

  Set this environment variable to any writable path, and it will cache the compiled code in it. We strongly recommend enabling caching for comPyler.

- `COMPYLER_THRESHOLD_RATIO`

  Sets a floating point number controlling the threshold at which JIT compilation is triggered. For example, with a setting of 0.5, the threshold becomes half of the default, and compilation happens earlier and more frequently.
