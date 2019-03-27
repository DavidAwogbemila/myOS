;A simple boot sector program to print hexadecimal value of a given 4-byte value.

print_hex:
	pusha
	mov bx, HEX_OUT
	add bx, 5

to_ascii_loop:
	mov ax, dx
	and ax, 0xf

	cmp ax, 10
	jge to_alpha
	add ax, 48
	jmp end_to_ascii_loop

to_alpha:
	add ax, 87

end_to_ascii_loop:
	mov [bx], al
	add bx, -1
	shr dx, 4
	cmp dx, 0
	je exit_print_hex
	jmp to_ascii_loop

exit_print_hex:
	mov bx, HEX_OUT
	call print_string
	popa
	ret

HEX_OUT:
	db "0x0000", 0 

