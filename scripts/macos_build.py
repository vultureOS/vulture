#!/usr/bin/env python3

import os
import time


def InstallBrewRequirements():
    print('Updating Brew')
    time.sleep(1)
    os.system('brew update')
    print('Brew doctor')
    time.sleep(1)
    os.system('brew doctor')
    print('Install Requirements')
    time.sleep(1)
    os.system('brew install x86_64-elf-binutils x86_64-elf-gcc x86_64-elf-gdb qemu')
    time.sleep(0.6)
    os.system('clear')
    print('BREW DEPENDENCIES INSTALLED SUCCESSFULLY')

def InstallGoRequirements():
    time.sleep(1)
    print('Installing Mage')
    os.system('go get -u github.com/magefile/mage')
    time.sleep(1)
    print('Export GOPATH')
    os.system('export GOPATH=$HOME/go')
    os.system('export PATH=$PATH:$GOROOT/bin:$GOPATH/bin')

def main():
    InstallBrewRequirements()
    InstallGoRequirements()
    time.sleep(1)
    os.system('clear')
    print('ALL REQUIREMENTS HAS BEEN INSTALLED!!!')
    print('github: https://github.com/vultureOS/vulture [make sure to give it a start]')
    print('run: mage qemu')


if __name__ == "__main__":
    main()