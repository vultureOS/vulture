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
	r.tf.SetRet(v)
}

func (r *Request) Ret() uintptr {
	return r.tf.Ret()
}

func (r *Request) SetErrorNo(errno syscall.Errno) {
	r.SetRet(Errno(errno))
}

func Errno(code syscall.Errno) uintptr {
	return uintptr(-code)
}

func (r *Request) Done() {
	wakeup(&r.Lock, 1)
}

func Error(err error) uintptr {
	if err == nil {
		return 0
	}

	if code, ok := err.(syscall.Errno); ok {
		return Errno(code)
	}

	ret := uintptr(syscall.EINVAL)
	return -ret
}
