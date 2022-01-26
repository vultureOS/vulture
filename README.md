# vulture
- A Unix Operating System Built Using Golang

[![forthebadge](https://forthebadge.com/images/badges/made-with-go.svg)](https://forthebadge.com)
[![forthebadge](https://forthebadge.com/images/badges/built-by-developers.svg)](https://forthebadge.com)

[![Go Build Status](https://github.com/vultureOS/vulture/actions/workflows/go.yml/badge.svg?branch=main)](https://github.com/vultureOS/vulture/actions/workflows/go.yml)

## Requirements:
- macOS:
- make sure you have go1.16 version
```bash
# update brew
$ brew update
$ brew doctor 

# to build some multiboot headers
$ brew install x86_64-elf-binutils x86_64-elf-gcc x86_64-elf-gdb qemu

# mage require for building os
$ go get github.com/magefile/mage
```

- linux:
```
$ sudo apt-get update
$ sudo apt-get install bulid-essential qemu
$ go get github.com/magefile/mage
```

## Building:
```
$ mage qemu
```

## Author:
- [krishpranav](https://github.com/krishpranav)

## License:
- vultureOS is licensed under [GPL-3.0 License](https://github.com/vultureOS/vulture/blob/main/LICENSE)