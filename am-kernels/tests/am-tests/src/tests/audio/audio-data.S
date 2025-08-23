.section .data
.global audio_payload, audio_payload_end
.p2align 3
audio_payload:
.incbin "src/tests/audio/output.wav"
audio_payload_end:
