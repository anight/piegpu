// the test stream (Annex B H.264), linked into the kernel image
	.section .rodata
	.balign	16
	.global	test_stream
test_stream:
	.incbin	STREAM_FILE
	.global	test_stream_end
test_stream_end:
