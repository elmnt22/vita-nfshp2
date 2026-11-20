# SDL2 native Windows input dependency

Version: **2.32.10**, official SDL2 Visual C++ development release.

Source archive: <https://www.libsdl.org/release/SDL2-devel-2.32.10-VC.zip>

Archive SHA-256: `af347939395a58b365846aaea27391e69f9ec9d4dd650d6ac40802159b418a6e`

The unmodified official headers, x64 `SDL2.dll`, and license are included here.
`PROVENANCE.json` records their individual hashes. The solution copies the DLL
and license next to the executable during an ordinary build; no separate SDL
installation, package manager, setup script, or SDL main library is needed.

`src/joysticke.cpp` loads that adjacent DLL and resolves function pointers whose
types come directly from the included SDL2 declarations. SDL provides actual
controller/joystick enumeration, names, standard controller mappings, raw axes,
buttons and hats, hotplug state, and vibration. The executable retains its own
Win32 window, keyboard input, message loop, focus handling and entry point.

Device selection uses the reported GUID and serial number, or the reported
physical device path when a serial number is unavailable. An explicit saved
choice is not replaced by another controller when disconnected. Simultaneously
observed duplicate factory serials are disambiguated by distinct physical paths,
or by connection-only identities if paths cannot distinguish them. Ambiguous
saved identities are rejected; they never mean the first enumerated controller.
Physical-path aliases keep a previously disambiguated selection valid when only
one member of the pair is present on a later run. Drivers that
provide no usable physical identity, including legacy `XInput#N` slot paths,
receive a connection-only identity and require explicit reselection after that
connection ends. Automatic selection remains available and allocates different
free devices to the two players. A changed USB port can change a path-based
identity when the device does not expose a serial number.

The same catalog and binding rules serve launcher capture and live gameplay.
An optional user-supplied `gamecontrollerdb.txt` beside the executable is read
through SDL2's actual mapping loader. The built-in official mappings work
without that optional file. Unknown joysticks can be mapped through their raw
physical controls in the launcher.

The original game's native action scanner and replay data format are retained
after input translation. No PS2 pad module, IOP code, console device transport,
or console CPU execution is used by this backend.
