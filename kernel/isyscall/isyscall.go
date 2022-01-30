package isyscall

import "syscall"

const (
	EPANIC syscall.Errno = 0xfffff
)

var (
	handlers [512]Handler
)

func wakeup(lock *uintptr, n int)

type Handler func(req *Request)

type Request struct {
	tf   *trapFrame
	Lock uintptr
}

func (r *Request) NO() uintptr {
	return r.tf.NO()
}

func (r *Request) Arg(n int) uintptr {
	return r.tf.Arg(n)
}

func (r *Request) SetRet(v uintptr) {
	return r.tf.Ret(v)
}

func (r *Request) Ret() uintptr {
	return r.tf.Ret()
}
