.text
.p2align 4
.globl PipelineBinaryAbiProbe
#ifdef _WIN32
.def PipelineBinaryAbiProbe; .scl 2; .type 32; .endef
.seh_proc PipelineBinaryAbiProbe
PipelineBinaryAbiProbe:
    subq $312, %rsp
.seh_stackalloc 312
    movaps %xmm6, 64(%rsp)
.seh_savexmm %xmm6, 64
    movaps %xmm7, 80(%rsp)
.seh_savexmm %xmm7, 80
    movaps %xmm8, 96(%rsp)
.seh_savexmm %xmm8, 96
    movaps %xmm9, 112(%rsp)
.seh_savexmm %xmm9, 112
    movaps %xmm10, 128(%rsp)
.seh_savexmm %xmm10, 128
    movaps %xmm11, 144(%rsp)
.seh_savexmm %xmm11, 144
    movaps %xmm12, 160(%rsp)
.seh_savexmm %xmm12, 160
    movaps %xmm13, 176(%rsp)
.seh_savexmm %xmm13, 176
    movaps %xmm14, 192(%rsp)
.seh_savexmm %xmm14, 192
    movaps %xmm15, 208(%rsp)
.seh_savexmm %xmm15, 208
    movq %rbx, 224(%rsp)
.seh_savereg %rbx, 224
    movq %rbp, 232(%rsp)
.seh_savereg %rbp, 232
    movq %rdi, 240(%rsp)
.seh_savereg %rdi, 240
    movq %rsi, 248(%rsp)
.seh_savereg %rsi, 248
    movq %r12, 256(%rsp)
.seh_savereg %r12, 256
    movq %r13, 264(%rsp)
.seh_savereg %r13, 264
    movq %r14, 272(%rsp)
.seh_savereg %r14, 272
    movq %r15, 280(%rsp)
