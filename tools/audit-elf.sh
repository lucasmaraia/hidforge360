#!/bin/bash
# Audits a linked hidforge ELF (the `image` next to hidforge.xex in buck-out).
#
#   tools/audit-elf.sh <ELF> [llvm-objdump]
#
# Fails (exit 1) when:
#   - there is any vector or floating point instruction: vector code inside the
#     USB context hung the console, so BUCK builds with -mno-altivec -mno-vmx128
#     -mno-vsx -fno-vectorize -fno-slp-vectorize and this proves it held;
#   - a function outside the reviewed list saves the condition register (mfcr).
#     mfcr in a prologue means the function keeps conditions in cr2-cr4, and
#     cr2-cr4 are not trusted across kernel or XAM calls on this console (see
#     docs/conhecimento.md). A new function on that list needs a manual review
#     of every cr2-cr4 read against the calls before it, then an entry here.
# It also prints the cr2-cr4 traffic and the calls of the two hot functions,
# interruptHandler and XamInputGetStateHook, for that review.
set -u

ELF=$(readlink -f "$1")
OBJ=${2:-${LLVM_OBJDUMP:-}}
if [ -z "$OBJ" ]; then
	XF=${XENON_FORGE_DIR:-$(cd "$(dirname "$0")/.." && pwd)/.xenon-forge}
	OBJ=$(ls "$XF"/buck-out/v2/art/toolchains/*/__ppc64-prebuilt-linux-x86_64__/*/bin/llvm-objdump 2>/dev/null | head -1)
fi
[ -x "$OBJ" ] || { echo "audit: llvm-objdump not found (pass it as the second argument)"; exit 2; }

# Functions reviewed by hand (see docs/desenvolvimento.md, "Auditoria do ELF").
ALLOWED_MFCR="_Z20initFunctionPointersv _ZN6Detour7InstallEv _Z16interruptHandlermi _Z20XamInputGetStateHookmmP13_XINPUT_STATE"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$OBJ" -d --no-show-raw-insn "$ELF" > "$TMP/dis"
awk -F'\t' '/^[0-9a-f]+:/ {print $2}' "$TMP/dis" > "$TMP/mnem"

status=0
echo "ELF: $ELF ($(stat -c %s "$ELF") bytes)"
echo "instructions: $(wc -l < "$TMP/mnem") (sanity: addi $(grep -cE '^addi' "$TMP/mnem"))"
vec=$(grep -cE '^(v|lv|stv|lf|stf|f[acdimnrs]|mtvscr|mfvscr|dcbz128)' "$TMP/mnem")
echo "vector/FP: $vec"
[ "$vec" -eq 0 ] || { echo "FAIL: vector or floating point instructions"; status=1; }
[ "$(wc -l < "$TMP/mnem")" -gt 1000 ] || { echo "FAIL: suspiciously small disassembly"; status=1; }

echo "functions with mfcr:"
for fn in $(awk '/^[0-9a-f]+ </ {fn=$2} /\tmfcr/ {print fn}' "$TMP/dis" | tr -d '<>:' | sort -u); do
	case " $ALLOWED_MFCR " in
	*" $fn "*) echo "  $fn (reviewed)" ;;
	*) echo "  $fn  <-- NOT REVIEWED"; status=1 ;;
	esac
done

for sym in _Z16interruptHandlermi _Z20XamInputGetStateHookmmP13_XINPUT_STATE; do
	n=$(grep -nE "^[0-9a-f]+ <$sym>" "$TMP/dis" | cut -d: -f1)
	[ -n "$n" ] || continue
	echo "=== $sym: calls and cr2-cr4 ==="
	sed -n "${n},/^\$/p" "$TMP/dis" | grep -nE 'bctrl|bl |mfcr|crset|crclr|crmove|cror|crand|crnot|b[tf]	(8|9|10|11|12|13|14|15|16|17|18|19),' || true
done

[ $status -eq 0 ] && echo "audit: OK" || echo "audit: FAILED"
exit $status
