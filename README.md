# InputTrace backend

InputTrace is a native Windows keyboard and mouse activity collector. This first stage records minute aggregates, physical key press and release durations, modifier shortcuts, mouse clicks, scrolling, and sampled cursor movement, with best-effort foreground application attribution. It does not record typed characters or window titles.

## Build

Requirements: Windows 10/11 x64, CMake 3.25+, Ninja, Python 3, and the supplied `D:\llvm-mingw` toolchain. The provided project directory contains `dependencies/qt-src-6.12.0` and `dependencies/sqlite-src-3.54`; these third-party trees are ignored by Git. The official SQLite 3.54 amalgamation is downloaded because generating it from the canonical source requires Tcl. The expected SHA3-256 digest is `7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93`.

From PowerShell in the project root:

```powershell
Invoke-WebRequest 'https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip' -OutFile dependencies\sqlite-amalgamation-3540000.zip
python -c "import hashlib,pathlib; print(hashlib.sha3_256(pathlib.Path('dependencies/sqlite-amalgamation-3540000.zip').read_bytes()).hexdigest())"
Expand-Archive dependencies\sqlite-amalgamation-3540000.zip dependencies\sqlite-amalgamation
$env:PATH = 'D:\llvm-mingw\bin;C:\path\to\python;' + $env:PATH
$env:CC = 'D:\llvm-mingw\bin\clang.exe'
$env:CXX = 'D:\llvm-mingw\bin\clang++.exe'
New-Item -ItemType Directory -Path build\qt -Force | Out-Null
Push-Location build\qt
& E:\input-trace\dependencies\qt-src-6.12.0\configure.bat -submodules qtbase -static -release -no-gui -no-opengl -nomake examples -nomake tests -prefix E:\input-trace\dependencies\qt-install
Pop-Location
cmake --build build\qt --parallel 8
cmake --install build\qt
cmake -S . -B build\app -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=D:/llvm-mingw/bin/clang.exe -DCMAKE_CXX_COMPILER=D:/llvm-mingw/bin/clang++.exe -DQt6_ROOT=E:/input-trace/dependencies/qt-install
cmake --build build\app --parallel 8
```

The executable is `build\app\inputtrace.exe`. QtCore and SQLite are statically linked. The verified binary imports Windows system DLLs and the Universal C Runtime, with no Qt or LLVM-MinGW DLL dependency. No Qt platform plugin is needed because this is a QtCore console program.

## Run

```powershell
build\app\inputtrace.exe --db E:\input-trace\build\inputtrace.db
build\app\inputtrace.exe --db E:\input-trace\build\inputtrace.db --duration 30 --stats-every 5
build\app\inputtrace.exe --db E:\input-trace\build\inputtrace.db --query
build\app\inputtrace.exe --db E:\input-trace\build\selftest.db --self-test
ctest --test-dir build\app --output-on-failure
```

Press Ctrl+C to stop and flush. `--retain-days 90` is the default; set 0 to disable pruning. `--flush-ms` defaults to 2000 and accepts 100–10000. `--benchmark PACKETS` replays a repeated key/mouse workload through aggregation, and `--storage-benchmark ROWS` measures SQLite growth with distinct spatial cells. These modes create or append to the chosen database. The self-test feeds deterministic keyboard and mouse transitions through the processing and database paths. It does not prove system-wide OS capture. The process must run in an interactive Windows session for Raw Input; Windows security boundaries and secure desktops may restrict capture.

See [architecture](docs/architecture.md), [database](docs/database.md), and [performance](docs/performance.md).
