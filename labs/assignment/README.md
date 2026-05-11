# Part A - WSPR RF transmitter on Si5351 CLK2

## Goal

The software in this folder makes the existing WSPR-SDR board transmit a
standard WSPR type-1 message on the unused Si5351 CLK2 output.  It keeps CLK0
and CLK1 untouched by using PLLB for CLK2, so the receiver local oscillator can
continue to run from the lab configuration.

Required command form:

```sh
./wspr_transmit VK2AAL QF56 3
```

## Build on the AUP-ZU3/PetaLinux board

Install or enable the same I2C userspace support used in Lab 3, then build:

```sh
cd ~/elec3607-lab-main/assignment1/part_a
make
sudo chmod a+rw /dev/i2c-3
./wspr_transmit --probe
./wspr_transmit -s VK2AAL QF56 3
```

Useful options:

```sh
./wspr_transmit -w VK2AAL QF56 3
./wspr_transmit -w -o 0 VK2AAL QF56 3
./wspr_transmit -n -s VK2AAL QF56 3
./wspr_transmit -f 7040100 VK2AAL QF56 3
```

`-w` waits for the next even UTC minute plus a 1.0 s start offset.  This keeps
the RF frame aligned with the Lab 5 `wsprwait`/`pa-wsprcan/k9an-wsprd` capture
window.  Use `-o 0` if you deliberately want to start exactly on the even UTC
minute.
`-n` is a dry run for checking the message and symbol list without touching
I2C.  The default RF tone-0 frequency is 7.040100 MHz, which is the 40 m WSPR
USB dial frequency, 7.038600 MHz, plus a 1500 Hz receive audio offset.

## I2C check using the Lab 3 style

The transmitter uses the same Linux userspace I2C path as `lab3-si5351`:
`/dev/i2c-3`, address `0x60`, and SMBus byte-data register reads/writes.
Before transmitting, run:

```sh
cd ~/elec3607-lab/labs/assignment
sudo chmod a+rw /dev/i2c-3
./wspr_transmit --probe
```

The output should look like the Lab 3 `i2cread` format:

```text
I2C device: /dev/i2c-3, Si5351 address: 0x60
r dev(0x60) reg(0x0)=0x11 (decimal 17)
...
```

If `/dev/i2c-3` is missing or the probe fails, first go back to the Lab 3
checks:

```sh
i2cdetect -l
i2cdetect -y 3
```

## Matching the Lab 5 receiver directly

The Lab 5 receiver records mono 12 kHz audio from PulseAudio for 114 seconds
and then runs `pa-wsprcan/k9an-wsprd`.  The transmitter defaults are chosen to
feed that receiver directly through the RF loopback:

- RF tone 0 is 7.040100 MHz, so the existing 7.038600 MHz receiver setup sees
  the WSPR signal near 1500 Hz audio.
- `-w` starts one second after the even UTC slot, which sits inside the Lab 5
  capture window.
- The group L message is `VK2AAL QF56 3`.

Run the receiver in one terminal:

```sh
cd ~/elec3607-lab/labs/lab5-wspr
./wsprwait
```

Run the transmitter in another terminal before the next even UTC minute:

```sh
cd ~/elec3607-lab/labs/assignment
./wspr_transmit -w -v VK2AAL QF56 3
```

The decoder line should contain:

```text
VK2AAL QF56 3
```

## RF loopback modification for the original PCB

Do not radiate this signal over the air for the assignment test.  Hardwire a
low-level RF loopback from CLK2 into the receiver input.

Recommended temporary loopback:

```text
Si5351 CLK2 / J1 pin 17
        |
       10 nF DC block
        |
      100 k series resistor
        |
        +---- to RFIN_TP7 or receiver RF input node
        |
       51 ohm shunt to GND at the receiver input
```

This attenuates the 3.3 V CMOS square wave by about:

```text
20 log10(51 / (100000 + 51)) = -65.9 dB
```

The CMOS square-wave fundamental is approximately:

```text
Vrms = 2 VDD / (pi sqrt(2)) = 1.49 Vrms for VDD = 3.3 V
```

After the attenuator this is about 0.75 mVrms, or roughly -49 dBm into
50 ohms, which is strong enough for a hardwired decode but small enough to
avoid deliberately overdriving the receiver.  If the audio waveform clips,
increase the series resistor to 220 k.

## Step-by-step validation

1. Run `./wspr_transmit -n -s VK2AAL QF56 3` and record the first 32 symbols.
2. Probe CLK2 with no loopback attached and confirm a 7.040100 MHz output.
3. Run with `-v` and confirm the four programmed RF tones are spaced by
   1.46484375 Hz.
4. Fit the RF attenuator loopback and check the receiver input level with an
   oscilloscope.  Record Vpp and convert to dBm.
5. Start the Lab 5 receiver with `../lab5-wspr/wsprwait`, then transmit with
   `./wspr_transmit -w -v VK2AAL QF56 3`.
6. Save the decoder line showing the correct callsign, grid and power.
7. Photograph the modified PCB with the CLK2-to-RFIN attenuator visible.

## Audio-loopback partial test

Before using RF, test the receiver chain with an audio WSPR file:

```sh
cd ~/elec3607-lab-main/notebook/wspr
python3 encoder_wspr.py VK2AAL QF56 3 --out vk2aal.wav
paplay vk2aal.wav &
cd ~/elec3607-lab-main/labs/lab5-wspr
./pa-wsprcan/k9an-wsprd
```

This does not prove the RF transmitter, but it proves the message encoding and
the WSPR decoder path in small steps.
