# OrientationMonitor

Windows service that monitors device orientation and sends changes to the codec audio driver over IOCTL

## IOCTL

```
GUID:  422BC88E-1437-44DD-98A3-B248781447B6
Code:  CTL_CODE(0x1212, 0x122, METHOD_BUFFERED, FILE_ANY_ACCESS)
Input: OrientationState (4 bytes)
```

## Usage

In terminal:

```bash
OrientationMonitor.exe test
```

Cycles through all four orientations with 1 second delay, sending each to the driver

Or just install it using INF
