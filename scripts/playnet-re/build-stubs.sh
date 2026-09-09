#!/bin/bash
set -e
CC=$(ls /build/openwrt/staging_dir/toolchain-mips_24kc_*/bin/mips-openwrt-linux-gcc | head -1)
OD=$(ls /build/openwrt/staging_dir/toolchain-mips_24kc_*/bin/mips-openwrt-linux-objdump | head -1)
echo "CC=$CC"
mkdir -p /tmp/stubs && cd /tmp/stubs && rm -f *.so.* *.c

gen() {
  local soname=$1; shift
  local c="s_${soname}.c"; : > "$c"
  for s in "$@"; do echo "void $s(void){}" >> "$c"; done
  $CC -shared -fPIC -nostdlib -Wl,-soname,"$soname" -o "$soname" "$c"
  printf '%-22s %s | exported=%s\n' "$soname" "$("$OD" -f "$soname" 2>/dev/null | awk -F, '/format/{print $1}' | awk '{print $NF}')" "$("$OD" -T "$soname" 2>/dev/null | grep -c 'g    DF')"
}

gen libfdk-aac.so.0 aacDecoder_Close aacDecoder_DecodeFrame aacDecoder_Fill aacDecoder_GetStreamInfo aacDecoder_Open aacDecoder_SetParam
gen libFLAC.so.8 FLAC__stream_decoder_delete FLAC__stream_decoder_get_state FLAC__stream_decoder_init_ogg_stream FLAC__stream_decoder_init_stream FLAC__stream_decoder_new FLAC__stream_decoder_process_single
gen libmad.so.0 mad_bit_read mad_bit_skip mad_frame_decode mad_frame_finish mad_frame_init mad_stream_buffer mad_stream_finish mad_stream_init mad_stream_skip mad_stream_sync mad_synth_frame mad_synth_init
gen libvorbisidec.so.1 ov_clear ov_comment ov_info ov_open_callbacks ov_read

echo "--- built stubs ---"; ls -la /tmp/stubs/*.so.*
tar -czf /tmp/stubs.tgz *.so.* && echo "packed /tmp/stubs.tgz"
