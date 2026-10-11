Third-party components vendored in source/thirdparty/
=====================================================

stb_vorbis.c — OGG Vorbis decoder
  by Sean Barrett — public domain / MIT (choose either).
  Compiled with STB_VORBIS_NO_STDIO and STB_VORBIS_NO_PUSHDATA_API.

dr_mp3.h — MP3 decoder
  by David Reid (mackron) — public domain / MIT-0 (choose either).
  Compiled with DR_MP3_NO_STDIO.

Both were taken from NuclearRT's vendored copies
(runtime/source/libs) and are re-licensed here under the project's
AGPL-3.0 terms together with their original permissive terms, which
permit redistribution.  Decoder bugs go upstream; integration glue
lives in source/audio_decode.c.
