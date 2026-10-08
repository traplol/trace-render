# Native symbol fixture

`fixture.c` is original TraceRender test code. `fixture.exe` and `fixture.pdb` were built from it with Ubuntu clang 18.1.3 and lld 18.1.3 on Linux, without a Windows SDK, CRT, or Windows machine. Tests read these files; they do not execute the EXE. The PDB is 72 KiB and contains full function and line information.

The recorded PDB identity is `5C6542D2-6C0D-3468-4C4C-44205044422E/1`. PE RVA `0x1005` is `allocate_buffer` at line 3; `0x100f` is the same function at line 4; `0x1025` is `release_buffer` at line 8. `0x1019` lies in padding between functions and must remain unresolved.

`regenerate.sh` records the compiler/linker invocation. Set `LLD_LINK` if the COFF linker is not named `lld-link-18`. Regeneration can change the PDB GUID because the linker records local paths. Update the expected GUID in the test after regeneration. Automated tests use the checked-in files and need no compiler, linker, network connection, or public symbol archive.
