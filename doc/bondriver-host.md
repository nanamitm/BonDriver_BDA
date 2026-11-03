# BonDriverHost: running the graph outside the application

Some drivers stop Windows when the process that owns a BDA graph is terminated
while it is receiving. Digital Devices boards such as the MAX M4 do this with bug
check `0x18 REFERENCE_BY_POINTER`: a system thread of the driver uses an object
that a forced termination has already released.

A terminated process runs no more code, so no change inside a BonDriver can stop
the graph first. `BonDriverHost.exe` therefore owns the graph, and the application
only loads a proxy. When the application disappears, the host still stops the
graph in the normal order and exits.

This is optional per BonDriver. A BonDriver deployed the usual way is unaffected,
and the BonDriver and BDASpecial DLLs themselves are the same in both layouts.

## Deployment

Rename `BonDriverProxy.dll` to the name the application should see, and put the
real BonDriver under the same name in a `BonDriverHost` subfolder:

```
BonDriver_MaxM4.dll                (BonDriverProxy.dll renamed)
BonDriverHost\BonDriverHost.exe
BonDriverHost\BonDriver_MaxM4.dll  (BonDriver_BDA.dll renamed)
BonDriverHost\BonDriver_MaxM4.ini
BonDriverHost\DD.dll
```

The subfolder keeps the real BonDriver out of the application's list, where it
could be opened directly and without this protection. The INI file and the
BDASpecial DLL are found next to the real BonDriver, as always. Channel files such
as `.ch2` stay where the application expects them.

The application's BonDriver setting needs no change. One host process runs for
each BonDriver instance.

## What is and is not covered

| Event | Result |
| --- | --- |
| Application exits, crashes, or is ended with `taskkill /F` | Host stops the graph |
| `taskkill /F /T` or "End process tree" on the application | Host stops the graph; it is not part of the tree |
| Application in a kill-on-close job that allows breakaway | Host stops the graph; it left the job |
| Application in a job that forbids breakaway | **Not covered**: the job ends the host too |
| `BonDriverHost.exe` itself is terminated | **Not covered**: the original failure |
| Logoff or shutdown | Host stops the graph on `WM_ENDSESSION` |

Do not end `BonDriverHost.exe` directly. To recover from a hung host, restart Windows.

## How it works

- The proxy creates two named pipes with unguessable names and starts the host.
  The host is started through an intermediate process that exits at once, so it has
  no living parent, and it asks to break away from any job.
- The control pipe carries the IBonDriver2 calls one at a time. The data pipe carries
  TS chunks, so a slow `SetChannel` does not hold up the stream.
- A broken pipe is how the host learns that the application is gone. Nothing polls.
- The host closes the tuner and releases the BonDriver, then exits. It never
  terminates itself or another host, even if closing hangs.
- While a host in that state is closing, it counts itself in a named semaphore.
  `OpenTuner` waits for that count to reach zero, for at most 10 seconds, because
  the tuner's exclusive lock is still held. This wait covers hosts of the same
  Windows session only.

## Behavior differences

- Up to 64 TS chunks are queued in the proxy, on top of the BonDriver's own buffer.
  When the application reads too slowly, the BonDriver's buffer overflows as before.
- Chunks read while `SetChannel` runs but before the driver purges its buffer are
  delivered, as they would be to an application reading during `SetChannel`.
- The process priority of the application does not apply to the host. The thread
  priorities in the INI file do, since they are set by the BonDriver.
- An application that loads the BonDriver and never calls `Release` keeps the proxy
  loaded until it exits.

## Testing

`tools\test-bon-driver-host.cmd` runs the forwarding, purge, reopen, termination,
process tree and job cases against a mock BonDriver. It does not touch a tuner, so
it cannot show that a real driver survives.

To check real hardware, end the application during reception with `taskkill /F`,
and accept that a defect here stops Windows, so close your work first. Then read
what the driver logged:

```powershell
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='DDTuner'} |
  Sort-Object TimeCreated |
  ForEach-Object { "{0:HH:mm:ss} {1,4} {2}" -f $_.TimeCreated, $_.Id, ($_.Message -split "`n")[0] }
```

The sequence must end in `Stop` and `Release`, and opening the tuner again must not
fail with `0x800700AA`.
