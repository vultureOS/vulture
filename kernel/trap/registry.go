package trap

var trapHandlers = [256]TrapHandler{}

type TrapHandler func()

// handler
func Handler(no int) TrapHandler {
	return trapHandlers[no]
}

/* register */
func Register(idx int, handler func()) {
	trapHandlers[idx] = handler
}
