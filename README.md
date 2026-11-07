# htc-paper-tracker-compat

Use the **HTC VIVE Facial Tracker** with **PaperTracker** natively, without installing SRanipal.

The bridge in [`bridge/`](bridge/) does three things:
- activates the tracker directly over its UVC extension unit;
- captures its IR stream with Media Foundation;
- feeds the stream to PaperTracker as a PaperTracker wireless face tracker, over UDP on localhost.

It is a single native Windows executable. It needs no drivers and does not modify PaperTracker.

See [`bridge/README.md`](bridge/README.md) for how it works, installation, settings and
troubleshooting.

```powershell
cd bridge
powershell -ExecutionPolicy Bypass -File .\install.ps1
```