.seh_savereg %r15, 280
.seh_endprologue
    movq %rcx, 288(%rsp)
    movq %rdx, 296(%rsp)
    stmxcsr 304(%rsp)
    fnstcw 308(%rsp)
    movq 64(%rcx), %rax
    movabsq $0x1234567887654321, %rbx
    xorq %rax, %rbx
    movabsq $0x1335577988664422, %rbp
    xorq %rax, %rbp
    movabsq $0x1436587a89674523, %r12
    xorq %rax, %r12
    movabsq $0x1537597b8a684624, %r13
    xorq %rax, %r13
    movabsq $0x16385a7c8b694725, %r14
    xorq %rax, %r14
    movabsq $0x17395b7d8c6a4826, %r15
    xorq %rax, %r15
    movabsq $0x183a5c7e8d6b4927, %rdi
    xorq %rax, %rdi
    movabsq $0x193b5d7f8e6c4a28, %rsi
    xorq %rax, %rsi
    movq %rax, %xmm0
    punpcklqdq %xmm0, %xmm0
    movdqu .LprobeSeed0(%rip), %xmm6
    pxor %xmm0, %xmm6
    movdqu .LprobeSeed1(%rip), %xmm7
    pxor %xmm0, %xmm7
    movdqu .LprobeSeed2(%rip), %xmm8
    pxor %xmm0, %xmm8
    movdqu .LprobeSeed3(%rip), %xmm9
    pxor %xmm0, %xmm9
    movdqu .LprobeSeed4(%rip), %xmm10
    pxor %xmm0, %xmm10
    movdqu .LprobeSeed5(%rip), %xmm11
    pxor %xmm0, %xmm11
    movdqu .LprobeSeed6(%rip), %xmm12
    pxor %xmm0, %xmm12
    movdqu .LprobeSeed7(%rip), %xmm13
    pxor %xmm0, %xmm13
    movdqu .LprobeSeed8(%rip), %xmm14
    pxor %xmm0, %xmm14
    movdqu .LprobeSeed9(%rip), %xmm15
    pxor %xmm0, %xmm15
    movq 296(%rsp), %r11
    movq %rbx, 0(%r11)
    movq %rbp, 8(%r11)
    movq %r12, 16(%r11)
    movq %r13, 24(%r11)
    movq %r14, 32(%r11)
    movq %r15, 40(%r11)
    movq %rdi, 48(%r11)
    movq %rsi, 56(%r11)
    movdqu %xmm6, 64(%r11)
    movdqu %xmm7, 80(%r11)
    movdqu %xmm8, 96(%r11)
    movdqu %xmm9, 112(%r11)
    movdqu %xmm10, 128(%r11)
    movdqu %xmm11, 144(%r11)
    movdqu %xmm12, 160(%r11)
    movdqu %xmm13, 176(%r11)
    movdqu %xmm14, 192(%r11)
    movdqu %xmm15, 208(%r11)
    stmxcsr 224(%r11)
    fnstcw 228(%r11)
    pushfq
    popq %rax
    shrq $10, %rax
    andl $1, %eax
    movw %ax, 230(%r11)
    movq 288(%rsp), %r10
    movq 40(%r10), %rax
    movq %rax, 32(%rsp)
    movq 48(%r10), %rax
    movq %rax, 40(%rsp)
    movq 56(%r10), %rax
    movq %rax, 48(%rsp)
    movq 8(%r10), %rcx
    movq 16(%r10), %rdx
    movq 24(%r10), %r8
    movq 32(%r10), %r9
    movq 0(%r10), %r10
    call *%r10
    movq 296(%rsp), %r11
    movq %rax, 464(%r11)
    movq %rbx, 232(%r11)
    movq %rbp, 240(%r11)
    movq %r12, 248(%r11)
    movq %r13, 256(%r11)
    movq %r14, 264(%r11)
    movq %r15, 272(%r11)
    movq %rdi, 280(%r11)
    movq %rsi, 288(%r11)
    movdqu %xmm6, 296(%r11)
    movdqu %xmm7, 312(%r11)
    movdqu %xmm8, 328(%r11)
    movdqu %xmm9, 344(%r11)
    movdqu %xmm10, 360(%r11)
    movdqu %xmm11, 376(%r11)
    movdqu %xmm12, 392(%r11)
    movdqu %xmm13, 408(%r11)
    movdqu %xmm14, 424(%r11)
    movdqu %xmm15, 440(%r11)
    stmxcsr 456(%r11)
    fnstcw 460(%r11)
    pushfq
    popq %rax
    shrq $10, %rax
    andl $1, %eax
    movw %ax, 462(%r11)
    cld
    ldmxcsr 304(%rsp)
    fldcw 308(%rsp)
    movaps 64(%rsp), %xmm6
    movaps 80(%rsp), %xmm7
    movaps 96(%rsp), %xmm8
    movaps 112(%rsp), %xmm9
    movaps 128(%rsp), %xmm10
    movaps 144(%rsp), %xmm11
    movaps 160(%rsp), %xmm12
    movaps 176(%rsp), %xmm13
    movaps 192(%rsp), %xmm14
    movaps 208(%rsp), %xmm15
    movq 224(%rsp), %rbx
    movq 232(%rsp), %rbp
    movq 240(%rsp), %rdi
    movq 248(%rsp), %rsi
    movq 256(%rsp), %r12
    movq 264(%rsp), %r13
    movq 272(%rsp), %r14
    movq 280(%rsp), %r15
    addq $312, %rsp
    ret
.seh_endproc
#else
.type PipelineBinaryAbiProbe, @function
PipelineBinaryAbiProbe:
.cfi_startproc
    endbr64
    subq $88, %rsp
.cfi_def_cfa_offset 96
    movq %rbx, 16(%rsp)
.cfi_offset %rbx, -80
    movq %rbp, 24(%rsp)
.cfi_offset %rbp, -72
    movq %r12, 32(%rsp)
.cfi_offset %r12, -64
    movq %r13, 40(%rsp)
.cfi_offset %r13, -56
    movq %r14, 48(%rsp)
