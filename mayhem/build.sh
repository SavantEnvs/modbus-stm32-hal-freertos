#!/usr/bin/env bash
#
# mayhem/build.sh — build the Modbus fuzz harnesses and the known-answer selftests.
#
# MODBUS-LIB is STM32 HAL + FreeRTOS firmware, so it is compiled unmodified against the host shims in
# mayhem/shim (FreeRTOS/CMSIS-RTOS/HAL/lwIP stand-ins; the slave/master task loops run on a worker
# thread). Two variants: RTU over a UART (default) and Modbus TCP over lwIP (-DMB_SHIM_TCP).
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${STANDALONE_FUZZ_MAIN:=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

INC="-I$SRC/mayhem/shim -I$SRC/MODBUS-LIB/Inc -I$SRC/mayhem"
SRCS="$SRC/MODBUS-LIB/Src/Modbus.c $SRC/MODBUS-LIB/Src/UARTCallback.c $SRC/mayhem/shim/rtos_shim.c"
OBJ=/tmp/modbus-build
rm -rf "$OBJ"
mkdir -p "$OBJ"

# $1 = fuzz|sanitized|plain, $2 = rtu|tcp -> $OBJ/$1-$2/*.o. The fuzz flavor carries SanCov
# instrumentation so libFuzzer sees coverage inside the library, not only in the harness.
compile_lib() {
  local flavor="$1" variant="$2" flags def="" f
  case "$flavor" in
    fuzz)      flags="-O1 $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link" ;;
    sanitized) flags="-O1 $SANITIZER_FLAGS $DEBUG_FLAGS" ;;
    *)         flags="-O2 $COVERAGE_FLAGS" ;;
  esac
  [ "$variant" = tcp ] && def="-DMB_SHIM_TCP"
  mkdir -p "$OBJ/$flavor-$variant"
  for f in $SRCS; do
    $CC $flags $def $INC -c "$f" -o "$OBJ/$flavor-$variant/$(basename "${f%.c}").o" &
  done
  wait
}

# 1) the library + shims with the sanitizers and DWARF 3, then each harness twice (libFuzzer, standalone)
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$OBJ/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OBJ/standalone_main.o"

for variant in rtu tcp; do
  compile_lib fuzz "$variant"
  compile_lib sanitized "$variant"
  def=""; [ "$variant" = tcp ] && def="-DMB_SHIM_TCP"
  for role in slave master; do
    name="fuzz_${role}_${variant}"
    common="-O1 $SANITIZER_FLAGS $DEBUG_FLAGS $def $INC"
    objs() { echo "$OBJ/$1-$variant/Modbus.o $OBJ/$1-$variant/UARTCallback.o $OBJ/$1-$variant/rtos_shim.o $OBJ/lsan_off.o"; }
    $CC $common $LIB_FUZZING_ENGINE "$SRC/mayhem/fuzz_$role.c" $(objs fuzz) -lpthread -o "/mayhem/$name" &
    $CC $common "$SRC/mayhem/fuzz_$role.c" "$OBJ/standalone_main.o" $(objs sanitized) -lpthread -o "/mayhem/$name-standalone" &
  done
done
wait

# 2) the known-answer selftests (mayhem/test.sh runs these) with the project's normal flags
for variant in rtu tcp; do
  compile_lib plain "$variant"
  def=""; [ "$variant" = tcp ] && def="-DMB_SHIM_TCP"
  $CC -O2 $COVERAGE_FLAGS $def $INC "$SRC/mayhem/selftest.c" \
      "$OBJ/plain-$variant/Modbus.o" "$OBJ/plain-$variant/UARTCallback.o" "$OBJ/plain-$variant/rtos_shim.o" \
      -lpthread -o "/mayhem/selftest_$variant" &
done
wait

ls -l /mayhem/fuzz_* /mayhem/selftest_*
