#!/bin/sh
# Turn a libretro core's static archive into one relocatable object that
# exports exactly one prefixed copy of the libretro API and nothing else.
#
# Cores are not written to coexist: every one of them exports the same
# retro_* names, and their internals collide too (two cores both defining
# `FCEU_free` or `memory_size` is normal). Renaming at the object level
# rather than patching sources means a core can be re-fetched at a newer
# commit without carrying local edits forward.
#
#   blob.sh <prefix> <archive> <output.o> [objcopy] [ld]
set -e
PREFIX="$1"; AR_IN="$2"; OUT="$3"
OBJCOPY="${4:-objcopy}"; LD="${5:-ld}"

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
     retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state
     retro_init retro_deinit retro_api_version retro_get_system_info
     retro_get_system_av_info retro_set_controller_port_device retro_reset
     retro_run retro_serialize_size retro_serialize retro_unserialize
     retro_cheat_reset retro_cheat_set retro_load_game retro_load_game_special
     retro_unload_game retro_get_region retro_get_memory_data
     retro_get_memory_size"

"$LD" -r -o "$OUT.tmp" --whole-archive "$AR_IN"

KEEP=""; REDEF=""
for s in $API; do
    KEEP="$KEEP -G $s"
    REDEF="$REDEF --redefine-sym $s=${PREFIX}_$s"
done

# -G keeps these global and localises everything else, which is what makes
# two cores linkable at once. Done before the rename so the names still match.
"$OBJCOPY" $KEEP "$OUT.tmp"
"$OBJCOPY" $REDEF "$OUT.tmp" "$OUT"
rm -f "$OUT.tmp"