.cfi_offset %r14, -48
    movq %r15, 56(%rsp)
.cfi_offset %r15, -40
    movq %rdi, 64(%rsp)
    movq %rsi, 72(%rsp)
    stmxcsr 80(%rsp)
    fnstcw 84(%rsp)
    movq 64(%rdi), %rax
    movabsq $0x1234567887654321, %rbx
    xorq %rax, %rbx
    movabsq $0x1335577988664422, %rbp
    xorq %rax, %rbp
    movabsq $0x1436587a89674523, %r12
    xorq %rax, %r12
    movabsq $0x1537597b8a684624, %r13
    xorq %rax, %r13
    movabsq $0x16385a7c8b694725, %r14
    xorq %rax, %r14
    movabsq $0x17395b7d8c6a4826, %r15
    xorq %rax, %r15
    movq %rsi, %r11
    movq %rbx, 0(%r11)
    movq %rbp, 8(%r11)
    movq %r12, 16(%r11)
    movq %r13, 24(%r11)
    movq %r14, 32(%r11)
    movq %r15, 40(%r11)
    stmxcsr 224(%r11)
    fnstcw 228(%r11)
    pushfq
.cfi_adjust_cfa_offset 8
    popq %rax
.cfi_adjust_cfa_offset -8
    shrq $10, %rax
    andl $1, %eax
    movw %ax, 230(%r11)
    movq %rdi, %r10
    movq 56(%r10), %rax
    movq %rax, 0(%rsp)
    movq 8(%r10), %rdi
    movq 16(%r10), %rsi
    movq 24(%r10), %rdx
    movq 32(%r10), %rcx
    movq 40(%r10), %r8
    movq 48(%r10), %r9
    movq 0(%r10), %r10
    call *%r10
    movq 72(%rsp), %r11
    movq %rax, 464(%r11)
    movq %rbx, 232(%r11)
    movq %rbp, 240(%r11)
    movq %r12, 248(%r11)
    movq %r13, 256(%r11)
    movq %r14, 264(%r11)
    movq %r15, 272(%r11)
    stmxcsr 456(%r11)
    fnstcw 460(%r11)
    pushfq
.cfi_adjust_cfa_offset 8
    popq %rax
.cfi_adjust_cfa_offset -8
    shrq $10, %rax
    andl $1, %eax
    movw %ax, 462(%r11)
    cld
    ldmxcsr 80(%rsp)
    fldcw 84(%rsp)
    movq 16(%rsp), %rbx
.cfi_restore %rbx
    movq 24(%rsp), %rbp
.cfi_restore %rbp
    movq 32(%rsp), %r12
.cfi_restore %r12
    movq 40(%rsp), %r13
.cfi_restore %r13
    movq 48(%rsp), %r14
.cfi_restore %r14
    movq 56(%rsp), %r15
.cfi_restore %r15
    addq $88, %rsp
.cfi_def_cfa_offset 8
    ret
.cfi_endproc
.size PipelineBinaryAbiProbe, .-PipelineBinaryAbiProbe
#endif
.p2align 4
.globl PipelineBinaryAbiConforming
#ifdef _WIN32
.def PipelineBinaryAbiConforming; .scl 2; .type 32; .endef
.seh_proc PipelineBinaryAbiConforming
PipelineBinaryAbiConforming:
.seh_endprologue
    cmpq $1, %rcx
    jne .LbadConforming
    cmpq $2, %rdx
    jne .LbadConforming
    cmpq $3, %r8
    jne .LbadConforming
    cmpq $4, %r9
    jne .LbadConforming
    cmpq $5, 40(%rsp)
    jne .LbadConforming
    movq 48(%rsp), %rax
    movl $0x5a, (%rax)
    xorl %eax, %eax
    ret
.LbadConforming:
    movl $-13, %eax
    ret
