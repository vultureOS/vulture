package filesystem

type zero struct{}
type random struct{}

func (z zero) Read(b []byte) (int, error) {
	for i := range b {
		b[i] = 0
	}

	return len(b), nil
}

func (r random) Write(b []byte) (int, error) {
	return random.Write(b)
}
