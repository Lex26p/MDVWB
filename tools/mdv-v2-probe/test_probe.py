"""Linux PTY smoke test: real serial code, independent simulated MDV v2 peer."""
import os
import pathlib
import pty
import select
import subprocess
import sys
import tempfile
import time

binary = sys.argv[1]


def checksum(data):
    return (0xFF - ((0x55 + sum(data)) & 0xFF)) & 0xFF


def reply(command, address, speed=0x55):
    data = bytearray(32)
    data[0], data[1], data[4] = 0xAA, command, address
    data[7], data[8], data[9], data[15], data[21] = speed, 21, 97, 3, 0x88
    data[30], data[31] = checksum(data[:30]), 0x55
    return data


def run(extra=(), respond=True, lose_c3=False, bad_first=False):
    with tempfile.TemporaryDirectory() as directory:
        master, slave = pty.openpty()
        proc = None
        requests = []
        try:
            logfile = pathlib.Path(directory) / "probe.log"
            with open(pathlib.Path(directory) / "stdout", "w+") as output:
                proc = subprocess.Popen([
                    binary, "--port", os.ttyname(slave), "--address", "27",
                    "--count", "2", "--period-ms", "200", "--timeout-ms", "120",
                    "--log", str(logfile), *extra], stdout=output, stderr=subprocess.STDOUT)
                pending = bytearray()
                deadline = time.monotonic() + 8
                while proc.poll() is None:
                    assert time.monotonic() < deadline, "probe hung"
                    if not select.select([master], [], [], 0.02)[0]:
                        continue
                    pending.extend(os.read(master, 4096))
                    while len(pending) >= 17:
                        wire = bytes(pending[:17])
                        del pending[:17]
                        requests.append(wire)
                        assert wire[0:2] == b"\xfe\xaa" and wire[-1] == 0x55
                        assert wire[15] == checksum(wire[1:15]), wire.hex()
                        assert wire[3] == 27
                        if not respond or (lose_c3 and wire[2] == 0xC3):
                            continue
                        frame = reply(wire[2], wire[3])
                        if bad_first:
                            corrupt = bytearray(frame)
                            corrupt[30] ^= 1
                            os.write(master, corrupt)
                            os.write(master, reply(0xD1, 27))
                            os.write(master, reply(wire[2], 26))
                        # Fragmented response including an embedded 55 payload byte.
                        os.write(master, frame[:12])
                        time.sleep(0.004)
                        os.write(master, frame[12:])
                output.seek(0)
                text = output.read()
            return proc.returncode, requests, text, logfile.read_text()
        finally:
            if proc and proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)


code, frames, text, log = run(bad_first=True)
assert code == 0, text
assert len(frames) == 2 and all(frame[2] == 0xD0 for frame in frames)
assert "room_temp=23.5" in log and "speed_raw=55" in log and "alarm_raw=3" in log
assert "crc=bad" in log and "match=no" in log

speeds = {"auto": 0x80, "1": 0x0C, "2": 0x14, "3": 0x1A,
          "4": 0x22, "5": 0x29, "6": 0x31, "7": 0x39}
for speed, raw in speeds.items():
    args = ["--speed", speed, "--power", "on", "--mode", "cool", "--temp", "21"]
    code, frames, text, log = run(args, lose_c3=True)
    assert code == 0, text
    assert [frame[2] for frame in frames] == [0xD0, 0xC3, 0xD0, 0xD0]
    assert frames[1][7:10] == bytes([0x88, raw, 21])
    assert frames[1][13:15] == bytes([0x40, 0x3C])
    assert "C3 reply absent; not retrying" in log

code, frames, text, log = run([
    "--speed", "1", "--power", "off", "--mode", "cool", "--temp", "17"])
assert code == 0, text
assert frames[1] == bytes.fromhex("FE AA C3 1B 00 80 80 00 0C 11 00 00 00 40 3C 89 55")

code, frames, text, log = run([
    "--speed", "1", "--power", "on", "--mode", "cool", "--temp", "21"], respond=False)
assert code == 2 and len(frames) == 1 and frames[0][2] == 0xD0, text
assert "C3 not sent" in log

for args in (["--speed", "8"], ["--speed", "1"], ["--address", "255"],
             ["--mode", "auto"], ["--temp", "17.5"]):
    result = subprocess.run([binary, "--port", "/does-not-exist", "--address", "27", *args],
                            capture_output=True, text=True)
    assert result.returncode == 1 and "opening serial" not in result.stderr

print("MDV v2 probe serial tests: OK")