.seh_endproc
#else
.type PipelineBinaryAbiConforming, @function
PipelineBinaryAbiConforming:
.cfi_startproc
    endbr64
    cmpq $1, %rdi
    jne .LbadConforming
    cmpq $2, %rsi
    jne .LbadConforming
    cmpq $3, %rdx
    jne .LbadConforming
    cmpq $4, %rcx
    jne .LbadConforming
    cmpq $5, %r8
    jne .LbadConforming
    movl $0x5a, (%r9)
    xorl %eax, %eax
    ret
.LbadConforming:
    movl $-13, %eax
    ret
.cfi_endproc
.size PipelineBinaryAbiConforming, .-PipelineBinaryAbiConforming
#endif
.p2align 4
.globl PipelineBinaryAbiClobbering
#ifdef _WIN32
.def PipelineBinaryAbiClobbering; .scl 2; .type 32; .endef
.seh_proc PipelineBinaryAbiClobbering
PipelineBinaryAbiClobbering:
.seh_endprologue
    cmpq $1, %rcx
    jne .LbadClobbering
    cmpq $2, %rdx
    jne .LbadClobbering
    cmpq $3, %r8
    jne .LbadClobbering
    cmpq $4, %r9
    jne .LbadClobbering
    cmpq $5, 40(%rsp)
    jne .LbadClobbering
    movq 48(%rsp), %rax
    movl $0x5a, (%rax)
    movabsq $0x1234567800000000, %rbx
    movabsq $0x1335577900000000, %rbp
    movabsq $0x1436587a00000000, %r12
    movabsq $0x1537597b00000000, %r13
    movabsq $0x16385a7c00000000, %r14
    movabsq $0x17395b7d00000000, %r15
    movabsq $0x183a5c7e00000000, %rdi
    movabsq $0x193b5d7f00000000, %rsi
    pcmpeqd %xmm6, %xmm6
    pcmpeqd %xmm7, %xmm7
    pcmpeqd %xmm8, %xmm8
    pcmpeqd %xmm9, %xmm9
    pcmpeqd %xmm10, %xmm10
    pcmpeqd %xmm11, %xmm11
    pcmpeqd %xmm12, %xmm12
    pcmpeqd %xmm13, %xmm13
    pcmpeqd %xmm14, %xmm14
    pcmpeqd %xmm15, %xmm15
    stmxcsr 8(%rsp)
    orl $0x8040, 8(%rsp)
    ldmxcsr 8(%rsp)
    fnstcw 16(%rsp)
    xorw $0x0c00, 16(%rsp)
    fldcw 16(%rsp)
    std
    xorl %eax, %eax
    ret
.LbadClobbering:
    movl $-13, %eax
    ret
.seh_endproc
#else
.type PipelineBinaryAbiClobbering, @function
PipelineBinaryAbiClobbering:
.cfi_startproc
    endbr64
    cmpq $1, %rdi
    jne .LbadClobbering
    cmpq $2, %rsi
    jne .LbadClobbering
    cmpq $3, %rdx
    jne .LbadClobbering
    cmpq $4, %rcx
    jne .LbadClobbering
    cmpq $5, %r8
    jne .LbadClobbering
    movl $0x5a, (%r9)
    movabsq $0x1234567800000000, %rbx
    movabsq $0x1335577900000000, %rbp
    movabsq $0x1436587a00000000, %r12
    movabsq $0x1537597b00000000, %r13
    movabsq $0x16385a7c00000000, %r14
    movabsq $0x17395b7d00000000, %r15
    stmxcsr -8(%rsp)
    orl $0x8040, -8(%rsp)
    ldmxcsr -8(%rsp)
    fnstcw -4(%rsp)
    xorw $0x0c00, -4(%rsp)
    fldcw -4(%rsp)
    std
    xorl %eax, %eax
    ret
.LbadClobbering:
    movl $-13, %eax
    ret
.cfi_endproc
.size PipelineBinaryAbiClobbering, .-PipelineBinaryAbiClobbering
#endif
.p2align 4
.globl PipelineBinaryAbiClobberingKey
#ifdef _WIN32
.def PipelineBinaryAbiClobberingKey; .scl 2; .type 32; .endef
.seh_proc PipelineBinaryAbiClobberingKey
PipelineBinaryAbiClobberingKey:
.seh_endprologue
    movl $1, 16(%r8)
    movb $1, 20(%r8)
    testq %rdx, %rdx
    je .LglobalClobberingKey
    movb $7, 20(%r8)
.LglobalClobberingKey:
    shrq $32, %rbx
    shlq $32, %rbx
    orq $1, %rbx
    shrq $32, %rbp
    shlq $32, %rbp
    orq $1, %rbp
    shrq $32, %rdi
    shlq $32, %rdi
    orq $1, %rdi
    shrq $32, %rsi
    shlq $32, %rsi
    orq $1, %rsi
    shrq $32, %r12
    shlq $32, %r12
    orq $1, %r12
    shrq $32, %r13
    shlq $32, %r13
    orq $1, %r13
    shrq $32, %r14
    shlq $32, %r14
    orq $1, %r14
    shrq $32, %r15
    shlq $32, %r15
    orq $1, %r15
    pcmpeqd %xmm6, %xmm6
    pcmpeqd %xmm7, %xmm7
    pcmpeqd %xmm8, %xmm8
    pcmpeqd %xmm9, %xmm9
    pcmpeqd %xmm10, %xmm10
    pcmpeqd %xmm11, %xmm11
    pcmpeqd %xmm12, %xmm12
    pcmpeqd %xmm13, %xmm13
    pcmpeqd %xmm14, %xmm14
    pcmpeqd %xmm15, %xmm15
    xorl %eax, %eax
    ret
.seh_endproc
#else
.type PipelineBinaryAbiClobberingKey, @function
PipelineBinaryAbiClobberingKey:
.cfi_startproc
    endbr64
    movl $1, 16(%rdx)
    movb $1, 20(%rdx)
    testq %rsi, %rsi
    je .LglobalClobberingKey
    movb $7, 20(%rdx)
.LglobalClobberingKey:
    shrq $32, %rbx
    shlq $32, %rbx
    orq $1, %rbx
    shrq $32, %rbp
    shlq $32, %rbp
    orq $1, %rbp
    shrq $32, %r12
    shlq $32, %r12
    orq $1, %r12
    shrq $32, %r13
    shlq $32, %r13
    orq $1, %r13
    shrq $32, %r14
    shlq $32, %r14
    orq $1, %r14
    shrq $32, %r15
    shlq $32, %r15
    orq $1, %r15
    xorl %eax, %eax
    ret
.cfi_endproc
.size PipelineBinaryAbiClobberingKey, .-PipelineBinaryAbiClobberingKey
#endif
#ifdef _WIN32
.section .rdata,"dr"
.p2align 4
.LprobeSeed0:
    .quad 0x3141592653589793, 0x2718281828459045
.LprobeSeed1:
    .quad 0x32425a2754599894, 0x2819291929469146
.LprobeSeed2:
    .quad 0x33435b28555a9995, 0x291a2a1a2a479247
.LprobeSeed3:
    .quad 0x34445c29565b9a96, 0x2a1b2b1b2b489348
.LprobeSeed4:
    .quad 0x35455d2a575c9b97, 0x2b1c2c1c2c499449
.LprobeSeed5:
    .quad 0x36465e2b585d9c98, 0x2c1d2d1d2d4a954a
.LprobeSeed6:
    .quad 0x37475f2c595e9d99, 0x2d1e2e1e2e4b964b
.LprobeSeed7:
    .quad 0x3848602d5a5f9e9a, 0x2e1f2f1f2f4c974c
.LprobeSeed8:
    .quad 0x3949612e5b609f9b, 0x2f203020304d984d
.LprobeSeed9:
    .quad 0x3a4a622f5c61a09c, 0x30213121314e994e
#else
.section .note.GNU-stack,"",@progbits
#endif
