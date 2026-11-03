# Calibration

How the look gets measured, without anything of Apple's going into this repository: only numbers do.

- `measure.sh` builds `measure.m` against the macOS 15 SDK and runs it on this Mac. AppKit picks its look by the SDK a program is
  linked against, so this draws Sequoia's look for what AppKit decides that way, whatever macOS this is. The rest of what it
  draws, and everything that isn't drawn by AppKit, is whatever this Mac runs. See `Documentation/DesignSystem.md`.
- To go further takes a real Sequoia: a virtual machine, below. `measure-in-vm.sh` runs the same program there.

## A macOS 15 virtual machine

macOS's licence appears to allow virtual machines on Mac hardware for development and testing, but check it for yourself. The
virtual machine runs on this Mac, and stays out of the repository.

1. See if there's room: it takes about 60 GB.

       Meta/Calibration/macos-vm.sh check

2. Download a restore image for macOS 15, about 15 GB, from Apple. They're listed by machine, as `VirtualMac2,1`, on
   `https://api.ipsw.me/v4/device/VirtualMac2,1?type=ipsw`. Then install it, which takes some minutes:

       Meta/Calibration/macos-vm.sh install --ipsw UniversalMac_15.x_Restore.ipsw

3. Start it, and go through Setup in its window: any account will do, with no Apple ID.

       Meta/Calibration/macos-vm.sh run

4. In Terminal, inside it, once:

       sh "/Volumes/My Shared Files/calibration/guest-setup.sh"

   With that, the host can run things in its session:

       Meta/Calibration/in-vm.sh "sw_vers"
       Meta/Calibration/measure-in-vm.sh > measured.json

The virtual machine has to be running and logged in for any of this. `--directory` puts it on another disk, and
`VULTURE_CALIBRATION_VM` says the same for the scripts.
