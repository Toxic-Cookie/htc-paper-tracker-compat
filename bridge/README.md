# VIVE Facial Tracker → PaperTracker bridge

Runs the HTC VIVE Facial Tracker (USB `0BB4:0321`, "HTC Multimedia Camera") as a face camera for
[PaperTracker](https://www.papertracker.cn), **without SRanipal**. It is a ~290 KB native program with
no dependencies beyond Windows itself, and it needs no drivers. PaperTracker is not modified.

## How it works

```
VIVE Facial Tracker --UVC (stock Windows driver)--> bridge --UDP 127.0.0.1:45454--> PaperTracker
```

1. **Activation.** It sends the tracker's activation sequence over its UVC extension unit
   (unit 4, selector 2, GUID `2CCB0BDA-6331-4FDB-850E-79054DBD5671`) through `IKsControl`:
   - the magic handshake;
   - stream off;
   - sensor/IR registers;
   - stream on.

   This is the job SRanipal normally does.
2. **Capture.** It captures the native `YUY2 400x400 @ 60 fps` stream with Media Foundation. That
   stream is really an 800x400 8-bit IR stereo pair. The bridge crops one 400x400 view (`--view`),
   optionally rotates, flips, applies gamma or scales it, and JPEG-encodes it with WIC.
3. **Delivery.** It sends the frames to PaperTracker the way PaperTracker's own wireless face
   tracker does: the 16-byte `PT\x01` UDP header, video type, face channel 0, fragmented JPEG. To
   PaperTracker it simply looks like a Wi-Fi face tracker at `127.0.0.1`.
4. **Gating.** The tracker is only activated while PaperTracker answers the `PTRACKER_PROBE_V1`
   handshake. When PaperTracker closes, the bridge stops the stream and switches the IR
   illuminator off.

## Install (background at logon)

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

The script does three things:
- builds the bridge if `out\` is empty (needs Visual Studio with C++);
- copies it to `%LOCALAPPDATA%\Programs\vft-papertracker-bridge`;
- registers a per-user logon task named **VFT PaperTracker Bridge**, then starts it.

It needs no admin rights. Re-run it to update; your `.cfg` is kept.
To remove everything, run `uninstall.ps1`.

Remove SRanipal (or at least make sure it is not running): only one program can own the camera.

## Settings

Edit `%LOCALAPPDATA%\Programs\vft-papertracker-bridge\vft-papertracker-bridge.cfg`. It accepts the
same options as the command line. Then restart the bridge:

```powershell
& "$env:LOCALAPPDATA\Programs\vft-papertracker-bridge\vft-papertracker-bridge.exe" --stop
Start-ScheduledTask -TaskName 'VFT PaperTracker Bridge'
```

| Option | Default | Meaning |
|---|---|---|
| `--view left\|right` | `left` | Camera of the stereo pair to forward |
| `--rotate 0\|90\|180\|270` | `0` | Clockwise rotation |
| `--flip-h`, `--flip-v` | off | Mirror (PaperTracker's own flip buttons work too) |
| `--gamma G` | `1.0` | >1 brightens |
| `--ir N` | `255` | IR illuminator level 0..255 |
| `--size N` | `400` | Output resolution NxN (64..400) |
| `--quality N` | `90` | JPEG quality |
| `--fps N` | `60` | Max frames/s sent |
| `--host IP --port N` | `127.0.0.1 45454` | PaperTracker address |
| `--verbose` | off | Also log frame-rate statistics |

## Console tool

`vft-papertracker-bridge.exe` is the same bridge with a status window. It also provides these
diagnostics:

- `--list`: list cameras; the tracker is marked `[VFT]`.
- `--dump 10 C:\some\dir`: activate the tracker and save 10 raw 800x400 frames (`.pgm`) plus the
  processed JPEGs.
- `--stop`: ask the running (background) bridge to switch the tracker off and exit.

The background log is `%LOCALAPPDATA%\vft-papertracker-bridge\bridge.log`.

## Troubleshooting

- **"could not open camera"**: another app holds the tracker (SRanipal, VRCFT's SRanipal module,
  Baballonia), or camera access for desktop apps is off in Windows *Settings → Privacy → Camera*.
- **"extension unit not found"**: this is not a working VIVE Facial Tracker. If the device shows
  up as "HTC Boot", it is stuck in its bootloader; SRanipal's firmware updater is the only known fix.
- **Black or dark image**: try `--gamma 1.5`. Make sure nothing covers the tracker's IR LEDs.
- **No image in PaperTracker**: check that *Stream* is enabled for the face tracker on
  PaperTracker's home page and that no face-tracker IP is pinned in PaperTracker.
  The bridge log should say `PaperTracker is running` and `Capture started`.

## Credits

The tracker protocol was reverse-engineered by
[LordOfDragons/vivefacialtracker](https://github.com/LordOfDragons/vivefacialtracker) and Project
Babble ([BabbleApp](https://github.com/Project-Babble/ProjectBabble),
[Baballonia](https://github.com/Project-Babble/Baballonia)). Further details came from
[Unvived](https://github.com/OptoCloud/Unvived),
[SRanipatch](https://github.com/SteffanDonal/SRanipatch) and
[BITFOXDEN](https://github.com/BITFOXDEN/Driver.SteamFrame.ViveFacialTracker).
The PaperTracker UDP protocol was derived from the PaperTracker 7.4.0 client, which is MIT-licensed.
