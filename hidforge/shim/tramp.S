/*
 * Trampoline buffer of the Detour class. It holds relocated instructions that
 * are executed, so it lives in a code section, as the upstream build places
 * it in .text.
 */
	.section .text.detour_trampolines, "ax", @progbits
	.globl DetourTrampolineBuffer
	.type DetourTrampolineBuffer, @object
	.balign 8
DetourTrampolineBuffer:
	.space 4000
	.size DetourTrampolineBuffer, 4000
