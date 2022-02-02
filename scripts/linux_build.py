#!/usr/bin/env python3

import os
import time

def InstallAptRequirements():
    print('Install Requirements')
    time.sleep(1)
    os.system('sudo apt-get update')
    time.sleep(1)
    os.system('sudo apt-get install bulid-essential qemu')
    time.sleep(0.5)
    print('Installed Requirements')

def InstallGoRequirements():
    time.sleep(1)
    print('Install Go Requirements')
    os.system('go get -u github.com/magefile/mage')
    time.sleep(1)
    print('Export GOPATH')
    os.system('export GOPATH=$HOME/go')
    os.system('export PATH=$PATH:$GOROOT/bin:$GOPATH/bin')

def main():
    InstallAptRequirements()
    InstallGoRequirements()
    os.system('clear')
    print('ALL REQUIREMENTS HAS BEEN INSTALLED!!!')
    print('github: https://github.com/vultureOS/vulture [make sure to give it a start]')
    print('run: mage qemu')

if __name__ == "__main__":
    main()