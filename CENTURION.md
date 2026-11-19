# Fuzix-Bintools for Centurion CPU6

This branch is based on Alan Cox's Fuzix-Bintools commit
`ca2ab54a4599f60dba7ca9882956178f070e7e13`, preserved on `main`.
[Upstream repository](https://codeberg.org/EtchedPixels/Fuzix-Bintools)

- The EE200 assembler adds CPU6 `MUL` and `DIV` with word-register,
  immediate, direct-memory and indexed-memory operands. It also supports
  the CPU6 conditional branch and byte operations used by the generated code.
- The linker preserves full-word big-endian fixups when producing CENTOS CBIN
  records and checks the compatible output format. The distribution enables
  the upstream archive-rescan facility for the target runtime.

See [LICENCE](LICENCE), [COPYRIGHT.MWC](COPYRIGHT.MWC) and file headers for
the upstream BSD and GPL licensing terms.
