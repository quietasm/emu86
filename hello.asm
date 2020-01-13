	use16
	org	100h

	mov	dx, _hello
	mov	ah, 9
	int	21h

	mov	ah, 8
	int	21h

	ret

_hello		db "Hello, World!",13,10,'$'