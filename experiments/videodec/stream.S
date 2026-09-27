// the test stream (Annex B H.264), linked into the kernel image
	.section .rodata
	.balign	16
	.global	test_stream
test_stream:
	.incbin	"test720.h264"
	.global	test_stream_end
test_stream_end:
