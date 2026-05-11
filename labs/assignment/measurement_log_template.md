# Part A measurement log template

Fill this in with your group's measured values and screenshots before putting
the results into the final report.

## Message

- Callsign: VK2AAL
- Grid: QF56
- Power: 3 dBm
- Command used: `./wspr_transmit -w -v VK2AAL QF56 3`
- First 32 WSPR symbols: 3 3 2 0 0 0 0 2 1 2 0 0 1 1 3 2 2 0 3 2 2 3 0 3 3 3 1 2 2 0 0 0

## CLK2 frequency checks

| Tone | Expected frequency (Hz) | Measured frequency (Hz) | Error (Hz) |
|---:|---:|---:|---:|
| 0 | 7040100.000000 | | |
| 1 | 7040101.464844 | | |
| 2 | 7040102.929688 | | |
| 3 | 7040104.394531 | | |

## RF loopback level

- Loopback parts fitted:
- Receiver input node used:
- Measured receiver input Vpp:
- Vrms:
- dBm into 50 ohms:
- Scope screenshot filename:

Use:

```text
Vrms = Vpp / (2 sqrt(2))
P = Vrms^2 / 50
dBm = 10 log10(P / 1 mW)
```

## Decode result

Paste the decoder line here:

```text

```

## PCB photo

- Photo filename:
- Notes:
